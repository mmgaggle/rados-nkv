/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Implementation of the in-process NVMe KV host (initiator) client shim.
 *
 * Mirrors the idioms of test/nvmf/kv/kv_host.c: attach over VFIOUSER, find the
 * CSI==KV namespace, alloc an io qpair, use spdk_dma_zmalloc DMA buffers, submit
 * each op then poll the qpair to completion capturing sct/sc/cdw0.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvme_kv.h"

#include "kv_host_shim.h"

struct kv_host_shim;

/*
 * Per-op identity context. ONE of these is heap-allocated per submitted op in
 * arm_op() and handed to the transport as that op's completion cb_arg. It
 * carries a back-pointer to the shim and the op's unique, monotonically
 * increasing token (op_id).
 *
 * LIFETIME (no slot reuse, robust to UNBOUNDED outstanding orphans):
 *   - Each submitted request's completion callback (io_complete) fires EXACTLY
 *     ONCE -- either its real completion, or its abort when the io qpair is
 *     freed/deleted (spdk_nvme_ctrlr_free_io_qpair -> abort_trackers). The ctx
 *     is freed in io_complete() on that single firing, so it is leak-free and
 *     never double-freed.
 *   - If submit FAILS (the request is never queued, so the callback will NEVER
 *     fire) the submit path frees the ctx instead.
 *   - Because every ctx is a DISTINCT allocation whose op_id is immutable for
 *     its whole lifetime, a late orphan callback reads back its OWN op_id from
 *     memory that no other op can have reused. This is what the old fixed ring
 *     could not guarantee: under sustained failure N orphans coexist, the ring
 *     wraps, and a stale slot's op_id gets overwritten by a later op -> a false
 *     identity match. A per-op heap ctx has no slot reuse and no aliasing, so it
 *     is correct for an arbitrary number of simultaneous outstanding orphans.
 */
struct kv_op_ctx {
	struct kv_host_shim	*sh;
	uint64_t		op_id;
};

struct kv_host_shim {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct spdk_nvme_qpair	*qpair;
	/* Cached KV namespace capabilities. */
	uint32_t		kvvml;	/* max value length */
	uint32_t		kvkml;	/* max key length */
	/* Whether this shim owns the SPDK env (called spdk_env_init). */
	bool			owns_env;
	/*
	 * Set when poll_to_completion() disconnected the qpair after a timeout
	 * or transport failure. The next op reconnects it via ensure_connected()
	 * before submitting, or fails fast if the target is still down.
	 */
	bool			qpair_failed;
	/*
	 * Per-op completion state with a PER-OP IDENTITY TOKEN.
	 *
	 * The transport (vfio-user, which reuses the PCIe qpair ops) does NOT
	 * abort an outstanding HARDWARE tracker on disconnect or on reconnect for
	 * this transport: the CONNECTED->ENABLING tracker abort in nvme_qpair.c is
	 * gated on trtype == PCIE and is skipped for vfio-user. So when an op times
	 * out or transport-fails, its tracker stays outstanding and its callback
	 * (an orphan) can fire at an UNBOUNDED later poll -- including DURING the
	 * NEXT op's poll_to_completion(). Under sustained failure an UNBOUNDED
	 * number of such orphans can be outstanding at once, all firing only when
	 * the io qpair is finally freed/deleted. A bounded drain on reconnect
	 * cannot guarantee consuming any of them.
	 *
	 * To make those orphans harmless we give every op a unique token carried in
	 * a PER-OP HEAP context (struct kv_op_ctx), not a recycled ring slot:
	 *   - next_op_id is a monotonically increasing counter; each op takes the
	 *     next value as its token before submitting (start 1; 0 == none).
	 *   - armed_op_id holds the token of the op currently being polled.
	 *   - the op's completion cb_arg is a freshly malloc'd struct kv_op_ctx
	 *     carrying that token; its op_id is immutable for the ctx's lifetime.
	 * io_complete() records a completion ONLY when ctx->op_id == armed_op_id;
	 * a stale orphan carries an OLD token != armed_op_id and is DISCARDED. So
	 * an orphan from op A can never be recorded as op B's result, even if it
	 * fires in the middle of B's armed poll window, and even if arbitrarily
	 * many orphans coexist -- each has its own distinct, never-reused ctx, so
	 * no slot reuse can ever alias one op's token onto another's memory.
	 */
	uint64_t		next_op_id;
	volatile uint64_t	armed_op_id;
	volatile bool		done;
	volatile uint8_t	last_sct;
	volatile uint8_t	last_sc;
	volatile uint32_t	last_cdw0;
};

