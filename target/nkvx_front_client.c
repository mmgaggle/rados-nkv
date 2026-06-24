/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/*
 * rados-nkv FRONT Mercury client core (Slice C4). See nkvx_front_client.h and
 * docs/design/slice-c-exec-rpc-mercury.md §4 (Mercury manual-progress model).
 */

#include "nkvx_front_client.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/queue.h>

#include <mercury.h>
#include <mercury_bulk.h>

/*
 * MR / hg_bulk handle cache (Slice C7.2, design §4.3 / OQ-7).
 *
 * Registering a bulk handle over a 64 MiB DPTR is not free: on the verbs
 * provider HG_Bulk_create -> ibv_reg_mr pins and page-walks ~16K pages on EVERY
 * large Exec — a perf cliff. The common case is a RECURRING tenant DPTR (SPDK
 * pool-recycles the DMA buffer), so a handle keyed by (addr,len,flags) is reused
 * across Execs instead of re-registered. Nil benefit on sm/tcp; the win is verbs.
 *
 * Bounded LRU: a fixed per-front slot array, evicting the least-recently-used
 * UNREFERENCED entry. nkvx_front is per reactor/channel and single-threaded (see
 * nkvx_front_client.h), so no locking. `refs` guards an in-flight handle from
 * eviction; a handle stays registered after refs hits 0 so the next reuse HITs.
 * If every slot is referenced at once (>= NKVX_BULK_CACHE_SLOTS concurrent
 * distinct DPTRs), the overflow handle is created uncached and freed on release.
 *
 * INVALIDATION CAVEAT (OQ-7, ties to C6 teardown): the cache keys on address and
 * cannot observe a tenant DPTR being freed/remapped. It relies on SPDK DMA
 * buffers being stable (pool-recycled, same mapping), which holds at the tenant
 * edge. Explicit invalidation-on-free is deferred (OQ-7 / C6a).
 */
#define NKVX_BULK_CACHE_SLOTS 64

struct nkvx_bulk_entry {
	void		*addr;		/* key: buffer base (HG_BULK_NULL handle == empty) */
	hg_size_t	len;		/* key: length */
	uint8_t		flags;		/* key: HG_BULK_READ_ONLY / HG_BULK_WRITE_ONLY */
	hg_bulk_t	handle;		/* the registered handle; HG_BULK_NULL if slot empty */
	uint32_t	refs;		/* in-flight Execs holding this handle */
	uint64_t	used_seq;	/* LRU stamp (front->bulk_seq at last acquire) */
};

/* Per-forward context, carried through HG_Forward's callback and freed there. */
struct nkvx_call {
	struct nkvx_front	*front;	/* owning front (for cache release in the cb) */
	nkvx_front_done_cb	cb;
	void			*arg;
	/*
	 * The Mercury handle this Exec rides on. Retained so nkvx_front_cancel_all()
	 * can HG_Cancel() it at teardown (Slice C6a). Destroyed in the forward
	 * completion.
	 */
	hg_handle_t		handle;
	/*
	 * Bulk handles the front originated for this Exec (Slice C7). They must stay
	 * registered for the whole RPC — the executor PULLs the input and PUSHes the
	 * result during the call, all before it responds — so they are released only
	 * here, in the forward completion. HG_BULK_NULL when the payload rode inline.
	 * Released via the C7.2 handle cache (kept registered for reuse, not freed).
	 */
	hg_bulk_t		input_bulk;
	hg_bulk_t		result_sink;
	/*
	 * In-flight list linkage (Slice C6a). A call is on front->inflight from
	 * submission until its forward completion fires. The channel-destroy drain
	 * walks it (cancel_all / fail_all_pending).
	 */
	TAILQ_ENTRY(nkvx_call)	link;
	/*
	 * Set by nkvx_front_fail_all_pending() (teardown-only): the tenant done-cb has
	 * already been fired FAILED and the call detached from the in-flight list, but
	 * a Mercury forward completion may still be pending (it carries this call as
	 * info->arg). When that completion eventually triggers, the forward cb reaps
	 * the call (release bulk + HG_Destroy + free) WITHOUT re-firing the tenant cb.
	 * This keeps the tenant completion exactly-once and avoids freeing the ctx
	 * while Mercury still references it.
	 */
	bool			tenant_done;

	/*
	 * Slice C6b two-phase cancel join (bead spdk-5ia, design §C6b). A cancel is a
	 * handshake: forward nkvx_cancel and wait for the executor's ACK — positive proof
	 * the executor's remote view of result_sink/input_bulk is gone — then HG_Cancel
	 * the Exec forward so the origin stops waiting. The DPTR/MR is released ONLY
	 * when BOTH the forward has resolved (forward_done) AND the ack has arrived
	 * (cancel_acked). Until then the call stays in-flight (n_inflight nonzero), which
	 * is what makes the channel-destroy drain wait for executor quiescence.
	 */
	bool			cancelling;	/* a cancel handshake is in progress */
	bool			forward_done;	/* the Exec forward has resolved (cb ran) */
	bool			cancel_acked;	/* the executor acked nkvx_cancel (or it is moot) */
	bool			detached;	/* removed from front->inflight (detach-once guard) */
	hg_handle_t		cancel_handle;	/* the nkvx_cancel forward handle (HG_HANDLE_NULL if none) */
	uint32_t		op_id;		/* cached for telemetry (== in->op_id) */
	uint64_t		call_id;	/* front-unique handshake token (keys the cancel) */
	enum spdk_kvdev_io_status join_status;	/* status to fire the tenant cb with on join */
};

struct nkvx_front {
	hg_class_t	*cls;
	hg_context_t	*ctx;
	hg_id_t		rpc_id;
	hg_id_t		cancel_rpc_id;	/* nkvx_cancel RPC id (Slice C6b) */
	hg_id_t		kv_rpc_id;	/* nkvx_kv base-verb RPC id (S3) */
	hg_addr_t	addr;	/* resolved executor target */

