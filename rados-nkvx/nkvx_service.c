/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/*
 * rados-nkvx standalone executor service — Slice C2 skeleton.
 *
 * A non-SPDK Mercury TARGET that listens for the inter-tier `nkvx_exec` RPC
 * (the contract frozen in Slice C3, ../nkvx_exec_rpc.h) coming from the front
 * (rados-nkv). See docs/design/slice-c-exec-rpc-mercury.md §2/§4.
 *
 * SCOPE (C2): the service skeleton ONLY — Mercury init (listen), register the
 * RPC, a manual HG_Progress/HG_Trigger progress loop, and self-address publish
 * (design OQ-8: a file on the testbed). The RPC handler is a STUB: it decodes a
 * well-formed request and replies SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED. The real
 * fetch -> sha256-verify -> wasmtime-run -> TB4-cache pipeline (reusing the
 * existing off-reactor kvdev_rados_nkvx*.c) lands in Slice C5; this proves the
 * transport + contract round-trip end to end first.
 *
 * Build under -std=gnu11 (load-bearing for the C3 wire-status static-asserts;
 * see ../nkvx_exec_rpc.c). Built by the standalone Makefile in this directory,
 * NOT the SPDK build graph (the executor is non-SPDK by design, design §1).
 *
 * Dev rung: na+sm:// (or ofi+tcp://) loopback. Acceptance rung: ofi+verbs over
 * the E810 DAC (Slice C8) — same code, swap the --listen provider string.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <sys/queue.h>

#include <mercury.h>
#include <mercury_bulk.h>

#include "nkvx_exec_rpc.h"
#include "nkvx_executor.h"

/* Set from a signal handler to break the progress loop for an orderly exit. */
static volatile sig_atomic_t g_stop;

/* TEST-ONLY: max --mem-object seeds (slice spdk-7sr.4 / S3 loopback verify). */
#define NKVX_SERVICE_MAX_MEM_OBJS 16

/*
 * The librados-backed executor backend (Slice C5a). NULL until a --rados-pool is
 * configured; when NULL the handler keeps the C2 skeleton behaviour (decode +
 * NOT_SUPPORTED) so the transport/contract still round-trips with no cluster.
 * Single executor per process, so a file-scope handle is sufficient.
 */
static struct nkvx_executor *g_executor;

/*
 * Slice C6b cancel protocol. The executor keeps a registry of in-flight requests
 * so an incoming nkvx_cancel RPC can find the live nkvx_req by its (origin, op_id)
 * key and set do-not-PUSH / HG_Bulk_cancel the in-flight result PUSH. A linear
 * TAILQ is sufficient: concurrency is small and cancel is rare. Single progress
 * thread -> no locking (design §C6b: "races" are trigger-ordering interleavings).
 *
 * g_hg_class is cached for HG_Addr_cmp (the registry-key origin comparison).
 */
static TAILQ_HEAD(, nkvx_req) g_inflight;
static hg_class_t *g_hg_class;

/*
 * Slice C6b TEST hook (tick-deferral, NOT usleep): when NKVX_TEST_PUSH_STALL_TICKS
 * is set, the executor holds back the result PUSH for that many progress ticks so
 * a cancel deterministically lands while the request is registered but the PUSH is
 * NOT yet in flight (Case b on every transport; Case c is reached on verbs / a big
 * object). usleep would freeze the single-threaded loop and block cancel
 * processing, so the stall is a per-req countdown decremented in the progress loop;
 * the PUSH is submitted only when it reaches 0. Read once in main().
 */
static unsigned int g_test_push_stall_ticks;

/*
 * Slice C6b TEST hook (PULL side, bead spdk-8od): the symmetric tick-deferral for
 * the large-INPUT PULL. When NKVX_TEST_PULL_STALL_TICKS is set, the executor holds
 * back the input PULL for that many progress ticks so a cancel deterministically
 * lands while a large-input Exec's PULL is registered-but-not-yet-on-the-wire (or,
 * with HG_Bulk_cancel of the live PULL, mid-flight — Case c). This is the F1
 * regression guard's lever: a large-input / inline-or-small-result Exec has a live
 * input_bulk but NO result_sink, and the front must still run the cancel handshake
 * (NOT skip phase 2 on result_sink==NULL alone). Same single-threaded tick-deferral
 * as the PUSH hook — NOT usleep. Read once in main(). 0 == no-op.
 */
static unsigned int g_test_pull_stall_ticks;

static void
on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

/*
 * The `nkvx_exec` RPC handler (Slice C5a compute + Slice C7 bulk RMA).
 *
 * The flow is an async state machine because the large-payload paths interpose
 * Mercury bulk transfers that complete on later progress ticks (design §1.3):
 *
 *   Get_input
 *     -> [large input] HG_Bulk_transfer(PULL) ──cb──> on_input_pulled
 *     -> run executor (cold-fill + module)
 *     -> [large result + result_sink] HG_Bulk_transfer(PUSH) ──cb──> on_result_pushed
 *     -> HG_Respond
 *
 * NORMATIVE (design §1.3): for a large result the executor MUST await the PUSH
 * completion BEFORE HG_Respond — the response is what unblocks the front's CQE,
 * so responding early lets the tenant read a partially-written DPTR (silent
 * corruption). Here HG_Respond is chained strictly from on_result_pushed.
 *
 * Bulk RMA is a TRANSPORT concern and lives here (the handler owns the Mercury
 * handle/context/origin addr), not in the executor backend, which is delivery-
 * agnostic and just hands back a result buffer (struct nkvx_exec_result).
 *
 * Mercury ownership: HG_Get_input allocates the decoded request (strings, inline
 * input, the optional input_bulk/result_sink handles); HG_Free_input releases
 * ALL of it via the proc HG_FREE path — so the handler frees only the LOCAL bulk
 * handles it created, never in.input_bulk / in.result_sink. The handle is
 * reference-counted; HG_Destroy drops our reference once the response is queued.
 */
struct nkvx_req {
	hg_handle_t		handle;
	hg_context_t		*ctx;		/* this target's context (for bulk) */
	hg_addr_t		origin;		/* the front's address (bulk peer) */
	nkvx_exec_in_t		in;
	struct nkvx_exec_result	res;		/* delivered bytes from the backend */
	nkvx_exec_out_t		out;		/* response envelope (borrows res.buf) */
	void			*input_buf;	/* pulled large input, if any */
	hg_bulk_t		local_input;	/* local WRITE handle for the PULL */
	hg_bulk_t		local_result;	/* local READ handle for the PUSH */

	/*
	 * Slice C6b cancel protocol (design §C6b). The req is on g_inflight from
	 * insert (in nkvx_exec_handler) until nkvx_req_finish removes it.
	 */
	uint32_t		op_id;		/* cached at insert (telemetry); in is gone after HG_Free_input */
	uint64_t		call_id;	/* front-unique registry key (nkvx_exec_in_t.client_call_id) */
	bool			registered;	/* on g_inflight (idempotent reg_remove guard) */
	TAILQ_ENTRY(nkvx_req)	reg_link;	/* g_inflight linkage */

	bool			push_in_flight;	/* a result PUSH (or input PULL) bulk op is live */
	hg_op_id_t		push_op;	/* its bulk op id, for HG_Bulk_cancel */
	bool			do_not_push;	/* cancel arrived pre-PUSH: fail ABORTED, never PUSH */
	bool			had_push_canceled; /* an in-flight PUSH was HG_Bulk_cancel'd (Case c telemetry) */

	hg_handle_t		cancel_handle;	/* the pending nkvx_cancel handle to ack from the chokepoint */

	unsigned int		stall_ticks;	/* TEST: progress ticks to defer the PUSH (g_test_push_stall_ticks) */
	bool			push_pending;	/* TEST: PUSH deferred by stall_ticks, awaiting the countdown */