/*
 * Arm the next op: allocate a fresh identity token AND a fresh per-op heap
 * context carrying it, and publish the token as the armed token. Returns the
 * per-op context to hand to the transport as cb_arg, or NULL on OOM (the caller
 * must fail the op and NOT submit).
 *
 * Unlike a ring slot, this allocation is distinct per op and is NEVER reused by
 * a later op, so its op_id can never be overwritten out from under an orphan: a
 * late io_complete() always reads back this op's own immutable token. The ctx is
 * freed exactly once -- in io_complete() when the op's (single) callback fires,
 * or on the submit-failure path if the request is never queued.
 */
static struct kv_op_ctx *
arm_op(struct kv_host_shim *sh)
{
	uint64_t id = ++sh->next_op_id;
	struct kv_op_ctx *ctx = malloc(sizeof(*ctx));

	if (ctx == NULL) {
		/* Roll back the token so next_op_id stays in step with armed ids;
		 * not strictly required (tokens only need to be unique), but keeps
		 * the counter tidy. armed_op_id is left unchanged (still 0/none). */
		sh->next_op_id--;
		return NULL;
	}
	ctx->sh = sh;
	ctx->op_id = id;
	sh->done = false;
	sh->armed_op_id = id;
	return ctx;
}

/*
 * Disarm the armed op: no completion will be accepted into the status slot until
 * the next arm_op(). Setting armed_op_id to 0 (never a valid token, since
 * next_op_id is pre-incremented so the first token is 1) means io_complete()
 * rejects EVERY callback -- including a stale orphan that fires after a timeout
 * or transport failure. Idempotent and cheap.
 */
static void
disarm_op(struct kv_host_shim *sh)
{
	sh->armed_op_id = 0;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct kv_host_shim *sh = cb_ctx;

	sh->ctrlr = ctrlr;
}

static void
io_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct kv_op_ctx *ctx = arg;
	struct kv_host_shim *sh = ctx->sh;

	/*
	 * Identity-token gate. Record a completion ONLY when it belongs to the
	 * currently armed op (ctx->op_id == armed_op_id). A stale orphan from a
	 * timed-out/transport-failed prior op carries that op's OLD token, which
	 * no longer matches armed_op_id (the timeout/failure path disarmed it, and
	 * a later op armed a new token), so it is DISCARDED here. This is what
	 * closes the vfio-user abort-on-reconnect window: the orphan's tracker is
	 * NOT aborted by disconnect/reconnect for this transport, so its callback
	 * may fire at an arbitrary later poll -- even in the middle of the next
	 * op's armed poll window, and even if arbitrarily many orphans are
	 * outstanding -- yet it can never be mistaken for that op's result because
	 * its token differs.
	 *
	 * ctx is a DISTINCT per-op heap allocation whose op_id is immutable for its
	 * lifetime; no later op reuses it (no ring, no slot recycling), so this
	 * dereference is always safe and the read-back op_id is unambiguous even
	 * under unbounded simultaneous orphans.
	 */
	if (ctx->op_id == sh->armed_op_id) {
		sh->armed_op_id = 0;	/* consume: the matching completion arrived */
		sh->last_sct = cpl->status.sct;
		sh->last_sc = cpl->status.sc;
		sh->last_cdw0 = cpl->cdw0;
		sh->done = true;
	}
	/*
	 * This op's (single) callback has now fired -- whether as its real
	 * completion above or as a discarded orphan. A given submitted request's
	 * cb fires EXACTLY ONCE (real completion or abort on qpair free/delete), so
	 * freeing the ctx here is correct, leak-free, and never a double free.
	 */
	free(ctx);
}