	/* In-flight Exec forwards (Slice C6a): channel-destroy cancel + drain. */
	TAILQ_HEAD(, nkvx_call)	inflight;
	unsigned		n_inflight;
	uint64_t		next_call_id;	/* monotonic per-front handshake-token source (C6b) */

	/* MR/hg_bulk handle cache (C7.2). */
	struct nkvx_bulk_entry	bulk_cache[NKVX_BULK_CACHE_SLOTS];
	uint64_t		bulk_seq;	/* monotonic LRU clock */
	uint64_t		bulk_hits;
	uint64_t		bulk_misses;
	uint64_t		bulk_evicts;
};

/*
 * Acquire a bulk handle for [addr, addr+len) with `flags`, from the cache when a
 * matching live entry exists (no re-registration), else register a fresh one and
 * insert it (evicting the LRU unreferenced entry if the cache is full). Returns 0
 * and sets *out; negative errno on a registration failure. A handle returned when
 * the cache is full-of-in-flight-entries is UNCACHED — nkvx_bulk_release frees it.
 */
static int
nkvx_bulk_acquire(struct nkvx_front *front, void *addr, hg_size_t len,
		  uint8_t flags, hg_bulk_t *out)
{
	struct nkvx_bulk_entry *empty = NULL, *victim = NULL, *slot;
	hg_bulk_t h;
	hg_return_t ret;
	int i;

	for (i = 0; i < NKVX_BULK_CACHE_SLOTS; i++) {
		struct nkvx_bulk_entry *e = &front->bulk_cache[i];

		if (e->handle != HG_BULK_NULL && e->addr == addr &&
		    e->len == len && e->flags == flags) {
			e->refs++;
			e->used_seq = ++front->bulk_seq;
			front->bulk_hits++;
			*out = e->handle;	/* HIT: reuse, no ibv_reg_mr */
			return 0;
		}
		if (e->handle == HG_BULK_NULL) {
			if (empty == NULL) {
				empty = e;
			}
		} else if (e->refs == 0 &&
			   (victim == NULL || e->used_seq < victim->used_seq)) {
			victim = e;	/* LRU unreferenced eviction candidate */
		}
	}

	/* MISS: register a fresh handle. */
	front->bulk_misses++;
	ret = HG_Bulk_create(front->cls, 1, &addr, &len, flags, &h);
	if (ret != HG_SUCCESS) {
		return -EIO;
	}

	slot = empty ? empty : victim;
	if (slot == NULL) {
		/* Every slot is in flight: hand back an uncached handle (release frees
		 * it — it will not be found in the cache). */
		*out = h;
		return 0;
	}
	if (slot->handle != HG_BULK_NULL) {	/* reclaiming a victim slot */
		HG_Bulk_free(slot->handle);
		front->bulk_evicts++;
	}
	slot->addr = addr;
	slot->len = len;
	slot->flags = flags;
	slot->handle = h;
	slot->refs = 1;
	slot->used_seq = ++front->bulk_seq;
	*out = h;
	return 0;
}

/*
 * Release a handle acquired above: drop the cache entry's refcount (the handle
 * stays REGISTERED for the next reuse). A handle not in the cache is an overflow
 * registration and is freed now. HG_BULK_NULL is a no-op.
 */
static void
nkvx_bulk_release(struct nkvx_front *front, hg_bulk_t h)
{
	int i;

	if (h == HG_BULK_NULL) {
		return;
	}
	for (i = 0; i < NKVX_BULK_CACHE_SLOTS; i++) {
		struct nkvx_bulk_entry *e = &front->bulk_cache[i];

		if (e->handle == h) {
			if (e->refs > 0) {
				e->refs--;
			}
			return;		/* keep registered for reuse */
		}
	}
	HG_Bulk_free(h);		/* uncached overflow handle */
}

/*
 * Register [result_sink, result_sink+len) as a WRITE-mode bulk backed by a
 * DMA-BUF fd (rados-nkvx S2 dma-buf bulk path, bead spdk-a27). The handle is
 * created via HG_Bulk_create_attr carrying {mem_type=HG_MEM_TYPE_HOST,
 * dmabuf_fd, dmabuf_offset}; na_ofi then registers the MR from the fd
 * (fi_mr_regattr(FI_MR_DMABUF)) so the executor RDMA-WRITEs straight into the
 * dma-buf-backed memory.
 *
 * UNCACHED on purpose (C7.2 caveat): a dma-buf handle's MR identity is the
 * (fd,offset) pair, NOT the VA — the cache keys on VA and would alias a stale fd.
 * So this never enters front->bulk_cache; nkvx_bulk_release HG_Bulk_free()s it
 * (handle-not-found path) when the RPC completes, exactly like an overflow handle.
 *
 * BASE (result_sink) is LOAD-BEARING, not a placeholder, on two counts:
 *   1. HG_Bulk_create_attr -> hg_bulk_create_na_mem_descs SKIPS any segment whose
 *      base == NULL ("Skip null segments"), so a NULL base would silently produce
 *      an UNREGISTERED bulk and the executor would have nothing to RDMA into.
 *   2. On the verbs/irdma provider (FI_MR_VIRT_ADDR), na_ofi sets the dma-buf MR's
 *      IOVA base_addr to this VA; Mercury RMA addresses the remote MR by that VA,
 *      so it MUST equal the IOVA the result_sink SGL segment advertised, or the
 *      executor's WRITE lands outside the MR window (irdma local access violation).
 * For the B-i mixed-SGL path the caller threads the dma-buf segment's guest IOVA
 * down as result_sink, which satisfies both. The IOVA is NEVER dereferenced (the
 * MR is registered from the fd, not from the VA), so a non-host-addressable IOVA
 * is safe here. We reject a NULL base with a dma-buf fd to fail loudly rather than
 * register a silently-skipped (unusable) bulk.
 */