	unsigned int		pull_stall_ticks; /* TEST: progress ticks to defer the input PULL (g_test_pull_stall_ticks) */
	bool			pull_pending;	/* TEST: input PULL deferred by pull_stall_ticks, awaiting the countdown */
};

/*
 * ====================================================================
 * Slice C6b in-flight registry (design §C6b). The (origin_addr, op_id) key, not
 * op_id alone: op_id is the tenant CDW13 and is not unique across fronts; a single
 * executor serves several. Single progress thread -> no locking.
 * ====================================================================
 */

/* Register req as in-flight so a cancel can find it. Idempotent (registered guard). */
static void
nkvx_reg_insert(struct nkvx_req *req)
{
	if (req->registered) {
		return;
	}
	TAILQ_INSERT_TAIL(&g_inflight, req, reg_link);
	req->registered = true;
}

/* Remove req from the in-flight registry. Idempotent — safe to call more than once
 * (the ack chokepoint removes FIRST, before any later teardown step). */
static void
nkvx_reg_remove(struct nkvx_req *req)
{
	if (!req->registered) {
		return;
	}
	TAILQ_REMOVE(&g_inflight, req, reg_link);
	req->registered = false;
}

/* Find the in-flight req matching (origin == addr, call_id), or NULL. call_id is
 * the front-UNIQUE handshake token (not op_id, the tenant CDW13, which is not
 * unique among a front's concurrent Execs — keying on it could abort the wrong
 * Exec). Origin match (HG_Addr_cmp) additionally scopes it across fronts. */
static struct nkvx_req *
nkvx_reg_lookup(hg_addr_t addr, uint64_t call_id)
{
	struct nkvx_req *req;

	TAILQ_FOREACH(req, &g_inflight, reg_link) {
		if (req->call_id == call_id &&
		    HG_Addr_cmp(g_hg_class, req->origin, addr) == HG_TRUE) {
			return req;
		}
	}
	return NULL;
}

/*
 * Respond with req->out (already populated), then tear the request down. This is
 * the Slice C6b ACK CHOKEPOINT — the single place a request leaves the in-flight
 * set, and where a pending nkvx_cancel is acked. The step ORDER is load-bearing
 * (design §C6b):
 *
 *   (1) reg_remove FIRST — closes the "responded but still registered" window so
 *       any cancel that arrives from here on is Case (a) ALREADY_DONE, not a
 *       cancel of a req we are already finishing.
 *   (2) HG_Respond the Exec — unblocks the front's CQE.
 *   (3) free local_result / local_input — THE QUIESCENCE POINT: the executor's
 *       view of the front's result_sink (and input_bulk) is now gone; no PUSH can
 *       land after this. ONLY then is it safe to tell the front via the ack.
 *   (4) if a cancel is pending, ack it (PUSH_CANCELED if a PUSH was actually
 *       cancelled, else ABORTED) and destroy its handle — the front releases the
 *       DPTR off the back of this ack.
 *   (5) free input / handle / req as before.
 */
static void
nkvx_req_finish(struct nkvx_req *req)
{
	hg_return_t ret;

	/* (1) Leave the registry before responding (close the Case-a window). */
	nkvx_reg_remove(req);

	/* (2) Respond to the Exec. */
	ret = HG_Respond(req->handle, NULL, NULL, &req->out);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: HG_Respond failed: %s\n",
			HG_Error_to_string(ret));
	}

	/* (3) HG_Respond encoded the response synchronously, so everything it borrowed
	 * (req->out.result_inline points into req->res.buf) can be released now.
	 * Freeing local_result/local_input is the quiescence point for the cancel ack:
	 * the executor's remote bulk view is gone, so no PUSH can land hereafter. */
	if (req->local_result != HG_BULK_NULL) {
		HG_Bulk_free(req->local_result);
	}
	if (req->local_input != HG_BULK_NULL) {
		HG_Bulk_free(req->local_input);
	}
	nkvx_exec_result_free(&req->res);

	/* (4) Ack any pending cancel — POST-quiesce, so the ack is the front's positive
	 * proof that no PUSH can land and it may release the result_sink / DPTR. */
	if (req->cancel_handle != HG_HANDLE_NULL) {
		nkvx_cancel_out_t cout;

		cout.ack = req->had_push_canceled ? NKVX_CANCEL_PUSH_CANCELED
						  : NKVX_CANCEL_ABORTED;
		fprintf(stderr,
			"nkvx_service: cancel ack op_id=%u call_id=%lu case=%c (no PUSH after ack)\n",
			req->op_id, (unsigned long)req->call_id,
			req->had_push_canceled ? 'c' : 'b');
		ret = HG_Respond(req->cancel_handle, NULL, NULL, &cout);
		if (ret != HG_SUCCESS) {
			fprintf(stderr, "nkvx_service: cancel HG_Respond failed: %s\n",
				HG_Error_to_string(ret));
		}
		HG_Destroy(req->cancel_handle);
		req->cancel_handle = HG_HANDLE_NULL;
	}

	/* (5) Free input / handle / req. */
	if (req->input_buf != NULL) {
		/* The large-input path repointed in.input_inline at our pulled buffer;
		 * detach it so HG_Free_input frees only Mercury-owned memory, then we
		 * free input_buf ourselves below. On the inline path input_buf is NULL
		 * and in.input_inline is the proc-decoded buffer HG_Free_input must free. */
		req->in.input_inline = NULL;
	}
	free(req->input_buf);	/* free(NULL) is fine on the inline path */
	HG_Free_input(req->handle, &req->in);
	HG_Destroy(req->handle);
	free(req);
}

/* Populate the response envelope with a terminal status (no result body) and
 * respond + tear down. Collapses the handler's many identical failure tails. */
static void
nkvx_req_fail(struct nkvx_req *req, enum spdk_kvdev_io_status status)
{
	req->out.status = nkvx_status_to_wire(status);
	req->out.result_len = 0;
	req->out.result_inline = NULL;
	req->out.result_inline_len = 0;
	nkvx_req_finish(req);
}

/*
 * PUSH-completion callback (NORMATIVE chain point): the large result has fully
 * landed in the front's result_sink. Only NOW is it safe to respond.
 */
static hg_return_t
nkvx_result_pushed_cb(const struct hg_cb_info *info)
{
	struct nkvx_req *req = info->arg;

	/* The bulk op is no longer live (cancelled or completed); its op id is stale. */
	req->push_in_flight = false;

	if (info->ret == HG_CANCELED) {
		/*
		 * Slice C6b Case (c): an nkvx_cancel HG_Bulk_cancel'd this PUSH. The
		 * remote view is being torn down; finish ABORTED — routed through the ack
		 * chokepoint, which acks the cancel POST-quiesce (PUSH_CANCELED). No
		 * success CQE over a torn DPTR. */
		fprintf(stderr, "nkvx_service: result PUSH canceled op_id=%u (case c) "
			"— PUSH skipped/aborted, no late PUSH\n", req->op_id);
		req->had_push_canceled = true;
		nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_ABORTED);
		return HG_SUCCESS;
	}
	if (info->ret != HG_SUCCESS) {
		/* The bytes did not (fully) land: do NOT report success — the front
		 * would fire a CQE over a torn DPTR. Synthesize FAILED. */
		fprintf(stderr, "nkvx_service: result PUSH failed: %s\n",
			HG_Error_to_string(info->ret));
		nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
		return HG_SUCCESS;
	}
	req->out.status = nkvx_status_to_wire(req->res.status);
	req->out.result_len = req->res.result_len;
	req->out.result_inline = NULL;		/* delivered via the sink, not inline */
	req->out.result_inline_len = 0;
	nkvx_req_finish(req);
	return HG_SUCCESS;
}