/*
 * Best-effort backstop drain. Pump a bounded number of poll passes with NO op
 * armed (armed_op_id == 0), so io_complete() discards anything that fires.
 *
 * This is NOT the primary protection and CANNOT be: on vfio-user the orphaned
 * tracker is not aborted by reconnect, so its callback may fire at an UNBOUNDED
 * later poll -- past any fixed number of passes here, and even during the next
 * op's armed poll. The identity-token gate in io_complete() is what actually
 * makes such an orphan harmless. This drain just opportunistically consumes any
 * orphan that happens to be ready right now, keeping the qpair tidy; the bound
 * means it can never hang.
 */
static void
drain_stale_completions(struct kv_host_shim *sh)
{
	unsigned int pass;

	for (pass = 0; pass < 8; pass++) {
		int32_t n = spdk_nvme_qpair_process_completions(sh->qpair, 0);

		if (n <= 0) {
			break;
		}
	}
}

/*
 * Per-op completion budget. Bounds how long an in-flight KV command may run
 * before poll_to_completion() gives up and reports -ETIMEDOUT.
 *
 * Rationale: poll_to_completion() previously busy-looped FOREVER. If the NVMe
 * controller/target died mid-op (e.g. the target was hard-killed during a
 * Store/Retrieve/Exist/Delete), the call hung forever, stalling the in-process
 * NIXL plugin data path. This mirrors the bounded-wait pattern in
 * test/nvmf/kv/kv_host.c (wait_for_completion_timeout). A healthy in-memory or
 * vfio-user KV op is sub-millisecond; 20s is generous headroom for a slow
 * round-trip (e.g. a RADOS-backed kvdev) while still bounding a dead/wedged
 * target to seconds, not forever. A transport-layer qpair failure (-ENXIO from
 * spdk_nvme_qpair_process_completions, the usual signal of a dead target) is
 * detected immediately and does NOT wait for the full deadline.
 */
#define KV_HOST_SHIM_OP_TIMEOUT_S 20u

/*
 * Poll the qpair until the in-flight command completes, the qpair fails at the
 * transport layer, or the timeout expires.
 *
 * Returns 0 on completion (sh->done set; the caller reads status_to_rc()).
 * Returns a negated errno otherwise:
 *   -ENXIO     the qpair failed at the transport layer (dead/removed target)
 *   -ETIMEDOUT the command did not complete within KV_HOST_SHIM_OP_TIMEOUT_S
 *
 * On any error path the qpair is disconnected and the shim is flagged for
 * reconnect (see ensure_connected()).
 *
 * IMPORTANT (vfio-user semantics): this transport does NOT abort an outstanding
 * HARDWARE tracker on disconnect OR on reconnect. disconnect only flushes
 * software-QUEUED requests; and the CONNECTED->ENABLING tracker abort is gated
 * on trtype == PCIE in nvme_qpair.c, so it is SKIPPED for vfio-user. The
 * outstanding tracker is aborted only on qpair DELETE/free or a full ctrlr
 * reset. So a timed-out/failed op's callback (an orphan) can fire at an
 * UNBOUNDED later poll -- including during the NEXT op's poll below. We make
 * that orphan harmless with the PER-OP IDENTITY TOKEN: on every error path here
 * we disarm the op (armed_op_id = 0) so the orphan no longer matches, and the
 * next op arms a fresh token. io_complete() then records only the token-matching
 * completion, dropping the orphan. So a late completion from a timed-out op can
 * never be mistaken for, or corrupt the status of, the NEXT op.
 */
static int
poll_to_completion(struct kv_host_shim *sh)
{
	uint64_t deadline;

	deadline = spdk_get_ticks() +
		   (uint64_t)KV_HOST_SHIM_OP_TIMEOUT_S * spdk_get_ticks_hz();

	while (!sh->done) {
		int32_t n = spdk_nvme_qpair_process_completions(sh->qpair, 0);

		if (n < 0) {
			/* Transport-level failure (e.g. -ENXIO: qpair failed). The
			 * target is gone; disconnect and flag for reconnect. The
			 * outstanding tracker (if any) is NOT aborted by this
			 * transport, so its orphan callback may fire later; disarm
			 * the op (token cleared) so io_complete() drops it. */
			disarm_op(sh);
			spdk_nvme_ctrlr_disconnect_io_qpair(sh->qpair);
			sh->qpair_failed = true;
			return n;
		}
		if (!sh->done && spdk_get_ticks() >= deadline) {
			/* Bounded backstop: the op never completed (target hung
			 * but not yet transport-failed). Disconnect and flag for
			 * reconnect. The op's hardware tracker is still outstanding
			 * and is NOT aborted by this transport; its orphan callback
			 * may fire at an arbitrary later poll. Disarm the op (token
			 * cleared) so io_complete() discards that orphan. */
			disarm_op(sh);
			spdk_nvme_ctrlr_disconnect_io_qpair(sh->qpair);
			sh->qpair_failed = true;
			return -ETIMEDOUT;
		}
	}
	return 0;
}