static int
nkvx_bulk_acquire_dmabuf(struct nkvx_front *front, void *result_sink,
			 hg_size_t len, int dmabuf_fd, uint64_t dmabuf_offset,
			 hg_bulk_t *out)
{
	struct hg_bulk_attr attr = {
		.mem_type = HG_MEM_TYPE_HOST,	/* a dma-buf BAR/udmabuf is host-iface */
		.device = 0,
		.dmabuf_fd = dmabuf_fd,
		.dmabuf_offset = dmabuf_offset,
	};
	void *base = result_sink;
	hg_bulk_t h;
	hg_return_t ret;

	if (base == NULL) {
		/* Mercury would skip a NULL-base segment, leaving the sink unregistered;
		 * and verbs needs the IOVA as the MR base. Refuse rather than silently
		 * produce an unusable bulk. */
		return -EINVAL;
	}

	front->bulk_misses++;	/* always a "miss": dma-buf sinks are never cached */
	ret = HG_Bulk_create_attr(front->cls, 1, &base, &len, HG_BULK_WRITE_ONLY,
				  &attr, &h);
	if (ret != HG_SUCCESS) {
		return -EIO;
	}
	*out = h;
	return 0;
}

/*
 * ====================================================================
 * Slice C6b two-phase cancel join (bead spdk-5ia, design §C6b).
 *
 * A cancel splits a call's terminal completion across TWO callbacks — the Exec
 * forward (HG_Cancel'd) and the nkvx_cancel ack. The DPTR/MR is released only when
 * BOTH have resolved, so the executor cannot PUSH into the sink after the front
 * frees it. This is the SOLE difference from the normal path, which still tears
 * down inline in nkvx_front_forward_cb (byte-for-byte as before).
 * ====================================================================
 */

/*
 * The cancel-path join point: reap the call once BOTH the Exec forward has
 * resolved (forward_done) AND the executor has acked nkvx_cancel (cancel_acked).
 * Fires the tenant cb exactly once (join_status), releases the bulk handles (THE
 * sole DPTR/MR release on the cancel path — now provably after executor quiesce),
 * detaches from the in-flight list, and frees the call. Idempotent guards make it
 * safe to call from either resolving callback and after a forced fail-all detach.
 */
static void
nkvx_call_try_complete(struct nkvx_call *call)
{
	struct nkvx_front *front = call->front;

	if (!(call->cancelling && call->forward_done && call->cancel_acked)) {
		return;		/* still waiting on the other half of the join */
	}

	/* Tenant cb exactly once (fail_all_pending may already have fired it). */
	if (!call->tenant_done) {
		call->cb(call->arg, call->join_status, 0, NULL, 0);
		call->tenant_done = true;
	}

	/*
	 * Release the bulk handles ONLY here, after executor quiescence is proven by
	 * the ack — the UAF-safe release point. fail_all_pending nulls the handles
	 * after releasing them, so this is a no-op (HG_BULK_NULL) on that path.
	 */
	nkvx_bulk_release(front, call->input_bulk);
	nkvx_bulk_release(front, call->result_sink);
	call->input_bulk = HG_BULK_NULL;
	call->result_sink = HG_BULK_NULL;

	/* Detach from the in-flight list (fail_all_pending may already have done so). */
	if (!call->detached) {
		TAILQ_REMOVE(&front->inflight, call, link);
		call->detached = true;
		front->n_inflight--;
	}

	/* The exec handle was destroyed+nulled when the forward resolved; the cancel
	 * handle when the ack resolved. Nothing left to destroy. */
	free(call);
}

/* Forward declaration: begin_cancel forwards nkvx_cancel with this completion. */
static hg_return_t nkvx_cancel_ack_cb(const struct hg_cb_info *info);

/*
 * Begin (idempotently) the two-phase cancel of an in-flight call (design §C6b):
 *   phase 2: forward nkvx_cancel and await the executor's ack (the safety proof);
 *   phase 1: HG_Cancel the Exec forward so the origin stops waiting — but ONLY once
 *            the cancel was submitted, so a failure to signal the executor degrades
 *            to UAF-safe natural completion rather than a stranded bulk op.
 * Optimization: with neither result_sink nor input_bulk there is no remote bulk view
 * (inline-only payload), so the handshake is skipped (cancel_acked=true) — HG_Cancel
 * alone is safe.
 */