/*
 * Submit the result PUSH into the front's result_sink (design §1.3). Factored out
 * of nkvx_req_run_and_deliver so the Slice C6b TEST stall hook can defer the
 * actual transfer by a few progress ticks while keeping the do-not-PUSH gate and
 * the op-id capture in ONE place. Responds from nkvx_result_pushed_cb (NORMATIVE).
 */
static void
nkvx_submit_result_push(struct nkvx_req *req)
{
	hg_class_t *cls = HG_Get_info(req->handle)->hg_class;
	void *p = req->res.buf;
	hg_size_t sz = req->res.buf_len;
	hg_return_t ret;

	/*
	 * Slice C6b do-not-PUSH gate (design §C6b): re-checked here (not just before
	 * run) because a cancel may have arrived during the run/PULL/stall. If set, the
	 * front has stopped waiting; never PUSH into a DPTR it may be reusing — finish
	 * ABORTED (routed through the ack chokepoint, which acks the pending cancel). */
	if (req->do_not_push) {
		fprintf(stderr, "nkvx_service: op_id=%u do_not_push set (case b) "
			"— PUSH skipped, no late PUSH\n", req->op_id);
		nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_ABORTED);
		return;
	}

	ret = HG_Bulk_create(cls, 1, &p, &sz, HG_BULK_READ_ONLY, &req->local_result);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: HG_Bulk_create(result) failed: %s\n",
			HG_Error_to_string(ret));
		nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
		return;
	}
	/* Capture the bulk op id (&req->push_op) so a cancel can HG_Bulk_cancel it;
	 * mark in-flight BEFORE the transfer (the cb clears it). */
	req->push_in_flight = true;
	ret = HG_Bulk_transfer(req->ctx, nkvx_result_pushed_cb, req,
			       HG_BULK_PUSH, req->origin, req->in.result_sink, 0,
			       req->local_result, 0, sz, &req->push_op);
	if (ret != HG_SUCCESS) {
		req->push_in_flight = false;
		fprintf(stderr, "nkvx_service: HG_Bulk_transfer(PUSH) failed: %s\n",
			HG_Error_to_string(ret));
		nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
		return;
	}
	/* respond from nkvx_result_pushed_cb (NORMATIVE) */
}

/*
 * Run the module, then deliver the result: inline in the response when it is
 * small (or no sink was offered), else PUSH it into the front's WRITE-registered
 * result_sink and respond only from the PUSH-completion callback.
 */
static void
nkvx_req_run_and_deliver(struct nkvx_req *req)
{
	nkvx_executor_run(g_executor, &req->in, &req->res);
	fprintf(stderr,
		"nkvx_service: nkvx_exec op_id=%u key_len=%u runtime=%u module_key=%s "
		"module_ns=%s -> status=%d result_len=%u deliver=%u%s\n",
		req->in.op_id, req->in.key_len, req->in.runtime,
		req->in.module_key ? req->in.module_key : "(null)",
		req->in.module_ns ? req->in.module_ns : "(null)",
		req->res.status, req->res.result_len, req->res.buf_len,
		(req->in.result_sink != HG_BULK_NULL) ? " sink" : "");

	/* Large result + a sink to push into: front-sink/executor-push (design §1.3). */
	if (req->in.result_sink != HG_BULK_NULL && req->res.buf_len > NKVX_INLINE_MAX) {
		/*
		 * Slice C6b do-not-PUSH gate (design §C6b): a cancel may have set this
		 * during the run / input PULL. Honor it before touching the sink. */
		if (req->do_not_push) {
			fprintf(stderr, "nkvx_service: op_id=%u do_not_push set (case b) "
				"— PUSH skipped, no late PUSH\n", req->op_id);
			nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_ABORTED);
			return;
		}
		/*
		 * Slice C6b TEST stall hook (tick-deferral): hold the PUSH back for
		 * stall_ticks progress ticks so a cancel deterministically lands while the
		 * req is registered but the PUSH is not yet on the wire (Case b). The
		 * progress loop decrements the countdown and submits at 0. */
		if (req->stall_ticks > 0) {
			req->push_pending = true;
			fprintf(stderr, "nkvx_service: op_id=%u PUSH stalled %u tick(s) (test)\n",
				req->op_id, req->stall_ticks);
			return;
		}
		nkvx_submit_result_push(req);
		return;
	}

	/* Inline path. A large result with NO sink offered cannot be delivered — the
	 * front did not register a DPTR to push into; decline cleanly (design §1.3).
	 *
	 * This branch is currently UNREACHABLE: the front registers a result_sink
	 * whenever osize > NKVX_INLINE_MAX, and the executor caps buf_len =
	 * min(result_len, osize) <= osize, so buf_len > NKVX_INLINE_MAX implies a
	 * sink was registered (and the PUSH path above was taken). It is retained as
	 * a defensive guard for when the C7.2 MR/handle cache makes sink registration
	 * conditional — the two thresholds (the front's osize-based sink offer and the
	 * executor's buf_len-based push) must stay in agreement. Do not delete. */
	if (req->res.buf_len > NKVX_INLINE_MAX) {
		fprintf(stderr, "nkvx_service: result %u > inline max %u and no result_sink "
			"— NOT_SUPPORTED\n", req->res.buf_len, NKVX_INLINE_MAX);
		req->out.status = nkvx_status_to_wire(SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED);
		req->out.result_len = 0;
		req->out.result_inline = NULL;
		req->out.result_inline_len = 0;
	} else {
		req->out.status = nkvx_status_to_wire(req->res.status);
		req->out.result_len = req->res.result_len;
		req->out.result_inline = req->res.buf;		/* borrowed; freed via res */
		req->out.result_inline_len = req->res.buf_len;
	}
	nkvx_req_finish(req);
}

/*
 * Large-input PULL completion: the request input has been read into input_buf.
 * Point the backend at it and proceed to run.
 */
static hg_return_t
nkvx_input_pulled_cb(const struct hg_cb_info *info)
{
	struct nkvx_req *req = info->arg;

	/* The PULL bulk op is no longer live; its op id is stale (Slice C6b). */
	req->push_in_flight = false;

	if (info->ret == HG_CANCELED) {
		/* A cancel during the input PULL (Slice C6b): finish ABORTED through the
		 * ack chokepoint, which acks the pending cancel post-quiesce. */
		fprintf(stderr, "nkvx_service: input PULL canceled op_id=%u (case c) "
			"— aborted before run, no late PUSH\n", req->op_id);
		req->had_push_canceled = true;
		nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_ABORTED);
		return HG_SUCCESS;
	}
	if (info->ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: input PULL failed: %s\n",
			HG_Error_to_string(info->ret));
		nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
		return HG_SUCCESS;
	}
	req->in.input_inline = req->input_buf;	/* backend reads input_inline */
	nkvx_req_run_and_deliver(req);
	return HG_SUCCESS;
}

/*
 * Submit the large-input PULL from the front's READ-registered input_bulk (design
 * §1.3). Factored out of nkvx_exec_handler so the bead spdk-8od TEST PULL stall hook
 * can defer the actual transfer by a few progress ticks while keeping the do-not-PUSH
 * gate and the op-id capture in ONE place — exactly mirroring nkvx_submit_result_push
 * on the PUSH side. Continues from nkvx_input_pulled_cb. Returns 0 on success or
 * -1 if the request was already terminated (failed) here.
 */