/*
 * Reconnect the qpair if a prior op timed out / saw a transport failure and
 * disconnected it. Called before submitting each op. Returns 0 if the qpair is
 * usable, or a negated errno if it could not be reconnected (the target is
 * still down) so the op fails fast rather than submitting onto a dead qpair.
 *
 * NOTE on the prior op's orphan: on vfio-user the prior op's outstanding tracker
 * is NOT aborted by reconnect (the CONNECTED->ENABLING abort is PCIe-only), so
 * its orphan callback may still fire later -- possibly during the next op's
 * poll. The best-effort drain below opportunistically consumes any orphan that
 * is ready right now (with no op armed, armed_op_id == 0, so io_complete()
 * discards it), but it is NOT relied upon: the per-op identity token is what
 * guarantees a late orphan can never be recorded as the next op's result.
 */
static int
ensure_connected(struct kv_host_shim *sh)
{
	int rc;

	if (!sh->qpair_failed) {
		return 0;
	}
	rc = spdk_nvme_ctrlr_reconnect_io_qpair(sh->qpair);
	if (rc != 0) {
		return rc < 0 ? rc : -rc;
	}
	/* No op armed: best-effort consume any orphan that is ready right now. */
	disarm_op(sh);
	drain_stale_completions(sh);
	sh->qpair_failed = false;
	return 0;
}

/*
 * Translate the captured completion into the public return convention:
 *   0          -> SUCCESS
 *   positive   -> device-reported NVMe status code (sct == GENERIC)
 *   negative   -> transport/other error (negated errno)
 */
static int
status_to_rc(struct kv_host_shim *sh)
{
	if (sh->last_sct != SPDK_NVME_SCT_GENERIC) {
		/* Non-generic status types are reported as a transport-ish error. */
		return -EIO;
	}
	return (int)sh->last_sc;
}

int
kv_host_shim_open(const struct kv_host_shim_opts *opts, struct kv_host_shim **out)
{
	struct kv_host_shim *sh;
	struct spdk_nvme_transport_id trid = {};
	const struct spdk_nvme_kv_ns_data *kv_ns_data;
	uint32_t nsid;
	int rc;

	if (opts == NULL || out == NULL) {
		return -EINVAL;
	}
	/*
	 * Define the out-param up front so every failure path below (env-init
	 * failure, probe/attach failure, no KV ns, qpair alloc failure) leaves
	 * the caller's handle defined rather than stale.
	 */
	*out = NULL;
	/* Size-version validation: require the field we read here. */
	if (opts->opts_size < sizeof(struct kv_host_shim_opts)) {
		return -EINVAL;
	}
	if (opts->vfu_addr == NULL) {
		return -EINVAL;
	}

	sh = calloc(1, sizeof(*sh));
	if (sh == NULL) {
		return -ENOMEM;
	}

	if (opts->init_env) {
		struct spdk_env_opts env_opts;

		env_opts.opts_size = sizeof(env_opts);
		spdk_env_opts_init(&env_opts);
		env_opts.name = opts->name ? opts->name : "kv_host_shim";
		/*
		 * As an in-process host harness we frequently run unprivileged and
		 * without reserved hugepages. Forcing no_huge selects IOVA=VA so DMA
		 * works without root/PA access; mem_size bounds the no-huge heap. If
		 * the caller has a root/hugepage environment this still works.
		 */
		env_opts.no_huge = true;
		env_opts.mem_size = 512;
		if (spdk_env_init(&env_opts) < 0) {
			free(sh);
			return -EFAULT;
		}
		sh->owns_env = true;
	}

	trid.trtype = SPDK_NVME_TRANSPORT_VFIOUSER;
	snprintf(trid.traddr, sizeof(trid.traddr), "%s", opts->vfu_addr);

	if (spdk_nvme_probe(&trid, sh, probe_cb, attach_cb, NULL) != 0 ||
	    sh->ctrlr == NULL) {
		rc = -ENODEV;
		goto err_env;
	}

	/* Bind the requested namespace, or the first CSI==KV ns when nsid==0. */
	if (opts->nsid != 0) {
		struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(sh->ctrlr, opts->nsid);

		if (ns != NULL && spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_KV) {
			sh->ns = ns;
		}
	} else {
		for (nsid = spdk_nvme_ctrlr_get_first_active_ns(sh->ctrlr); nsid != 0;
		     nsid = spdk_nvme_ctrlr_get_next_active_ns(sh->ctrlr, nsid)) {
			struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(sh->ctrlr, nsid);

			if (ns != NULL && spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_KV) {
				sh->ns = ns;
				break;
			}
		}
	}

	if (sh->ns == NULL) {
		rc = -ENOENT;
		goto err_detach;
	}

	kv_ns_data = spdk_nvme_kv_ns_get_data(sh->ns);
	if (kv_ns_data == NULL) {
		rc = -EPROTO;
		goto err_detach;
	}
	sh->kvkml = kv_ns_data->kvf[0].kvkml;
	sh->kvvml = kv_ns_data->kvf[0].kvvml;

	sh->qpair = spdk_nvme_ctrlr_alloc_io_qpair(sh->ctrlr, NULL, 0);
	if (sh->qpair == NULL) {
		rc = -ENOMEM;
		goto err_detach;
	}

	*out = sh;
	return 0;

err_detach:
	spdk_nvme_detach(sh->ctrlr);
err_env:
	if (sh->owns_env) {
		spdk_env_fini();
	}
	free(sh);
	return rc;
}