static void
nkvx_call_begin_cancel(struct nkvx_call *call)
{
	struct nkvx_front *front = call->front;
	nkvx_cancel_in_t cin;
	hg_handle_t ch = HG_HANDLE_NULL;
	hg_return_t ret;

	if (call->cancelling) {
		return;		/* idempotent */
	}
	call->cancelling = true;
	call->join_status = SPDK_KVDEV_IO_STATUS_ABORTED;

	/*
	 * No bulk handle registered over a tenant DPTR -> the executor has no remote
	 * view to PUSH into OR PULL from, so the cross-process UAF cannot occur and the
	 * handshake is unnecessary. Must check BOTH handles: a large-input / inline-or-
	 * small-result Exec has result_sink == NULL but a live input_bulk the executor
	 * may still be PULLing (spdk-5ia review). HG_Cancel alone is safe here.
	 */
	if (call->result_sink == HG_BULK_NULL && call->input_bulk == HG_BULK_NULL) {
		(void)HG_Cancel(call->handle);
		call->cancel_acked = true;
		return;
	}

	/*
	 * Phase 2 BEFORE phase 1 (spdk-5ia review). Forward nkvx_cancel FIRST, and only
	 * HG_Cancel the Exec forward once the cancel is actually submitted. If we cannot
	 * signal the executor, we must NOT cancel the Exec forward: letting it complete
	 * naturally is UAF-safe because the executor PUSHes/PULLs and only THEN responds
	 * (PUSH-before-Respond, design §1.3), so the natural forward completion is itself
	 * proof the executor's bulk access finished and the DPTR is releasable. Cancelling
	 * the forward early is precisely what would strand an in-flight bulk op against a
	 * freed DPTR — so the failure fallback waits for natural completion, not origin-
	 * only cancel. (The tenant still observes ABORTED via join_status; only the DPTR
	 * release is deferred to the safe point.)
	 */
	ret = HG_Create(front->ctx, front->addr, front->cancel_rpc_id, &ch);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_front: cancel HG_Create failed: %s (degrading to "
			"natural completion — UAF-safe via PUSH-before-Respond)\n",
			HG_Error_to_string(ret));
		call->cancel_acked = true;	/* join now waits only on the natural forward cb */
		return;
	}
	cin.call_id = call->call_id;
	ret = HG_Forward(ch, nkvx_cancel_ack_cb, call, &cin);
	if (ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_front: cancel HG_Forward failed: %s (degrading to "
			"natural completion — UAF-safe via PUSH-before-Respond)\n",
			HG_Error_to_string(ret));
		HG_Destroy(ch);
		call->cancel_acked = true;	/* join waits only on the natural forward cb */
		return;
	}
	call->cancel_handle = ch;

	/* Cancel submitted -> safe to abort the Exec forward fast (origin stops waiting).
	 * The forward cb resolves later (HG_CANCELED if it won, or the real status if the
	 * Exec finished first) and sets forward_done; the ack resolves in
	 * nkvx_cancel_ack_cb. The DPTR is released only when BOTH have fired. */
	(void)HG_Cancel(call->handle);
}

/*
 * nkvx_cancel ack completion. ANY resolution is the proof we need: a delivered ack
 * (HG_SUCCESS) is positive confirmation the executor's bulk view is gone; an error
 * (e.g. the executor finalized at channel-destroy) is safe in that teardown
 * context. Either way: destroy the cancel handle, mark acked, try to join.
 */
static hg_return_t
nkvx_cancel_ack_cb(const struct hg_cb_info *info)
{
	struct nkvx_call *call = info->arg;
	hg_handle_t ch = info->info.forward.handle;

	if (info->ret != HG_SUCCESS) {
		fprintf(stderr, "nkvx_front: cancel ack resolved %s (safe at "
			"channel-destroy)\n", HG_Error_to_string(info->ret));
	}
	HG_Destroy(ch);
	call->cancel_handle = HG_HANDLE_NULL;
	call->cancel_acked = true;
	nkvx_call_try_complete(call);
	return HG_SUCCESS;
}

/*
 * HG_Forward completion, fired from HG_Trigger inside nkvx_front_progress() — so
 * it runs on the progressing thread (the SPDK reactor in the integrated case).
 * Decodes the response, maps the wire status, hands the result to the caller's
 * done-cb, then releases the output, the handle, and this context.
 */