static int
nkvx_submit_input_pull(struct nkvx_req *req)
{
	hg_class_t *cls = HG_Get_info(req->handle)->hg_class;
	hg_size_t sz = req->in.input_len;
	void *p;
	hg_return_t ret;

	/*
	 * Slice C6b do-not-PUSH gate (bead spdk-8od): re-checked here because a cancel
	 * may have arrived during the stall, before the PULL ever went on the wire. The
	 * front has stopped waiting and is about to release the input MR; never PULL
	 * from a buffer it may be reusing — finish ABORTED (routed through the ack
	 * chokepoint, which acks the pending cancel). This is the PULL-side analogue of
	 * the PUSH do-not-PUSH gate. */
	if (req->do_not_push) {
		fprintf(stderr, "nkvx_service: op_id=%u do_not_push set (case b) "
			"— input PULL skipped, no late PULL\n", req->op_id);
		nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_ABORTED);
		return -1;
	}

	req->input_buf = malloc(req->in.input_len);
	if (req->input_buf == NULL) {
		nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_NOMEM);
		return -1;
	}
	p = req->input_buf;
	ret = HG_Bulk_create(cls, 1, &p, &sz, HG_BULK_WRITE_ONLY, &req->local_input);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: HG_Bulk_create(input) failed: %s\n",
			HG_Error_to_string(ret));
		nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
		return -1;
	}
	/* Slice C6b: capture the PULL op id (&req->push_op) and mark in-flight so a
	 * cancel during the input PULL HG_Bulk_cancels it for prompt/symmetric
	 * teardown (the cb clears push_in_flight). */
	req->push_in_flight = true;
	ret = HG_Bulk_transfer(req->ctx, nkvx_input_pulled_cb, req,
			       HG_BULK_PULL, req->origin, req->in.input_bulk, 0,
			       req->local_input, 0, sz, &req->push_op);
	if (ret != HG_SUCCESS) {
		req->push_in_flight = false;
		fprintf(stderr, "nkvx_service: HG_Bulk_transfer(PULL) failed: %s\n",
			HG_Error_to_string(ret));
		nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
		return -1;
	}
	return 0;	/* continue from nkvx_input_pulled_cb */
}

static hg_return_t
nkvx_exec_handler(hg_handle_t handle)
{
	struct nkvx_req *req;
	const struct hg_info *info;
	hg_return_t ret;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		fprintf(stderr, "nkvx_service: out of memory for request\n");
		HG_Destroy(handle);
		return HG_NOMEM;
	}
	req->handle = handle;
	req->local_input = HG_BULK_NULL;
	req->local_result = HG_BULK_NULL;

	ret = HG_Get_input(handle, &req->in);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: HG_Get_input failed: %s\n",
			HG_Error_to_string(ret));
		/* Cannot form a meaningful reply without the request; drop it. */
		free(req);
		HG_Destroy(handle);
		return ret;
	}

	info = HG_Get_info(handle);
	req->ctx = info->context;
	req->origin = info->addr;

	/*
	 * Slice C6b: register the req as in-flight so an incoming nkvx_cancel can find
	 * it by (origin, call_id). Cache call_id (the front-unique key) and op_id (for
	 * telemetry) NOW — req->in is freed by HG_Free_input in nkvx_req_finish, so the
	 * registry must never deref `in` after that. The cancel_handle is NULL until a
	 * cancel stashes it. (Not done on the HG_Get_input-fail path above.) */
	req->op_id = req->in.op_id;
	req->call_id = req->in.client_call_id;
	req->cancel_handle = HG_HANDLE_NULL;
	req->push_op = HG_OP_ID_NULL;
	req->stall_ticks = g_test_push_stall_ticks;
	req->pull_stall_ticks = g_test_pull_stall_ticks;
	nkvx_reg_insert(req);

	if (g_executor == NULL) {
		/*
		 * No cluster configured (C2 skeleton mode): decode + decline so the
		 * transport/contract still round-trips with no RADOS. NOT_SUPPORTED
		 * maps at the tenant to INVALID_OPCODE (design §3).
		 */
		fprintf(stderr,
			"nkvx_service: nkvx_exec op_id=%u key_len=%u runtime=%u "
			"module_key=%s module_ns=%s -> NOT_SUPPORTED (no cluster)\n",
			req->in.op_id, req->in.key_len, req->in.runtime,
			req->in.module_key ? req->in.module_key : "(null)",
			req->in.module_ns ? req->in.module_ns : "(null)");
		nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED);
		return HG_SUCCESS;
	}

	/*
	 * Large input (rare, design §1.3): pull it from the front's READ-registered
	 * input_bulk into a local buffer BEFORE running. Small input rode inline.
	 */
	if (req->in.input_bulk != HG_BULK_NULL && req->in.input_len > NKVX_INLINE_MAX) {
		/*
		 * Bead spdk-8od TEST PULL stall hook (tick-deferral): hold the input PULL
		 * back for pull_stall_ticks progress ticks so a cancel deterministically
		 * lands while the req is registered but the PULL is not yet on the wire
		 * (Case b — do_not_push). The progress loop decrements the countdown and
		 * submits at 0 (or skips it if a cancel set do_not_push meanwhile). This is
		 * the F1 regression lever: a large-input / inline-result Exec has a live
		 * input_bulk but NO result_sink, so the front MUST still run the cancel
		 * handshake. A no-op unless NKVX_TEST_PULL_STALL_TICKS was set. */
		if (req->pull_stall_ticks > 0) {
			req->pull_pending = true;
			fprintf(stderr, "nkvx_service: op_id=%u input PULL stalled %u tick(s) (test)\n",
				req->op_id, req->pull_stall_ticks);
			return HG_SUCCESS;	/* continue from the stall-tick countdown */
		}
		(void)nkvx_submit_input_pull(req);
		return HG_SUCCESS;	/* continue from nkvx_input_pulled_cb (or already failed) */
	}

	nkvx_req_run_and_deliver(req);
	return HG_SUCCESS;
}

/*
 * The `nkvx_cancel` RPC handler (Slice C6b, design §C6b). The front forwards this
 * to make a cross-process abort use-after-free-safe. Look up the in-flight req by
 * (origin, op_id) and take one of three cases:
 *
 *   (a) NOT found       -> the Exec already finished (or unknown op_id, or the
 *                          second of a double-cancel). Nothing to abort and the
 *                          executor's remote view is already gone, so ack
 *                          ALREADY_DONE immediately + Destroy here.
 *   (b) found, no PUSH   -> set do_not_push and stash cancel_handle on the req. The
 *       in flight          do-not-PUSH gate finishes it ABORTED and the ack
 *                          chokepoint (nkvx_req_finish) sends the ABORTED ack.
 *   (c) found, PUSH      -> stash cancel_handle and HG_Bulk_cancel the in-flight
 *       in flight          PUSH (or input PULL). Its cb fires HG_CANCELED -> finish
 *                          ABORTED -> the chokepoint acks PUSH_CANCELED. If the
 *                          PUSH already completed (HG_SUCCESS but cancel_handle
 *                          set), the chokepoint still acks — memory-safe because the
 *                          front deferred its DPTR release until this ack.
 *
 * Only op_id is on the wire; origin rides via HG_Get_info(handle)->addr. The ack
 * value is telemetry only — the DELIVERED ack is the front's safety proof.
 */
