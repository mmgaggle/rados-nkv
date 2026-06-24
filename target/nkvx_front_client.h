/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * rados-nkv FRONT Mercury client core (Slice C4) — the origin side of the
 * inter-tier Exec RPC. See docs/design/slice-c-exec-rpc-mercury.md §4.
 *
 * This is deliberately SPDK-agnostic (its only SPDK include is <spdk/kvdev.h>
 * for the status enum, via nkvx_exec_rpc.h): it owns a Mercury ORIGIN class in
 * MANUAL-progress mode (design §4.1 "Do NOT use Margo on the front"), so the
 * SPDK front can drive HG_Progress/HG_Trigger from an SPDK poller on the reactor
 * thread and keep completion callbacks on that thread (design §4.2). The same
 * core is unit-tested standalone against the Slice C2 executor with no reactor.
 *
 * Threading: a struct nkvx_front is NOT thread-safe. Mercury context progress
 * must not run concurrently from two threads, so the SPDK integration owns one
 * nkvx_front PER reactor/channel (per-thread, no locking) — the SPDK-idiomatic
 * choice. All of init/forward/progress/fini for a given front run on its owning
 * thread.
 */

#ifndef NKVX_FRONT_CLIENT_H
#define NKVX_FRONT_CLIENT_H

#include "nkvx_exec_rpc.h"	/* nkvx_exec_in_t, status enum, contract procs */

#include <stdbool.h>	/* bool (nkvx_front_cancel return) */
#include <stdint.h>	/* uint64_t cancel token */