static hg_return_t
nkvx_front_forward_cb(const struct hg_cb_info *info)
{
	struct nkvx_call *call = info->arg;
	hg_handle_t handle = info->info.forward.handle;

	/*
	 * Slice C6b cancel path: this is phase 1 of the two-phase join resolving. Do
	 * NOT release bulks / fire the tenant cb / detach / free here — the DPTR must
	 * survive until the nkvx_cancel ack proves the executor will not PUSH. Just
	 * record that the forward resolved, drop our exec-handle ref, and try to join
	 * (which completes only once the ack has also arrived). Keeping the call on the
	 * in-flight list (n_inflight nonzero) is what makes the channel-destroy drain
	 * wait for executor quiescence.
	 */
	if (call->cancelling) {
		call->forward_done = true;
		HG_Destroy(handle);
		call->handle = HG_HANDLE_NULL;
		nkvx_call_try_complete(call);
		return HG_SUCCESS;
	}

	/*
	 * Teardown reap (Slice C6a, nkvx_front_fail_all_pending): the tenant cb was
	 * already fired FAILED and the call already removed from the in-flight list at
	 * forced teardown, but this Mercury completion was still pending. Reap WITHOUT
	 * re-firing the tenant cb or touching the list/counter again — exactly-once.
	 */
	if (call->tenant_done) {
		nkvx_bulk_release(call->front, call->input_bulk);
		nkvx_bulk_release(call->front, call->result_sink);
		HG_Destroy(handle);
		free(call);
		return HG_SUCCESS;
	}

	if (info->ret != HG_SUCCESS) {
		/*
		 * A cancelled forward (Slice C6a) resolves here with HG_CANCELED:
		 * map it to ABORTED -> ABORTED_BY_REQUEST at the tenant (design §C6a /
		 * §3 caps-exceeded row uses ABORTED; an aborted Exec is the request
		 * being withdrawn). Any OTHER transport/RPC failure (timeout, peer
		 * down, NAK) has no executor status: synthesize FAILED ->
		 * INTERNAL_DEVICE_ERROR (design §3 "New front-side cases").
		 */
		call->cb(call->arg,
			 (info->ret == HG_CANCELED) ? SPDK_KVDEV_IO_STATUS_ABORTED
						    : SPDK_KVDEV_IO_STATUS_FAILED,
			 0, NULL, 0);
		goto out;
	}

	nkvx_exec_out_t out;
	memset(&out, 0, sizeof(out));

	hg_return_t ret = HG_Get_output(handle, &out);
	if (ret != HG_SUCCESS) {
		/* The reply did not decode: treat as a transport-level fault. */
		call->cb(call->arg, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
		goto out;
	}

	call->cb(call->arg, nkvx_status_from_wire(out.status), out.result_len,
		 out.result_inline, out.result_inline_len);

	HG_Free_output(handle, &out);

out:
	/*
	 * Terminal completion (success, transport error, OR cancel ack). The ORIGIN
	 * side is now drained, so it is safe to release the bulk handles, destroy the
	 * handle, and free the call ctx — this is the SOLE place that happens on the
	 * normal path; cancellation routes through here too (HG_Cancel does not free
	 * early). CAVEAT (NOT fully UAF-safe; bead spdk-5ia): HG_Cancel is ORIGIN-side
	 * only — the executor has no do-not-PUSH awareness and may still PUSH into
	 * result_sink AFTER HG_CANCELED, because the forward completion fires when the
	 * ORIGIN-side ops drain, not when the executor stops. Releasing the result_sink
	 * MR here is safe at channel-destroy teardown (front+executor torn down, DPTR
	 * not reused per-command); a SAFE per-command abort needs the executor-side
	 * cancel protocol (spdk-5ia). The bulk handles stay REGISTERED in the C7.2 cache
	 * (the MR over still-mapped SPDK pool memory remains valid); overflow handles
	 * are freed.
	 *
	 * Remove from the in-flight list FIRST so a re-entrant cancel/teardown cannot
	 * match this (now-terminal) call. (Single-threaded per front, but the order is
	 * still load-bearing.)
	 */
	TAILQ_REMOVE(&call->front->inflight, call, link);
	call->detached = true;
	call->front->n_inflight--;
	nkvx_bulk_release(call->front, call->input_bulk);
	nkvx_bulk_release(call->front, call->result_sink);
	HG_Destroy(handle);
	free(call);
	return HG_SUCCESS;
}

int
nkvx_front_init(const char *na_init, const char *target_addr,
		struct nkvx_front **out)
{
	struct nkvx_front *front;
	hg_return_t ret;

	if (out == NULL || na_init == NULL || target_addr == NULL) {
		return -EINVAL;
	}
	*out = NULL;

	front = calloc(1, sizeof(*front));
	if (front == NULL) {
		return -ENOMEM;
	}
	front->addr = HG_ADDR_NULL;
	TAILQ_INIT(&front->inflight);

	front->cls = HG_Init(na_init, HG_FALSE /* origin, no listen */);
	if (front->cls == NULL) {
		free(front);
		return -ENOTCONN;
	}

	front->ctx = HG_Context_create(front->cls);
	if (front->ctx == NULL) {
		HG_Finalize(front->cls);
		free(front);
		return -ENOMEM;
	}

	/* Origin registers the same name/procs to obtain the matching RPC id;
	 * no handler on the origin (NULL rpc_cb). */
	front->rpc_id = HG_Register_name(front->cls, "nkvx_exec",
					 hg_proc_nkvx_exec_in_t,
					 hg_proc_nkvx_exec_out_t,
					 NULL);
	if (front->rpc_id == 0) {
		goto err;
	}

	/* Slice C6b: the origin half of the cancel RPC — same name/procs to obtain the
	 * matching id, NULL handler (the origin only forwards it, design §C6b). */
	front->cancel_rpc_id = HG_Register_name(front->cls, "nkvx_cancel",
						hg_proc_nkvx_cancel_in_t,
						hg_proc_nkvx_cancel_out_t,
						NULL);
	if (front->cancel_rpc_id == 0) {
		goto err;
	}

	/* S3: the origin half of the base-layer KV verb RPC — same name/procs to obtain
	 * the matching id, NULL handler (the origin only forwards it). */
	front->kv_rpc_id = HG_Register_name(front->cls, "nkvx_kv",
					    hg_proc_nkvx_kv_in_t,
					    hg_proc_nkvx_kv_out_t,
					    NULL);
	if (front->kv_rpc_id == 0) {
		goto err;
	}

	ret = HG_Addr_lookup2(front->cls, target_addr, &front->addr);
	if (ret != HG_SUCCESS) {
		goto err;
	}

	*out = front;
	return 0;

err:
	HG_Context_destroy(front->ctx);
	HG_Finalize(front->cls);
	free(front);
	return -ENOTCONN;
}

void
nkvx_front_fini(struct nkvx_front *front)
{
	int i;

	if (front == NULL) {
		return;
	}
	/* Free every cached bulk handle. Callers drain in-flight Execs before fini
	 * (per the threading contract), so no entry should still be referenced. */
	for (i = 0; i < NKVX_BULK_CACHE_SLOTS; i++) {
		if (front->bulk_cache[i].handle != HG_BULK_NULL) {
			HG_Bulk_free(front->bulk_cache[i].handle);
		}
	}
	HG_Addr_free(front->cls, front->addr);
	HG_Context_destroy(front->ctx);
	HG_Finalize(front->cls);
	free(front);
}

void
nkvx_front_get_bulk_stats(const struct nkvx_front *front,
			  struct nkvx_front_bulk_stats *stats)
{
	if (front == NULL || stats == NULL) {
		return;
	}
	stats->hits = front->bulk_hits;
	stats->misses = front->bulk_misses;
	stats->evicts = front->bulk_evicts;
}

/*
 * Full forward implementation: nkvx_front_forward_tok and
 * nkvx_front_forward_dmabuf are thin wrappers. When result_sink_dmabuf_fd >= 0
 * the result_sink is registered from that dma-buf fd (uncached, S2); otherwise
 * the VA is registered cache-eligibly (the original C7 path, byte-for-byte).
 */
static int
nkvx_front_forward_full(struct nkvx_front *front, const nkvx_exec_in_t *in,
			void *result_sink, uint32_t result_sink_len,
			int result_sink_dmabuf_fd, uint64_t result_sink_dmabuf_offset,
			nkvx_front_done_cb cb, void *arg, uint64_t *out_token)
{
	struct nkvx_call *call;
	nkvx_exec_in_t local;		/* mutable copy: carries the bulk handles */
	hg_handle_t handle = HG_HANDLE_NULL;
	hg_return_t ret;
	int rc;

	if (out_token != NULL) {
		*out_token = NKVX_CALL_TOKEN_NONE;
	}

	if (front == NULL || in == NULL || cb == NULL) {
		return -EINVAL;
	}

	/* A large input must come with a source buffer to register as a READ bulk;
	 * a large input_len with no input_inline would ship a request advertising
	 * input it cannot deliver (neither inline nor via PULL). Reject it. */
	if (in->input_len > NKVX_INLINE_MAX && in->input_inline == NULL) {
		return -EINVAL;
	}

	/*
	 * Defense in depth (R2, bead spdk-avu): a dma-buf result_sink has NO
	 * host_out fallback -- the result is delivered ONLY by a bulk WRITE into the
	 * dma-buf MR. A sink at or below the inline cap registers no bulk (the gate
	 * below requires > NKVX_INLINE_MAX), so the executor would return the result
	 * inline and the front would DROP it while still completing SUCCESS: silent
	 * data loss. The NVMf layer already rejects this upstream with a clean NVMe
	 * status; this is a belt-and-suspenders reject for any other caller.
	 */
	if (result_sink_dmabuf_fd >= 0 && result_sink_len <= NKVX_INLINE_MAX) {
		return -EINVAL;
	}

	call = calloc(1, sizeof(*call));
	if (call == NULL) {
		return -ENOMEM;
	}
	call->front = front;
	call->cb = cb;
	call->arg = arg;
	call->input_bulk = HG_BULK_NULL;
	call->result_sink = HG_BULK_NULL;
	call->op_id = in->op_id;		/* telemetry only */
	call->call_id = ++front->next_call_id;	/* front-unique handshake token (C6b); 0 reserved as none */
	call->cancel_handle = HG_HANDLE_NULL;	/* (calloc already zeroes the join fields) */

	/*
	 * Originate the front-side bulk handles (Slice C7, design §1.3): the front is
	 * the registrable side, so it registers its own input buffer (READ) and host
	 * output buffer (WRITE) and ships the handles. The encode below copies them
	 * onto the wire; the executor PULLs/PUSHes against them mid-RPC.
	 */
	local = *in;
	local.client_call_id = call->call_id;	/* front-assigned; the executor keys its cancel registry on it */
	local.input_bulk = HG_BULK_NULL;
	local.result_sink = HG_BULK_NULL;

	if (in->input_len > NKVX_INLINE_MAX && in->input_inline != NULL) {
		/* C7.2: reuse a cached MR/hg_bulk handle for this input DPTR, or
		 * register one (design §4.3); released back to the cache in the cb. */
		rc = nkvx_bulk_acquire(front, in->input_inline, in->input_len,
				       HG_BULK_READ_ONLY, &call->input_bulk);
		if (rc != 0) {
			free(call);
			return rc;
		}
		local.input_bulk = call->input_bulk;
	}

	/*
	 * B3 fix (bead spdk-avu): a pure-VRAM dma-buf sink has NO host VA
	 * (result_sink == NULL) -- its MR identity is (fd, offset), not a VA. The old
	 * gate `result_sink != NULL` would skip registration for it, leaving the
	 * executor with no bulk to RDMA-WRITE into. Register whenever there is a
	 * dma-buf fd OR a non-NULL VA, provided the sink is larger than the inline cap.
	 */
	if ((result_sink_dmabuf_fd >= 0 || result_sink != NULL) &&
	    result_sink_len > NKVX_INLINE_MAX) {
		if (result_sink_dmabuf_fd >= 0) {
			/* S2 dma-buf bulk path: register the result_sink from the
			 * dma-buf fd (uncached — keyed by fd, not VA). The executor
			 * RDMA-WRITEs straight into the dma-buf-backed region. */
			rc = nkvx_bulk_acquire_dmabuf(front, result_sink,
						      result_sink_len,
						      result_sink_dmabuf_fd,
						      result_sink_dmabuf_offset,
						      &call->result_sink);
		} else {
			/* C7.2: reuse a cached MR/hg_bulk handle for this result-sink
			 * DPTR (the recurring tenant output buffer), or register one. */
			rc = nkvx_bulk_acquire(front, result_sink, result_sink_len,
					       HG_BULK_WRITE_ONLY,
					       &call->result_sink);
		}
		if (rc != 0) {
			nkvx_bulk_release(front, call->input_bulk);
			free(call);
			return rc;
		}
		local.result_sink = call->result_sink;
	}

	ret = HG_Create(front->ctx, front->addr, front->rpc_id, &handle);
	if (ret != HG_SUCCESS) {
		goto err_bulk;
	}
	call->handle = handle;	/* retained for teardown HG_Cancel (Slice C6a) */
	call->tenant_done = false;

	/*
	 * Track in-flight BEFORE HG_Forward: the forward cb (which removes the call)
	 * can only fire from a later nkvx_front_progress() on this same thread, never
	 * synchronously inside HG_Forward, so inserting first cannot be undone out of
	 * order. On a forward submission failure we remove it again below.
	 */
	TAILQ_INSERT_TAIL(&front->inflight, call, link);
	front->n_inflight++;

	/*
	 * HG_Forward encodes the request into the SEND synchronously (the proc
	 * runs now), so `local` may go out of scope on return; the registered bulk
	 * handles, however, must outlive the RPC and are freed in the completion.
	 * The completion fires later from nkvx_front_progress() via the forward cb.
	 */
	ret = HG_Forward(handle, nkvx_front_forward_cb, call, &local);
	if (ret != HG_SUCCESS) {
		TAILQ_REMOVE(&front->inflight, call, link);
		front->n_inflight--;
		HG_Destroy(handle);
		goto err_bulk;
	}

	/*
	 * Slice C6c: hand back the cancel token (the call's front-unique call_id) so
	 * a later per-command ABORT can target THIS call. A token by value (not a ctx
	 * pointer) cannot dangle after the call completes + frees; nkvx_front_cancel
	 * looks it up and no-ops if it is already gone.
	 */
	if (out_token != NULL) {
		*out_token = call->call_id;
	}
	return 0;

err_bulk:
	nkvx_bulk_release(front, call->input_bulk);
	nkvx_bulk_release(front, call->result_sink);
	free(call);
	return -EIO;
}

int
nkvx_front_forward_tok(struct nkvx_front *front, const nkvx_exec_in_t *in,
		       void *result_sink, uint32_t result_sink_len,
		       nkvx_front_done_cb cb, void *arg, uint64_t *out_token)
{
	/* VA-registered result_sink (dmabuf_fd = -1): the original C7 path. */
	return nkvx_front_forward_full(front, in, result_sink, result_sink_len,
				       -1, 0, cb, arg, out_token);
}

int
nkvx_front_forward_dmabuf(struct nkvx_front *front, const nkvx_exec_in_t *in,
			  void *result_sink, uint32_t result_sink_len,
			  int result_sink_dmabuf_fd,
			  uint64_t result_sink_dmabuf_offset,
			  nkvx_front_done_cb cb, void *arg, uint64_t *out_token)
{
	/* S2 dma-buf bulk path: register the result_sink from a dma-buf fd. */
	return nkvx_front_forward_full(front, in, result_sink, result_sink_len,
				       result_sink_dmabuf_fd,
				       result_sink_dmabuf_offset, cb, arg,
				       out_token);
}

int
nkvx_front_forward(struct nkvx_front *front, const nkvx_exec_in_t *in,
		   void *result_sink, uint32_t result_sink_len,
		   nkvx_front_done_cb cb, void *arg)
{
	/* Token-less convenience wrapper (channel-destroy cancel_all does not need a
	 * per-call token; the standalone front UT calls this form). */
	return nkvx_front_forward_tok(front, in, result_sink, result_sink_len,
				      cb, arg, NULL);
}

/*
 * ====================================================================
 * Base-layer KV verb forward (slice spdk-7sr.4 / S3). Mirrors the Exec forward but
 * over the additive `nkvx_kv` RPC with the nkvx_kv_in_t/out_t envelope. S3 wires
 * RETRIEVE: only a result_sink (no input PULL), and no two-phase cancel join (a
 * Retrieve is short and non-mutating). The result_sink bulk is released in the
 * completion. NOT tracked on front->inflight (the inflight list / cancel machinery
 * is Exec-specific); a KV call is self-contained.
 * ====================================================================
 */
struct nkvx_kv_call {
	struct nkvx_front	*front;
	nkvx_front_done_cb	cb;
	void			*arg;
	hg_handle_t		handle;
	hg_bulk_t		result_sink;	/* HG_BULK_NULL when the value rode inline */
};

static hg_return_t
nkvx_kv_forward_cb(const struct hg_cb_info *info)
{
	struct nkvx_kv_call *call = info->arg;
	hg_handle_t handle = info->info.forward.handle;

	if (info->ret != HG_SUCCESS) {
		/* No KV-verb status from a transport failure: synthesize FAILED (design
		 * §3). HG_CANCELED is not expected here (no cancel path for KV in S3). */
		call->cb(call->arg, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
		goto out;
	}

	{
		nkvx_kv_out_t out;
		hg_return_t ret;

		memset(&out, 0, sizeof(out));
		ret = HG_Get_output(handle, &out);
		if (ret != HG_SUCCESS) {
			call->cb(call->arg, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
			goto out;
		}
		/* result_inline is NULL/empty on the PUSH path (value already in the sink);
		 * the done trampoline's inline-copy is then a no-op. */
		call->cb(call->arg, nkvx_status_from_wire(out.status), out.result_len,
			 out.result_inline, out.result_inline_len);
		HG_Free_output(handle, &out);
	}

out:
	nkvx_bulk_release(call->front, call->result_sink);
	HG_Destroy(handle);
	free(call);
	return HG_SUCCESS;
}

int
nkvx_front_kv_forward(struct nkvx_front *front, const nkvx_kv_in_t *in,
		      void *result_sink, uint32_t result_sink_len,
		      nkvx_front_done_cb cb, void *arg)
{
	struct nkvx_kv_call *call;
	nkvx_kv_in_t local;		/* mutable copy: carries the bulk handle */
	hg_handle_t handle = HG_HANDLE_NULL;
	hg_return_t ret;
	int rc;

	if (front == NULL || in == NULL || cb == NULL) {
		return -EINVAL;
	}

	call = calloc(1, sizeof(*call));
	if (call == NULL) {
		return -ENOMEM;
	}
	call->front = front;
	call->cb = cb;
	call->arg = arg;
	call->result_sink = HG_BULK_NULL;

	local = *in;
	local.value_bulk = HG_BULK_NULL;	/* Store large-value PULL is a later slice */
	local.result_sink = HG_BULK_NULL;

	/* Register the host output buffer WRITE-mode when it can hold a large value so
	 * the executor PUSHes straight into it (Retrieve/List). A small value returns
	 * inline. */
	if (result_sink != NULL && result_sink_len > NKVX_INLINE_MAX) {
		rc = nkvx_bulk_acquire(front, result_sink, result_sink_len,
				       HG_BULK_WRITE_ONLY, &call->result_sink);
		if (rc != 0) {
			free(call);
			return rc;
		}
		local.result_sink = call->result_sink;
	}

	ret = HG_Create(front->ctx, front->addr, front->kv_rpc_id, &handle);
	if (ret != HG_SUCCESS) {
		nkvx_bulk_release(front, call->result_sink);
		free(call);
		return -EIO;
	}
	call->handle = handle;

	ret = HG_Forward(handle, nkvx_kv_forward_cb, call, &local);
	if (ret != HG_SUCCESS) {
		HG_Destroy(handle);
		nkvx_bulk_release(front, call->result_sink);
		free(call);
		return -EIO;
	}
	return 0;
}

bool
nkvx_front_cancel(struct nkvx_front *front, uint64_t token)
{
	struct nkvx_call *call;

	if (front == NULL || token == NKVX_CALL_TOKEN_NONE) {
		return false;
	}
	/*
	 * Slice C6c live per-command cancel (bead spdk-v3w). Locate the targeted call
	 * on the in-flight list by its front-unique call_id and route it through the
	 * SAME two-phase begin-cancel handshake as nkvx_front_cancel_all() — never a
	 * duplicate of the handshake logic. If the token matches no in-flight call the
	 * call already completed (forward cb reaped + freed it) or was force-failed at
	 * teardown: nothing to cancel, return false. The single-threaded-per-front
	 * contract (see header) means the list cannot change under this walk.
	 */
	TAILQ_FOREACH(call, &front->inflight, link) {
		if (call->call_id == token) {
			nkvx_call_begin_cancel(call);	/* idempotent if already cancelling */
			return true;
		}
	}
	return false;
}

void
nkvx_front_cancel_all(struct nkvx_front *front)
{
	struct nkvx_call *call;

	if (front == NULL) {
		return;
	}
	/*
	 * Slice C6b: begin the two-phase cancel handshake for each in-flight call —
	 * HG_Cancel the Exec forward (phase 1) AND forward nkvx_cancel (phase 2). The
	 * call stays in-flight until BOTH the forward resolves and the executor acks,
	 * so the DPTR/MR is not released until the executor's remote view is provably
	 * gone (no late PUSH). begin_cancel is idempotent and does NOT mutate the
	 * in-flight list here (resolution detaches later), so a straight forward-walk is
	 * safe. The caller progresses until nkvx_front_outstanding() reaches 0 — now
	 * bounded by the ack, not just the origin cancel.
	 */
	TAILQ_FOREACH(call, &front->inflight, link) {
		nkvx_call_begin_cancel(call);
	}
}

void
nkvx_front_fail_all_pending(struct nkvx_front *front,
			    enum spdk_kvdev_io_status status)
{
	struct nkvx_call *call;

	if (front == NULL) {
		return;
	}
	/*
	 * TEARDOWN-ONLY last resort (the bounded cancel+drain did not reach 0 — a
	 * wedged/dead executor). For each still-in-flight call: fire the tenant done-cb
	 * ONCE (FAILED) so the io completes instead of hanging, release the bulk
	 * handles to the cache, and detach it from the in-flight list (n_inflight
	 * reaches 0 for fini). We do NOT free the call ctx nor HG_Destroy the handle
	 * here: a cancelled-but-not-drained forward may still have a Mercury completion
	 * pending (it carries this call as info->arg). The tenant_done flag tells the
	 * eventual forward cb to reap the call (release-again-noop + HG_Destroy + free)
	 * without re-firing the tenant cb; if that trigger never comes, HG_Finalize
	 * tears the handle down (Mercury holds its own forward-side ref — HG_Destroy on
	 * a non-terminal handle would only drop OUR ref, so deferring it is safe).
	 *
	 * Slice C6b: a call may be mid-cancel-JOIN — it can have TWO pending Mercury
	 * completions (the Exec forward AND the nkvx_cancel ack), each carrying this
	 * call as arg. So for a cancelling call we must NOT let the first-resolving
	 * forward cb free it; nkvx_call_try_complete already gates freeing on
	 * (forward_done && cancel_acked), so leaving tenant_done set and detaching here
	 * routes BOTH callbacks through try_complete and the last one reaps. A
	 * non-cancelling call has only the forward cb pending, reaped via its
	 * tenant_done branch. Detach via the shared `detached` guard so try_complete
	 * does not double-remove.
	 *
	 * The cross-process late-PUSH residual is now closed by the handshake itself;
	 * this teardown last-resort remains best-effort only when the executor is
	 * already dead (no ack will ever come — safe, the peer cannot PUSH either).
	 */
	/* glibc <sys/queue.h> has no TAILQ_FOREACH_SAFE; hand-roll the safe walk so the
	 * standalone build (which includes only <sys/queue.h>) compiles. */
	call = TAILQ_FIRST(&front->inflight);
	while (call != NULL) {
		struct nkvx_call *next = TAILQ_NEXT(call, link);

		call->cb(call->arg, status, 0, NULL, 0);
		call->tenant_done = true;
		nkvx_bulk_release(front, call->input_bulk);
		nkvx_bulk_release(front, call->result_sink);
		call->input_bulk = HG_BULK_NULL;	/* released; the reap cb must not re-release */
		call->result_sink = HG_BULK_NULL;
		TAILQ_REMOVE(&front->inflight, call, link);
		call->detached = true;
		front->n_inflight--;
		call = next;
	}
}

unsigned
nkvx_front_outstanding(const struct nkvx_front *front)
{
	return (front != NULL) ? front->n_inflight : 0;
}

int
nkvx_front_progress(struct nkvx_front *front, unsigned int timeout_ms)
{
	hg_return_t ret;
	unsigned int triggered = 0;

	if (front == NULL) {
		return -EINVAL;
	}

	/* Advance the network first, then fire whatever became ready. */
	ret = HG_Progress(front->ctx, timeout_ms);
	if (ret != HG_SUCCESS && ret != HG_TIMEOUT) {
		return -EIO;
	}

	do {
		unsigned int actual = 0;
		ret = HG_Trigger(front->ctx, 0, 1, &actual);
		triggered += actual;
	} while (ret == HG_SUCCESS);
	/* HG_Trigger returns HG_TIMEOUT when the completion queue is drained. */
	if (ret != HG_TIMEOUT) {
		return -EIO;
	}

	return (int)triggered;
}