static hg_return_t
nkvx_cancel_handler(hg_handle_t handle)
{
	const struct hg_info *info = HG_Get_info(handle);
	nkvx_cancel_in_t cin;
	nkvx_cancel_out_t cout;
	struct nkvx_req *req;
	uint64_t call_id;
	hg_return_t ret;

	ret = HG_Get_input(handle, &cin);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: cancel HG_Get_input failed: %s\n",
			HG_Error_to_string(ret));
		HG_Destroy(handle);
		return ret;
	}

	call_id = cin.call_id;		/* cache before HG_Free_input (don't deref cin after) */
	req = nkvx_reg_lookup(info->addr, call_id);
	HG_Free_input(handle, &cin);	/* scalar; nothing allocated, but symmetric */

	if (req == NULL) {
		/* (a) already finished / unknown / second-of-double-cancel. */
		fprintf(stderr, "nkvx_service: cancel call_id=%lu case=a (already done) "
			"— no PUSH after ack\n", (unsigned long)call_id);
		cout.ack = NKVX_CANCEL_ALREADY_DONE;
		ret = HG_Respond(handle, NULL, NULL, &cout);
		if (ret != HG_SUCCESS) {
			fprintf(stderr, "nkvx_service: cancel HG_Respond failed: %s\n",
				HG_Error_to_string(ret));
		}
		HG_Destroy(handle);
		return HG_SUCCESS;
	}

	if (req->cancel_handle != HG_HANDLE_NULL) {
		/*
		 * A cancel is already pending on this req (duplicate). Don't overwrite the
		 * stashed handle (that would leak it and lose its ack); ack the duplicate
		 * ALREADY_DONE now — the original cancel's ack still proves quiescence. */
		fprintf(stderr, "nkvx_service: cancel op_id=%u case=a (duplicate) "
			"— no PUSH after ack\n", req->op_id);
		cout.ack = NKVX_CANCEL_ALREADY_DONE;
		ret = HG_Respond(handle, NULL, NULL, &cout);
		if (ret != HG_SUCCESS) {
			fprintf(stderr, "nkvx_service: cancel HG_Respond failed: %s\n",
				HG_Error_to_string(ret));
		}
		HG_Destroy(handle);
		return HG_SUCCESS;
	}

	/* Stash the cancel handle; it is acked from the ack chokepoint post-quiesce. */
	req->cancel_handle = handle;

	if (req->push_in_flight) {
		/* (c) A bulk op (result PUSH or input PULL) is on the wire: cancel it. The
		 * cb resolves HG_CANCELED (or HG_SUCCESS if it raced to completion) and
		 * routes to nkvx_req_finish, which acks post-quiesce. */
		fprintf(stderr, "nkvx_service: cancel op_id=%u call_id=%lu case=c (PUSH in flight) "
			"— HG_Bulk_cancel\n", req->op_id, (unsigned long)req->call_id);
		ret = HG_Bulk_cancel(req->push_op);
		if (ret != HG_SUCCESS) {
			/* The cancel could not be issued; the bulk cb will still fire
			 * eventually (HG_SUCCESS) and ack from the chokepoint. Also set
			 * do_not_push so any not-yet-submitted PUSH (e.g. a stalled one whose
			 * countdown just expired) is skipped. */
			fprintf(stderr, "nkvx_service: HG_Bulk_cancel failed: %s\n",
				HG_Error_to_string(ret));
			req->do_not_push = true;
		}
	} else {
		/* (b) No bulk op in flight (pre-PUSH, mid-run, or stalled): mark
		 * do-not-PUSH. The gate in run_and_deliver / nkvx_submit_result_push (or a
		 * stalled PUSH whose countdown expires) finishes it ABORTED, and the
		 * chokepoint sends the ABORTED ack. */
		fprintf(stderr, "nkvx_service: cancel op_id=%u call_id=%lu case=b (no PUSH in flight) "
			"— do_not_push\n", req->op_id, (unsigned long)req->call_id);
		req->do_not_push = true;
	}
	return HG_SUCCESS;
}

/*
 * ====================================================================
 * Base-layer KV verb handler (slice spdk-7sr.4 / S3): the `nkvx_kv` RPC.
 *
 * Additive to the frozen nkvx_exec contract (registered under a distinct name, like
 * nkvx_cancel), so it does not perturb the Exec wire id or envelope. S3 implements
 * the RETRIEVE verb end-to-end; Store/Delete/Exist/List land in S4.
 *
 * Delivery mirrors the Exec inline-vs-PUSH rule (design §1.3): a small value rides
 * inline in nkvx_kv_out_t; a large value (> NKVX_INLINE_MAX) the front offered a
 * result_sink for is PUSHed straight into the front's host DPTR, and the response is
 * sent only AFTER the PUSH completes (NORMATIVE — responding early would let the
 * tenant read a partially-written DPTR). The Slice C6b cross-process cancel handshake
 * is NOT wired for base verbs in S3 (Retrieve is short and non-mutating); per-verb
 * cancel is a later concern.
 * ====================================================================
 */
struct nkvx_kv_req {
	hg_handle_t		handle;
	hg_context_t		*ctx;
	hg_addr_t		origin;
	nkvx_kv_in_t		in;
	struct nkvx_exec_result	res;		/* delivered bytes from the backend */
	nkvx_kv_out_t		out;		/* response envelope (borrows res.buf) */
	void			*value_buf;	/* pulled large STORE value, if any */
	hg_bulk_t		local_value;	/* local WRITE handle for the value PULL */
	hg_bulk_t		local_result;	/* local READ handle for the PUSH */
};

/* Respond with req->out (already populated) and tear the KV request down. */
static void
nkvx_kv_req_finish(struct nkvx_kv_req *req)
{
	hg_return_t ret;

	ret = HG_Respond(req->handle, NULL, NULL, &req->out);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: kv HG_Respond failed: %s\n",
			HG_Error_to_string(ret));
	}
	if (req->local_result != HG_BULK_NULL) {
		HG_Bulk_free(req->local_result);
	}
	if (req->local_value != HG_BULK_NULL) {
		HG_Bulk_free(req->local_value);
	}
	nkvx_exec_result_free(&req->res);
	if (req->value_buf != NULL) {
		/* The large-value PULL repointed in.value_inline at our pulled buffer;
		 * detach it so HG_Free_input frees only Mercury-owned memory, then we free
		 * value_buf ourselves below. On the inline path value_buf is NULL and
		 * in.value_inline is the proc-decoded buffer HG_Free_input must free. */
		req->in.value_inline = NULL;
	}
	free(req->value_buf);	/* free(NULL) is fine on the inline path */
	HG_Free_input(req->handle, &req->in);
	HG_Destroy(req->handle);
	free(req);
}

static void
nkvx_kv_req_fail(struct nkvx_kv_req *req, enum spdk_kvdev_io_status status)
{
	req->out.status = nkvx_status_to_wire(status);
	req->out.result_len = 0;
	req->out.result_inline = NULL;
	req->out.result_inline_len = 0;
	nkvx_kv_req_finish(req);
}

/* PUSH-completion callback: the value has fully landed in the front's result_sink;
 * only NOW respond (NORMATIVE PUSH-before-Respond, design §1.3). */
static hg_return_t
nkvx_kv_result_pushed_cb(const struct hg_cb_info *info)
{
	struct nkvx_kv_req *req = info->arg;

	if (info->ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: kv result PUSH failed: %s\n",
			HG_Error_to_string(info->ret));
		nkvx_kv_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
		return HG_SUCCESS;
	}
	req->out.status = nkvx_status_to_wire(req->res.status);
	req->out.result_len = req->res.result_len;	/* TRUE length -> CQE DW0 */
	req->out.result_inline = NULL;			/* delivered via the sink */
	req->out.result_inline_len = 0;
	nkvx_kv_req_finish(req);
	return HG_SUCCESS;
}

/*
 * Run the verb against the backend and deliver the response. The STORE value (large
 * → pulled into req->in.value_inline by the value PULL; small → rode inline) and all
 * other inputs are in place by now. Retrieve/List large values are PUSHed into the
 * front's result_sink and the response is sent only from the PUSH-completion cb
 * (NORMATIVE); everything else responds inline here. Shared by the inline entry and
 * the post-value-PULL continuation.
 */