#ifdef __cplusplus
extern "C" {
#endif

struct nkvx_front;

/**
 * Per-Exec completion, invoked from nkvx_front_progress() (i.e. on the caller's
 * thread — the SPDK reactor in the integrated case, design §4.2) once the
 * forwarded RPC completes.
 *
 * \param arg            caller context passed to nkvx_front_forward().
 * \param status         the executor's status already mapped back through
 *                       nkvx_status_from_wire() — a transport/RPC failure is
 *                       surfaced as SPDK_KVDEV_IO_STATUS_FAILED (design §3).
 * \param result_len     TRUE result length (may exceed osize; truncation
 *                       semantics, design §1.2). Valid only when status maps to
 *                       a delivered result; 0 otherwise.
 * \param result_inline  inline result bytes (NULL when none, or when the result
 *                       was delivered straight into the caller's sink buffer via
 *                       a result_sink bulk PUSH — in which case the bytes are
 *                       already in that buffer and nothing is passed here).
 *                       Borrowed — valid only for the callback; copy if needed.
 * \param result_inline_len  number of valid bytes at result_inline.
 */
typedef void (*nkvx_front_done_cb)(void *arg,
				   enum spdk_kvdev_io_status status,
				   uint32_t result_len,
				   const void *result_inline,
				   uint32_t result_inline_len);

/**
 * Bring up a front client: init a Mercury ORIGIN class on \p na_init (e.g.
 * "na+sm://", "ofi+tcp://", "ofi+verbs://..."), register the nkvx_exec contract,
 * and resolve the executor self-address \p target_addr (as published by
 * nkvx_service, design OQ-8). The address lookup is synchronous here (bootstrap,
 * before the reactor poller is hot).
 *
 * \return 0 and *out set on success; negative errno-style on failure (and *out
 * left NULL). The class is torn down with nkvx_front_fini().
 */
int nkvx_front_init(const char *na_init, const char *target_addr,
		    struct nkvx_front **out);

/** Tear down a front client (free address, destroy context, finalize class). */
void nkvx_front_fini(struct nkvx_front *front);

/**
 * MR/hg_bulk handle-cache counters (Slice C7.2). A `hit` reused a registered
 * handle for a recurring DPTR (no ibv_reg_mr); a `miss` registered a fresh one;
 * an `evict` freed an LRU unreferenced entry to make room. Steady-state DPTR
 * reuse drives hits up and keeps misses ~= the distinct-buffer count.
 */
struct nkvx_front_bulk_stats {
	uint64_t	hits;
	uint64_t	misses;
	uint64_t	evicts;
};

/** Read the handle-cache counters (for observability / acceptance tests). */
void nkvx_front_get_bulk_stats(const struct nkvx_front *front,
			       struct nkvx_front_bulk_stats *stats);

/**
 * Forward ONE nkvx_exec request (async), with the large-payload bulk RMA wired
 * (Slice C7, design §1.3). The scalar/inline fields of \p in are encoded
 * synchronously into the SEND before returning, so the \p in struct itself may
 * be freed/reused on return. The large-payload SOURCE BUFFERS, however, must
 * outlive the RPC: \p result_sink AND, for large input (input_len >
 * NKVX_INLINE_MAX), the buffer \p in->input_inline points at. Both are
 * registered as bulk handles the executor accesses ASYNCHRONOUSLY during the
 * call (it PULLs the input and PUSHes the result on later progress ticks) and
 * are released only when \p cb fires. The caller MUST leave \p in's bulk handles HG_BULK_NULL — this
 * function originates and owns them (the front is the registrable side):
 *
 *   - Large input: when in->input_len > NKVX_INLINE_MAX, \p in->input_inline is
 *     registered as a READ-mode bulk and the executor PULLs it. (Small input
 *     rides inline; in->input_inline carries it as before.)
 *   - Large result: when \p result_sink_len > NKVX_INLINE_MAX, \p result_sink is
 *     registered as a WRITE-mode bulk and shipped so the executor PUSHes the
 *     result straight into it. The executor PUSHes only when the result actually
 *     exceeds the inline cap; a small result still returns inline (the done-cb
 *     then carries result_inline). \p result_sink is the caller's host output
 *     buffer (the tenant DPTR) and \p result_sink_len its capacity (== osize);
 *     pass (NULL, 0) for a guaranteed-small result.
 *
 * The bulk handles stay registered until the RPC completes and are released in
 * the completion path, so \p result_sink and (for large input) the buffer at
 * \p in->input_inline must remain valid until \p cb fires.
 *
 * On success the RPC is in flight and \p cb will fire exactly once from a later
 * nkvx_front_progress() call. On a synchronous submission failure (bulk register
 * / handle create / forward), returns negative and \p cb is NOT called.
 *
 * \return 0 if the RPC was submitted; negative errno-style otherwise.
 */
int nkvx_front_forward(struct nkvx_front *front, const nkvx_exec_in_t *in,
		       void *result_sink, uint32_t result_sink_len,
		       nkvx_front_done_cb cb, void *arg);

/**
 * Like nkvx_front_forward(), but also hand back an opaque CANCEL TOKEN for the
 * call it submitted (Slice C6c, bead spdk-v3w). On success \p out_token (when
 * non-NULL) is set to the front-unique handle for this in-flight Exec; a later
 * nkvx_front_cancel(front, token) routes THAT specific call through the same
 * two-phase begin-cancel handshake nkvx_front_cancel_all() uses. The token is a
 * plain value (the call's front-unique call_id), NOT a pointer into the call
 * ctx, so retaining it cannot dangle after the call completes and frees: a
 * cancel against an already-completed (or never-submitted) token is a safe
 * no-op. \p out_token is left 0 (== NKVX_CALL_TOKEN_NONE) on a submission
 * failure. Pass out_token == NULL to behave exactly like nkvx_front_forward().
 *
 * The per-io retention this token enables is what lets a live NVMe ABORT find
 * and cancel the one in-flight Exec it targets (vs. cancel_all at channel
 * destroy).
 */
int nkvx_front_forward_tok(struct nkvx_front *front, const nkvx_exec_in_t *in,
			   void *result_sink, uint32_t result_sink_len,
			   nkvx_front_done_cb cb, void *arg, uint64_t *out_token);

/**
 * Like nkvx_front_forward(), but register the large \p result_sink from a DMA-BUF
 * fd instead of its virtual address (rados-nkvx S2 dma-buf bulk path, bead
 * spdk-a27). When \p result_sink_dmabuf_fd >= 0 and \p result_sink_len >
 * NKVX_INLINE_MAX, the result-sink bulk is created via HG_Bulk_create_attr with
 * {mem_type=HG_MEM_TYPE_HOST, dmabuf_fd, dmabuf_offset}, so on the verbs provider
 * the executor RDMA-WRITEs the Exec result straight into the dma-buf-backed region
 * (e.g. a host udmabuf in the non-GPU test, or a GPU-VRAM vfio-user P2PDMA sink in
 * S3). \p result_sink is still passed (it is the VA the segment advertises and the
 * cache/identity key) but the actual MR is taken from the fd at
 * \p result_sink_dmabuf_offset. NOTE: \p result_sink must be NON-NULL even on the
 * dma-buf path -- Mercury skips a NULL-base segment (it would never register the
 * MR), and on verbs/irdma (FI_MR_VIRT_ADDR) it is load-bearing as the dma-buf MR's
 * IOVA base. For the B-i mixed-SGL case the caller threads the dma-buf segment's
 * guest IOVA here; it is the advertised VA and is NEVER dereferenced.
 *
 * dma-buf result sinks BYPASS the C7.2 bulk-handle cache: a dma-buf handle is
 * keyed by (fd, offset), not VA, so it must never be reused for a different fd
 * sharing the same VA — it is created uncached and freed (HG_Bulk_free) when the
 * RPC completes, exactly like a cache-overflow handle.
 *
 * Pass \p result_sink_dmabuf_fd < 0 to behave exactly like nkvx_front_forward()
 * (VA registration, cache-eligible). The input bulk path is unchanged.
 *
 * \return 0 if submitted; negative errno-style otherwise. \p out_token (when
 * non-NULL) carries the cancel token as in nkvx_front_forward_tok().
 */
int nkvx_front_forward_dmabuf(struct nkvx_front *front, const nkvx_exec_in_t *in,
			      void *result_sink, uint32_t result_sink_len,
			      int result_sink_dmabuf_fd,
			      uint64_t result_sink_dmabuf_offset,
			      nkvx_front_done_cb cb, void *arg,
			      uint64_t *out_token);

/**
 * Forward ONE base-layer KV verb (slice spdk-7sr.4 / S3) over the additive
 * `nkvx_kv` RPC. S3 wires RETRIEVE: \p in carries the verb, key, read_only, and
 * osize; \p result_sink is the tenant host output buffer (DPTR). When the value
 * exceeds NKVX_INLINE_MAX the front registers \p result_sink WRITE-mode and the
 * executor PUSHes the value straight into it; a smaller value returns inline and
 * the done-cb carries it (the caller copies it into the same buffer). Either way
 * the bytes land in \p result_sink. The scalar/inline fields of \p in are encoded
 * synchronously, but \p result_sink must remain valid until \p cb fires.
 *
 * The caller MUST leave \p in's bulk handles HG_BULK_NULL; this function originates
 * them. Store's large value_inline (the input-PULL analogue) is a later slice.
 *
 * On success the RPC is in flight and \p cb fires exactly once from a later
 * nkvx_front_progress(); on a synchronous submission failure returns negative and
 * \p cb is NOT called.
 *
 * \return 0 if submitted; negative errno-style otherwise.
 */
int nkvx_front_kv_forward(struct nkvx_front *front, const nkvx_kv_in_t *in,
			  void *result_sink, uint32_t result_sink_len,
			  nkvx_front_done_cb cb, void *arg);

/** Sentinel "no call" cancel token (a valid token is always nonzero). */
#define NKVX_CALL_TOKEN_NONE 0ull

/**
 * Cancel the ONE in-flight Exec identified by \p token (Slice C6c, bead
 * spdk-v3w) — the live per-command (tenant NVMe ABORT) path. Looks the call up
 * on \p front's in-flight list by its front-unique call_id and, if still in
 * flight, routes it through the SAME two-phase begin-cancel handshake as
 * nkvx_front_cancel_all() (forward nkvx_cancel + HG_Cancel the Exec forward; the
 * DPTR/MR is released only at the ack/forward join — UAF-safe, see
 * nkvx_front_cancel_all). Idempotent and race-safe:
 *   - token == NKVX_CALL_TOKEN_NONE, or no matching in-flight call (it already
 *     completed/was reaped, or was never submitted): a no-op, returns false.
 *   - a call mid-cancel already: begin_cancel is itself idempotent, returns true.
 * The caller still drives nkvx_front_progress() to resolve the handshake and
 * fire the tenant done-cb (with ABORTED) exactly once.
 *
 * \return true if a matching in-flight call was found and (re)entered cancel;
 *         false if there was nothing to cancel.
 */
bool nkvx_front_cancel(struct nkvx_front *front, uint64_t token);

/**
 * Cancel EVERY in-flight Exec on \p front (channel-destroy teardown drain).
 *
 * UAF-SAFE via the Slice C6b two-phase cancel handshake (bead spdk-5ia): for each
 * in-flight call this (1) HG_Cancel()s the ORIGIN-side Exec forward so it resolves
 * to a terminal completion, AND (2) forwards an nkvx_cancel RPC to the executor.
 * The call stays in-flight (counted by nkvx_front_outstanding) until BOTH the
 * forward has resolved AND the executor has ACKED the cancel — the ack is positive
 * proof the executor has set do-not-PUSH / HG_Bulk_cancel'd any in-flight PUSH and
 * its remote view of the result_sink is gone. ONLY THEN are the bulk handles (the
 * tenant DPTR / MR) released. So no PUSH can land in the DPTR after release — the
 * cross-process use-after-free C6a left open is closed here. (An inline-only call
 * with no result_sink skips phase 2: there is no DPTR for the executor to PUSH.)
 *
 * Idempotent. The caller progresses until nkvx_front_outstanding() reaches 0
 * (bounded by the ack), then, if the bound is exceeded (a wedged/dead executor),
 * uses nkvx_front_fail_all_pending() before fini.
 *
 * NOTE: this is the channel-destroy teardown primitive. Wiring the live NVMe
 * ABORT-opcode to a PER-COMMAND abort is a deferred follow-on (it needs per-io
 * handle retention keyed by NVMe cmd-id); the protocol here is what makes that
 * follow-on UAF-safe.
 */
void nkvx_front_cancel_all(struct nkvx_front *front);

/**
 * Number of Exec forwards currently in flight on \p front (submitted, cb not yet
 * fired). For the channel-destroy drain: cancel all outstanding and progress
 * until this reaches 0 before nkvx_front_fini().
 */
unsigned nkvx_front_outstanding(const struct nkvx_front *front);

/**
 * TEARDOWN-ONLY forced completion of every still-in-flight Exec (channel-destroy
 * last resort). For each in-flight call: fire its done-cb ONCE with \p status
 * (the tenant io completes FAILED instead of hanging), release its bulk handles
 * to the cache, and remove it from the in-flight list so nkvx_front_outstanding()
 * reaches 0 before fini — no leaked call ctxs, no Mercury finalize over a live
 * in-flight list as far as our bookkeeping is concerned.
 *
 * MEMORY-SAFETY: this does NOT free the call ctx nor HG_Destroy the handle here.
 * A cancelled-but-not-yet-drained forward may still have a Mercury completion
 * pending (it carries the call as info->arg); freeing the ctx now would be a UAF
 * when that completion later triggers. Instead the call is detached (its tenant
 * cb is marked already-fired) and left for the normal forward completion to reap
 * (release-handles + HG_Destroy + free) without re-firing the tenant cb. The
 * handle teardown is thus left to HG_Finalize / the eventual trigger, which is
 * the documented-safe path (Mercury holds its own forward-side ref on the handle;
 * HG_Destroy on a non-terminal handle only drops OUR ref).
 *
 * Use ONLY after cancel_all + a bounded progress drain failed to reach 0 (a
 * wedged/dead executor). The cross-process late-PUSH residual is bead spdk-5ia.
 */
void nkvx_front_fail_all_pending(struct nkvx_front *front,
				 enum spdk_kvdev_io_status status);

/**
 * Drive Mercury progress once (design §4.2): advance the network for up to
 * \p timeout_ms, then trigger any ready completions (firing nkvx_front_done_cb
 * on this thread). The SPDK poller calls this with timeout_ms == 0 (non-blocking,
 * never stalls the reactor). A standalone caller may pass a small timeout to
 * block.
 *
 * \return the number of completions triggered this call (>= 0), or negative on
 * a fatal progress error.
 */
int nkvx_front_progress(struct nkvx_front *front, unsigned int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* NKVX_FRONT_CLIENT_H */