void
kv_host_shim_close(struct kv_host_shim *sh)
{
	if (sh == NULL) {
		return;
	}
	/*
	 * Free the io qpair FIRST, before freeing sh. On vfio-user, outstanding
	 * orphan trackers (from timed-out/failed ops) are aborted only when the
	 * qpair is freed/deleted: spdk_nvme_ctrlr_free_io_qpair() ->
	 * nvme_pcie_qpair_abort_trackers() fires each one's io_complete() exactly
	 * once. Each such firing frees that orphan's per-op heap ctx (the orphan's
	 * op_id != armed_op_id, so it is discarded but still freed), so NO ctx
	 * leaks across an arbitrary number of outstanding orphans. The disarm of
	 * the last op left armed_op_id == 0 (close is only reached after the final
	 * op returned, which disarms on every path), so these abort callbacks
	 * record nothing -- they just free. Doing this before free(sh) guarantees
	 * no orphan callback dereferences a freed sh.
	 */
	if (sh->qpair != NULL) {
		spdk_nvme_ctrlr_free_io_qpair(sh->qpair);
	}
	if (sh->ctrlr != NULL) {
		spdk_nvme_detach(sh->ctrlr);
	}
	if (sh->owns_env) {
		spdk_env_fini();
	}
	free(sh);
}

void *
kv_host_shim_dma_alloc(size_t len)
{
	return spdk_dma_zmalloc(len, 0, NULL);
}

void
kv_host_shim_dma_free(void *buf)
{
	spdk_dma_free(buf);
}

uint32_t
kv_host_shim_max_value_len(const struct kv_host_shim *sh)
{
	return sh ? sh->kvvml : 0;
}

uint32_t
kv_host_shim_max_key_len(const struct kv_host_shim *sh)
{
	return sh ? sh->kvkml : 0;
}

int
kv_host_shim_store(struct kv_host_shim *sh, const void *key, uint8_t key_len,
		   const void *value, uint32_t value_len)
{
	struct kv_op_ctx *ctx;
	int rc;

	if (sh == NULL) {
		return -EINVAL;
	}
	rc = ensure_connected(sh);
	if (rc != 0) {
		return rc;
	}
	/* Arm a fresh identity token + per-op heap ctx; io_complete() records only
	 * the completion carrying this token (ctx), dropping any stale orphan from a
	 * prior op, and frees ctx when this op's callback fires. */
	ctx = arm_op(sh);
	if (ctx == NULL) {
		return -ENOMEM;
	}
	rc = spdk_nvme_kv_store(sh->ns, sh->qpair, key, key_len, value, value_len,
				io_complete, ctx, 0);
	if (rc != 0) {
		/* Request never queued -> io_complete will NEVER fire for this ctx,
		 * so the submit path owns the free here. Disarm the token too. */
		disarm_op(sh);
		free(ctx);
		return rc < 0 ? rc : -rc;
	}
	rc = poll_to_completion(sh);
	if (rc != 0) {
		return rc;
	}
	return status_to_rc(sh);
}