static void
nkvx_kv_run_and_deliver(struct nkvx_kv_req *req)
{
	hg_return_t ret;

	nkvx_executor_kv(g_executor, &req->in, &req->res);
	fprintf(stderr,
		"nkvx_service: nkvx_kv verb=%u key_len=%u osize=%u -> status=%d "
		"result_len=%u deliver=%u%s\n",
		req->in.verb, req->in.key_len, req->in.osize, req->res.status,
		req->res.result_len, req->res.buf_len,
		(req->in.result_sink != HG_BULK_NULL) ? " sink" : "");

	/* Large value + a sink to push into: front-sink/executor-push (design §1.3). */
	if (req->in.result_sink != HG_BULK_NULL && req->res.buf_len > NKVX_INLINE_MAX) {
		hg_class_t *cls = HG_Get_info(req->handle)->hg_class;
		void *p = req->res.buf;
		hg_size_t sz = req->res.buf_len;

		ret = HG_Bulk_create(cls, 1, &p, &sz, HG_BULK_READ_ONLY, &req->local_result);
		if (ret != HG_SUCCESS) {
			fprintf(stderr, "nkvx_service: kv HG_Bulk_create(result) failed: %s\n",
				HG_Error_to_string(ret));
			nkvx_kv_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
			return;
		}
		ret = HG_Bulk_transfer(req->ctx, nkvx_kv_result_pushed_cb, req,
				       HG_BULK_PUSH, req->origin, req->in.result_sink, 0,
				       req->local_result, 0, sz, HG_OP_ID_IGNORE);
		if (ret != HG_SUCCESS) {
			fprintf(stderr, "nkvx_service: kv HG_Bulk_transfer(PUSH) failed: %s\n",
				HG_Error_to_string(ret));
			nkvx_kv_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
			return;
		}
		return;	/* respond from nkvx_kv_result_pushed_cb (NORMATIVE) */
	}

	/* Inline path. A large value with no sink offered cannot be delivered (mirrors
	 * the Exec guard); decline cleanly. */
	if (req->res.buf_len > NKVX_INLINE_MAX) {
		fprintf(stderr, "nkvx_service: kv value %u > inline max %u and no result_sink "
			"— NOT_SUPPORTED\n", req->res.buf_len, NKVX_INLINE_MAX);
		nkvx_kv_req_fail(req, SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED);
		return;
	}
	req->out.status = nkvx_status_to_wire(req->res.status);
	req->out.result_len = req->res.result_len;		/* TRUE length -> CQE DW0 */
	req->out.result_inline = req->res.buf;			/* borrowed; freed via res */
	req->out.result_inline_len = req->res.buf_len;
	nkvx_kv_req_finish(req);
}

/*
 * Large-STORE-value PULL completion: the value has been read into value_buf. Point the
 * backend store at it (via value_inline) and run the verb. Mirrors nkvx_input_pulled_cb.
 */
static hg_return_t
nkvx_kv_value_pulled_cb(const struct hg_cb_info *info)
{
	struct nkvx_kv_req *req = info->arg;

	if (info->ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: kv value PULL failed: %s\n",
			HG_Error_to_string(info->ret));
		nkvx_kv_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
		return HG_SUCCESS;
	}
	req->in.value_inline = req->value_buf;	/* backend store reads value_inline */
	nkvx_kv_run_and_deliver(req);
	return HG_SUCCESS;
}

/*
 * Submit the large-STORE-value PULL from the front's READ-registered value_bulk (the
 * input-PULL analogue of the Exec input PULL, nkvx_submit_input_pull). Continues from
 * nkvx_kv_value_pulled_cb. The base-verb path has no Slice C6b cancel handshake (S3),
 * so the PULL op id is ignored (like the KV result PUSH). Terminates the request and
 * returns -1 on a synchronous failure, else 0.
 */
static int
nkvx_kv_submit_value_pull(struct nkvx_kv_req *req)
{
	hg_class_t *cls = HG_Get_info(req->handle)->hg_class;
	hg_size_t sz = req->in.value_len;
	void *p;
	hg_return_t ret;

	req->value_buf = malloc(req->in.value_len);
	if (req->value_buf == NULL) {
		nkvx_kv_req_fail(req, SPDK_KVDEV_IO_STATUS_NOMEM);
		return -1;
	}
	p = req->value_buf;
	ret = HG_Bulk_create(cls, 1, &p, &sz, HG_BULK_WRITE_ONLY, &req->local_value);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: kv HG_Bulk_create(value) failed: %s\n",
			HG_Error_to_string(ret));
		nkvx_kv_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
		return -1;
	}
	ret = HG_Bulk_transfer(req->ctx, nkvx_kv_value_pulled_cb, req,
			       HG_BULK_PULL, req->origin, req->in.value_bulk, 0,
			       req->local_value, 0, sz, HG_OP_ID_IGNORE);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: kv HG_Bulk_transfer(PULL) failed: %s\n",
			HG_Error_to_string(ret));
		nkvx_kv_req_fail(req, SPDK_KVDEV_IO_STATUS_FAILED);
		return -1;
	}
	return 0;	/* continue from nkvx_kv_value_pulled_cb */
}

static hg_return_t
nkvx_kv_handler(hg_handle_t handle)
{
	struct nkvx_kv_req *req;
	const struct hg_info *info;
	hg_return_t ret;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		fprintf(stderr, "nkvx_service: out of memory for kv request\n");
		HG_Destroy(handle);
		return HG_NOMEM;
	}
	req->handle = handle;
	req->local_result = HG_BULK_NULL;
	req->local_value = HG_BULK_NULL;

	ret = HG_Get_input(handle, &req->in);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: kv HG_Get_input failed: %s\n",
			HG_Error_to_string(ret));
		free(req);
		HG_Destroy(handle);
		return ret;
	}

	info = HG_Get_info(handle);
	req->ctx = info->context;
	req->origin = info->addr;

	if (g_executor == NULL) {
		/* C2 skeleton mode (no cluster): decode + decline so the transport/contract
		 * still round-trips with no RADOS. */
		fprintf(stderr, "nkvx_service: nkvx_kv verb=%u key_len=%u -> NOT_SUPPORTED "
			"(no cluster)\n", req->in.verb, req->in.key_len);
		nkvx_kv_req_fail(req, SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED);
		return HG_SUCCESS;
	}

	/*
	 * A large STORE value is delivered out-of-band: PULL it from the front's READ-
	 * registered value_bulk into a local buffer BEFORE running the store (mirrors the
	 * Exec large-input PULL). A small value rode inline in value_inline. The PULL
	 * continues at nkvx_kv_value_pulled_cb, which then runs nkvx_kv_run_and_deliver.
	 */
	if (req->in.value_bulk != HG_BULK_NULL && req->in.value_len > NKVX_INLINE_MAX) {
		nkvx_kv_submit_value_pull(req);	/* continues from nkvx_kv_value_pulled_cb */
		return HG_SUCCESS;
	}

	nkvx_kv_run_and_deliver(req);
	return HG_SUCCESS;
}

/*
 * Publish the target's self-address string so the front can HG_Addr_lookup it
 * (design OQ-8 bootstrap = a file on the testbed). Written atomically (temp +
 * rename) so a reader never observes a half-written address. Also echoed to
 * stdout for interactive/`--addr-file -` use.
 */
static int
publish_self_addr(hg_class_t *hg, const char *addr_file)
{
	hg_addr_t self = HG_ADDR_NULL;
	char addr_str[512];
	hg_size_t addr_len = sizeof(addr_str);
	hg_return_t ret;

	ret = HG_Addr_self(hg, &self);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: HG_Addr_self failed: %s\n",
			HG_Error_to_string(ret));
		return -1;
	}
	ret = HG_Addr_to_string(hg, addr_str, &addr_len, self);
	HG_Addr_free(hg, self);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_service: HG_Addr_to_string failed: %s\n",
			HG_Error_to_string(ret));
		return -1;
	}

	printf("nkvx_service: listening, self addr = %s\n", addr_str);
	fflush(stdout);

	if (addr_file == NULL) {
		return 0;
	}

	char tmp[4096];
	int n = snprintf(tmp, sizeof(tmp), "%s.tmp.%d", addr_file, (int)getpid());
	if (n < 0 || (size_t)n >= sizeof(tmp)) {
		fprintf(stderr, "nkvx_service: addr-file path too long\n");
		return -1;
	}

	FILE *f = fopen(tmp, "w");
	if (f == NULL) {
		fprintf(stderr, "nkvx_service: fopen(%s) failed: %s\n", tmp, strerror(errno));
		return -1;
	}
	if (fprintf(f, "%s\n", addr_str) < 0 || fclose(f) != 0) {
		fprintf(stderr, "nkvx_service: writing %s failed: %s\n", tmp, strerror(errno));
		remove(tmp);
		return -1;
	}
	if (rename(tmp, addr_file) != 0) {
		fprintf(stderr, "nkvx_service: rename(%s -> %s) failed: %s\n",
			tmp, addr_file, strerror(errno));
		remove(tmp);
		return -1;
	}
	return 0;
}

/*
 * Slice C6b / bead spdk-8od TEST stall hook: per progress tick, advance the deferral
 * countdown of any PUSH-stalled OR input-PULL-stalled req and, at 0, submit its PUSH
 * / PULL (or skip it if a cancel set do_not_push in the meantime — which finishes it
 * ABORTED). Submitting/finishing may remove the req from g_inflight, so walk it
 * safely. A no-op unless NKVX_TEST_PUSH_STALL_TICKS or NKVX_TEST_PULL_STALL_TICKS was
 * set. A given req is in at most one of the two stall states (PULL precedes the run
 * which precedes the PUSH), so the two branches never both fire for the same req.
 */
static void
nkvx_service_tick_stalls(void)
{
	struct nkvx_req *req, *next;

	if (g_test_push_stall_ticks == 0 && g_test_pull_stall_ticks == 0) {
		return;
	}
	req = TAILQ_FIRST(&g_inflight);
	while (req != NULL) {
		next = TAILQ_NEXT(req, reg_link);
		if (req->pull_pending) {
			/* bead spdk-8od PULL-side tick-deferral (mirrors the PUSH branch). */
			if (req->do_not_push) {
				/* A cancel landed during the PULL stall (Case b): never PULL. */
				req->pull_pending = false;
				fprintf(stderr, "nkvx_service: op_id=%u do_not_push set during "
					"stall (case b) — input PULL skipped, no late PULL\n",
					req->op_id);
				nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_ABORTED);
			} else if (req->pull_stall_ticks > 0) {
				req->pull_stall_ticks--;
			} else {
				/* Countdown expired with no cancel: submit the real PULL now. */
				req->pull_pending = false;
				(void)nkvx_submit_input_pull(req);
			}
		} else if (req->push_pending) {
			if (req->do_not_push && req->stall_ticks == 0) {
				/*
				 * A cancel landed during the stall (Case b): never PUSH. We let the
				 * remaining stall_ticks drain FIRST (the else-if below still runs
				 * while do_not_push is set) so the req stays REGISTERED for the rest
				 * of the stall window before we reap it ABORTED. This faithfully
				 * models a req whose (now-cancelled) transfer would still have held
				 * the executor's remote view for its duration, and — load-bearing for
				 * the F2 same-op_id guard (bead spdk-8od) — keeps BOTH same-op_id reqs
				 * registered long enough that a concurrent second cancel is looked up
				 * against a still-live registry (so a buggy op_id-keyed registry
				 * visibly aliases). The no-late-PUSH invariant is unaffected: the PUSH
				 * is never submitted. */
				req->push_pending = false;
				fprintf(stderr, "nkvx_service: op_id=%u do_not_push set during "
					"stall (case b) — PUSH skipped, no late PUSH\n", req->op_id);
				nkvx_req_fail(req, SPDK_KVDEV_IO_STATUS_ABORTED);
			} else if (req->stall_ticks > 0) {
				req->stall_ticks--;
			} else {
				/* Countdown expired with no cancel: submit the real PUSH now. */
				req->push_pending = false;
				nkvx_submit_result_push(req);
			}
		}
		req = next;
	}
}

/* Manual progress loop (design §4: the executor drives its own HG_Progress/
 * HG_Trigger). Trigger all ready completions, then block in Progress up to
 * timeout_ms so the loop wakes periodically to observe g_stop. */
static void
progress_loop(hg_context_t *ctx, unsigned int timeout_ms)
{
	while (!g_stop) {
		hg_return_t ret;
		unsigned int actual;

		do {
			actual = 0;
			ret = HG_Trigger(ctx, 0, 1, &actual);
		} while (ret == HG_SUCCESS && actual);

		/* Slice C6b: advance any TEST-stalled PUSH countdowns each tick. */
		nkvx_service_tick_stalls();

		if (g_stop) {
			break;
		}

		ret = HG_Progress(ctx, timeout_ms);
		if (ret != HG_SUCCESS && ret != HG_TIMEOUT) {
			fprintf(stderr, "nkvx_service: HG_Progress failed: %s\n",
				HG_Error_to_string(ret));
			break;
		}
	}
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--listen NA_INIT_STRING] [--addr-file PATH]\n"
		"          [--rados-pool POOL [--rados-namespace NS] [--rados-conf PATH] [--rados-user ID]]\n"
		"  --listen           Mercury/NA init string to listen on "
		"(default \"na+sm://\"; e.g. \"ofi+tcp://\", \"ofi+verbs://\")\n"
		"  --addr-file        publish the self-address to PATH (atomic write) for "
		"front bootstrap (design OQ-8)\n"
		"  --rados-pool       RADOS pool holding the KV objects; enables the Slice C5a\n"
		"                     cold-fill + module run (absent -> C2 skeleton, NOT_SUPPORTED)\n"
		"  --rados-namespace  RADOS namespace (== KV namespace; default namespace if omitted)\n"
		"  --rados-conf       ceph.conf path (default librados search if omitted)\n"
		"  --rados-user       ceph client id (default \"admin\")\n"
		"  --mem-object KEY=VALUE  TEST-ONLY in-memory object (slice spdk-7sr.4 / S3\n"
		"                     RETRIEVE loopback verify); repeatable; exclusive with --rados-pool\n",
		prog);
}