int
kv_host_shim_retrieve(struct kv_host_shim *sh, const void *key, uint8_t key_len,
		      void *value, uint32_t buf_len, uint32_t *value_len_out)
{
	struct kv_op_ctx *ctx;
	int rc;

	if (sh == NULL) {
		return -EINVAL;
	}
	rc = ensure_connected(sh);
	if (rc != 0) {
		return rc;
	}
	/* Arm a fresh identity token + per-op heap ctx; io_complete() records only
	 * the completion carrying this token (ctx), dropping any stale orphan from a
	 * prior op, and frees ctx when this op's callback fires. */
	ctx = arm_op(sh);
	if (ctx == NULL) {
		return -ENOMEM;
	}
	rc = spdk_nvme_kv_retrieve(sh->ns, sh->qpair, key, key_len, value, buf_len,
				   io_complete, ctx, 0);
	if (rc != 0) {
		/* Request never queued -> io_complete will NEVER fire for this ctx,
		 * so the submit path owns the free here. Disarm the token too. */
		disarm_op(sh);
		free(ctx);
		return rc < 0 ? rc : -rc;
	}
	rc = poll_to_completion(sh);
	if (rc != 0) {
		return rc;
	}
	rc = status_to_rc(sh);
	/* On SUCCESS, cdw0 carries the device's TRUE value length. */
	if (rc == 0 && value_len_out != NULL) {
		*value_len_out = sh->last_cdw0;
	}
	return rc;
}

int
kv_host_shim_exist(struct kv_host_shim *sh, const void *key, uint8_t key_len)
{
	struct kv_op_ctx *ctx;
	int rc;

	if (sh == NULL) {
		return -EINVAL;
	}
	rc = ensure_connected(sh);
	if (rc != 0) {
		return rc;
	}
	/* Arm a fresh identity token + per-op heap ctx; io_complete() records only
	 * the completion carrying this token (ctx), dropping any stale orphan from a
	 * prior op, and frees ctx when this op's callback fires. */
	ctx = arm_op(sh);
	if (ctx == NULL) {
		return -ENOMEM;
	}
	rc = spdk_nvme_kv_exist(sh->ns, sh->qpair, key, key_len, io_complete, ctx);
	if (rc != 0) {
		/* Request never queued -> io_complete will NEVER fire for this ctx,
		 * so the submit path owns the free here. Disarm the token too. */
		disarm_op(sh);
		free(ctx);
		return rc < 0 ? rc : -rc;
	}
	rc = poll_to_completion(sh);
	if (rc != 0) {
		return rc;
	}
	return status_to_rc(sh);
}

int
kv_host_shim_delete(struct kv_host_shim *sh, const void *key, uint8_t key_len)
{
	struct kv_op_ctx *ctx;
	int rc;

	if (sh == NULL) {
		return -EINVAL;
	}
	rc = ensure_connected(sh);
	if (rc != 0) {
		return rc;
	}
	/* Arm a fresh identity token + per-op heap ctx; io_complete() records only
	 * the completion carrying this token (ctx), dropping any stale orphan from a
	 * prior op, and frees ctx when this op's callback fires. */
	ctx = arm_op(sh);
	if (ctx == NULL) {
		return -ENOMEM;
	}
	rc = spdk_nvme_kv_delete(sh->ns, sh->qpair, key, key_len, io_complete, ctx);
	if (rc != 0) {
		/* Request never queued -> io_complete will NEVER fire for this ctx,
		 * so the submit path owns the free here. Disarm the token too. */
		disarm_op(sh);
		free(ctx);
		return rc < 0 ? rc : -rc;
	}
	rc = poll_to_completion(sh);
	if (rc != 0) {
		return rc;
	}
	return status_to_rc(sh);
}