int
main(int argc, char **argv)
{
	const char *na_init = "na+sm://";
	const char *addr_file = NULL;
	const char *rados_pool = NULL;
	const char *rados_ns = NULL;
	const char *rados_conf = NULL;
	const char *rados_user = NULL;
	/* TEST-ONLY in-memory objects (slice spdk-7sr.4 / S3 loopback verify): each
	 * --mem-object KEY=VALUE seeds one object at oid=hex(KEY). Mutually exclusive
	 * with --rados-pool. */
	const char *mem_objs[NKVX_SERVICE_MAX_MEM_OBJS];
	int mem_obj_count = 0;
	int rc = 0;

	enum {
		OPT_RADOS_POOL = 256, OPT_RADOS_NS, OPT_RADOS_CONF, OPT_RADOS_USER,
		OPT_MEM_OBJECT,
	};
	static const struct option opts[] = {
		{ "listen",          required_argument, NULL, 'l' },
		{ "addr-file",       required_argument, NULL, 'a' },
		{ "rados-pool",      required_argument, NULL, OPT_RADOS_POOL },
		{ "rados-namespace", required_argument, NULL, OPT_RADOS_NS },
		{ "rados-conf",      required_argument, NULL, OPT_RADOS_CONF },
		{ "rados-user",      required_argument, NULL, OPT_RADOS_USER },
		{ "mem-object",      required_argument, NULL, OPT_MEM_OBJECT },
		{ "help",            no_argument,       NULL, 'h' },
		{ NULL,              0,                 NULL, 0 },
	};
	int c;
	while ((c = getopt_long(argc, argv, "l:a:h", opts, NULL)) != -1) {
		switch (c) {
		case 'l':
			na_init = optarg;
			break;
		case 'a':
			addr_file = optarg;
			break;
		case OPT_RADOS_POOL:
			rados_pool = optarg;
			break;
		case OPT_RADOS_NS:
			rados_ns = optarg;
			break;
		case OPT_RADOS_CONF:
			rados_conf = optarg;
			break;
		case OPT_RADOS_USER:
			rados_user = optarg;
			break;
		case OPT_MEM_OBJECT:
			if (mem_obj_count >= NKVX_SERVICE_MAX_MEM_OBJS) {
				fprintf(stderr, "nkvx_service: too many --mem-object\n");
				return 2;
			}
			mem_objs[mem_obj_count++] = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 2;
		}
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	/* Slice C6b in-flight registry + TEST stall-tick hook. */
	TAILQ_INIT(&g_inflight);
	{
		const char *st = getenv("NKVX_TEST_PUSH_STALL_TICKS");
		if (st != NULL) {
			long v = strtol(st, NULL, 10);
			if (v > 0) {
				g_test_push_stall_ticks = (unsigned int)v;
				fprintf(stderr, "nkvx_service: TEST PUSH stall = %u tick(s)\n",
					g_test_push_stall_ticks);
			}
		}
		/* bead spdk-8od: the symmetric PULL-side stall (F1 input-bulk cancel guard). */
		st = getenv("NKVX_TEST_PULL_STALL_TICKS");
		if (st != NULL) {
			long v = strtol(st, NULL, 10);
			if (v > 0) {
				g_test_pull_stall_ticks = (unsigned int)v;
				fprintf(stderr, "nkvx_service: TEST PULL stall = %u tick(s)\n",
					g_test_pull_stall_ticks);
			}
		}
	}

	hg_class_t *hg = HG_Init(na_init, HG_TRUE /* listen */);
	if (hg == NULL) {
		fprintf(stderr, "nkvx_service: HG_Init(\"%s\", listen) failed\n", na_init);
		return 1;
	}
	g_hg_class = hg;	/* cached for HG_Addr_cmp in the cancel registry (Slice C6b) */

	hg_context_t *ctx = HG_Context_create(hg);
	if (ctx == NULL) {
		fprintf(stderr, "nkvx_service: HG_Context_create failed\n");
		HG_Finalize(hg);
		return 1;
	}

	hg_id_t rpc_id = HG_Register_name(hg, "nkvx_exec",
					  hg_proc_nkvx_exec_in_t,
					  hg_proc_nkvx_exec_out_t,
					  nkvx_exec_handler);
	if (rpc_id == 0) {
		fprintf(stderr, "nkvx_service: HG_Register_name(nkvx_exec) failed\n");
		rc = 1;
		goto out;
	}

	/* Slice C6b: the cross-process cancel RPC. Additive (registered by a distinct
	 * name), so its name-hashed id does not perturb the frozen nkvx_exec contract. */
	hg_id_t cancel_id = HG_Register_name(hg, "nkvx_cancel",
					     hg_proc_nkvx_cancel_in_t,
					     hg_proc_nkvx_cancel_out_t,
					     nkvx_cancel_handler);
	if (cancel_id == 0) {
		fprintf(stderr, "nkvx_service: HG_Register_name(nkvx_cancel) failed\n");
		rc = 1;
		goto out;
	}

	/* Slice spdk-7sr.4 (S3): the base-layer KV verb RPC (Retrieve in S3; the other
	 * verbs in S4). Additive (distinct name), so it does not perturb the frozen
	 * nkvx_exec contract. */
	hg_id_t kv_id = HG_Register_name(hg, "nkvx_kv",
					 hg_proc_nkvx_kv_in_t,
					 hg_proc_nkvx_kv_out_t,
					 nkvx_kv_handler);
	if (kv_id == 0) {
		fprintf(stderr, "nkvx_service: HG_Register_name(nkvx_kv) failed\n");
		rc = 1;
		goto out;
	}

	/* TEST-ONLY in-memory backend (slice spdk-7sr.4 / S3): seed objects from
	 * --mem-object KEY=VALUE so the RETRIEVE datapath round-trips over na+sm with no
	 * librados. Mutually exclusive with --rados-pool. */
	if (mem_obj_count > 0) {
		int erc;

		if (rados_pool != NULL) {
			fprintf(stderr, "nkvx_service: --mem-object and --rados-pool are exclusive\n");
			rc = 1;
			goto out;
		}
		erc = nkvx_executor_open_mem(&g_executor);
		if (erc != 0) {
			fprintf(stderr, "nkvx_service: nkvx_executor_open_mem failed: %s\n",
				strerror(-erc));
			rc = 1;
			goto out;
		}
		for (int i = 0; i < mem_obj_count; i++) {
			const char *eq = strchr(mem_objs[i], '=');
			const char *key, *val;
			size_t klen;
			char oid[SPDK_KVDEV_EXEC_KEY_MAX_LEN * 2 + 1];
			static const char hex[] = "0123456789abcdef";

			if (eq == NULL) {
				fprintf(stderr, "nkvx_service: --mem-object must be KEY=VALUE\n");
				rc = 1;
				goto out;
			}
			key = mem_objs[i];
			klen = (size_t)(eq - key);
			val = eq + 1;
			if (klen == 0 || klen > SPDK_KVDEV_EXEC_KEY_MAX_LEN) {
				fprintf(stderr, "nkvx_service: --mem-object key must be 1..%d bytes\n",
					SPDK_KVDEV_EXEC_KEY_MAX_LEN);
				rc = 1;
				goto out;
			}
			for (size_t j = 0; j < klen; j++) {
				oid[j * 2]     = hex[(unsigned char)key[j] >> 4];
				oid[j * 2 + 1] = hex[(unsigned char)key[j] & 0xf];
			}
			oid[klen * 2] = '\0';
			erc = nkvx_executor_mem_put(g_executor, oid, val, strlen(val));
			if (erc != 0) {
				fprintf(stderr, "nkvx_service: mem_put(%s) failed: %s\n",
					oid, strerror(-erc));
				rc = 1;
				goto out;
			}
			printf("nkvx_service: mem object key='%.*s' (oid=%s) len=%zu\n",
			       (int)klen, key, oid, strlen(val));
		}
	}

	/* Slice C5a: connect librados up front (before publishing the address, so the
	 * executor can serve the first Exec the moment the front looks it up). Absent
	 * --rados-pool keeps the C2 skeleton (NOT_SUPPORTED) behaviour. */
	if (rados_pool != NULL) {
		int erc = nkvx_executor_open(rados_conf, rados_user, rados_pool, rados_ns,
					     &g_executor);
		if (erc != 0) {
			fprintf(stderr, "nkvx_service: nkvx_executor_open(pool=%s) failed: %s\n",
				rados_pool, strerror(-erc));
			rc = 1;
			goto out;
		}
		printf("nkvx_service: librados connected (pool=%s ns=%s)\n",
		       rados_pool, rados_ns ? rados_ns : "(default)");
	}

	if (publish_self_addr(hg, addr_file) != 0) {
		rc = 1;
		goto out;
	}

	progress_loop(ctx, 100 /* ms */);

out:
	if (addr_file != NULL) {
		/* Best-effort: do not leave a stale address behind on clean exit. */
		remove(addr_file);
	}
	nkvx_executor_close(g_executor);
	g_executor = NULL;
	HG_Context_destroy(ctx);
	HG_Finalize(hg);
	return rc;
}
