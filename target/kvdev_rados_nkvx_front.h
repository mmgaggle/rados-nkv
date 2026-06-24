/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * Two-tier front bridge (Slice C4): the thin glue between kvdev_rados.c (the
 * SPDK datapath) and the Mercury front client core (nkvx_front_client.{h,c}).
 *
 * This header deliberately pulls in NO Mercury headers — kvdev_rados.c includes
 * it and must stay free of <mercury.h>. `struct nkvx_front` is opaque here; its
 * definition lives in nkvx_front_client.c. The whole bridge (and these symbols)
 * compile only under SPDK_CONFIG_MERCURY; kvdev_rados.c guards every call site
 * with the same macro so a stock build is byte-identical (design §C4 / §1).
 *
 * Object-flow model (design §2): the front sends KEY-ONLY — key + binding +
 * input. The executor cold-fills the object from RADOS and owns the TB4 cache.
 */

#ifndef KVDEV_RADOS_NKVX_FRONT_H
#define KVDEV_RADOS_NKVX_FRONT_H

#include "spdk/stdinc.h"
#include "spdk/kvdev.h"		/* enum spdk_kvdev_io_status, spdk_kvdev_io_completion_cb */

#ifdef __cplusplus
extern "C" {
#endif

struct nkvx_front;	/* opaque; defined in nkvx_front_client.c */

/**
 * Bring up a front client bound to the executor self-address \p target_addr (as
 * published by the Slice C2 nkvx_service, design OQ-8). The NA init/provider
 * string is derived from the address prefix (e.g. "ofi+tcp://127.0.0.1:7" ->
 * origin "ofi+tcp://"). Synchronous (bootstrap, before the poller is hot).
 *
 * \return 0 and *out set on success; negative errno-style otherwise.
 */
int kvdev_rados_nkvx_front_create(const char *target_addr, struct nkvx_front **out);

/** Tear down a front client. */
void kvdev_rados_nkvx_front_destroy(struct nkvx_front *front);

/**
 * Non-blocking Mercury progress tick for the per-channel SPDK poller (design
 * §4.2): fires any ready completion callbacks on the calling (reactor) thread.
 *
 * \return number of completions fired (>= 0), or negative on a fatal error.
 */
int kvdev_rados_nkvx_front_progress(struct nkvx_front *front);

/**
 * BLOCKING progress for the channel-destroy teardown drain ONLY (Slice C6a):
 * advance Mercury for up to \p timeout_ms and fire ready completions. Unlike the
 * non-blocking poller tick, a small block here lets cancelled forwards reach a
 * terminal completion without busy-spinning the (being-destroyed) reactor.
 *
 * \return completions fired (>= 0), or negative on a fatal progress error.
 */
int kvdev_rados_nkvx_front_drain_progress(struct nkvx_front *front,
					  unsigned int timeout_ms);

/**
 * Forward ONE nkvx Exec to the remote executor (design §2, key-only). The wire
 * fields mirror what kvdev_rados_exec() resolved from the binding:
 *   - WASM route: runtime=NKVX, module_ns != "nkvx", (module_key, module_ns)
 *     locator, sha256 (+valid), caps tier.
 *   - built-in route: runtime=NKVX, module_ns="nkvx", module_key=<built-in name>,
 *     sha256_valid=false.
 *
 * On success (return 0) the RPC is in flight and \p cb_fn fires exactly once from
 * a later progress tick (on the reactor thread): up to \p output_buf_len bytes of
 * the inline result are copied into \p output_buf, then
 * cb_fn(cb_arg, status, TRUE_result_len) runs. The request is submitted
 * asynchronously: the output buffer (result sink) and, for large input, the
 * input buffer must remain valid until \p cb_fn fires (the bridge's callers
 * already keep the io's buffers alive until completion, which satisfies this).
 *
 * On a synchronous submission failure returns negative and \p cb_fn is NOT
 * called (the caller completes the io). Large input (> NKVX_INLINE_MAX) needs the
 * front-origin bulk PULL path (Slice C7); until then it is reported NOT_SUPPORTED
 * via \p cb_fn (return 0).
 *
 * Slice C6c: \p out_token (when non-NULL) receives an opaque per-command CANCEL
 * TOKEN for the submitted forward — pass it to kvdev_rados_nkvx_front_cancel() to
 * abort THIS specific in-flight Exec from a tenant NVMe ABORT. It is set to
 * KVDEV_RADOS_NKVX_TOKEN_NONE (0) on any path where there is nothing to cancel
 * (synchronous failure, or a forward that already terminally completed via cb_fn).
 * The caller retains it keyed by NVMe cmd-id and clears it when the io completes.
 *
 * \return 0 if submitted (or terminally completed via cb_fn); negative on a
 *         synchronous failure where cb_fn was not invoked.
 */
int kvdev_rados_nkvx_front_forward(struct nkvx_front *front,
				   const void *key, uint8_t key_len,
				   uint32_t op_id, bool read_only,
				   uint8_t runtime,
				   const char *module_key, const char *module_ns,
				   const uint8_t *sha256, bool sha256_valid,
				   uint64_t caps,
				   const void *input, uint32_t input_len,
				   void *output_buf, uint32_t output_buf_len,
				   spdk_kvdev_io_completion_cb cb_fn, void *cb_arg,
				   uint64_t *out_token);

/**
 * B-i V2 (bead spdk-avu): like kvdev_rados_nkvx_front_forward(), but the result
 * sink is a dma-buf (exported GPU VRAM) identified by (\p sink_fd, \p
 * sink_offset) rather than a host VA. Forwards via nkvx_front_forward_dmabuf()
 * so the remote executor RDMA-WRITEs the Exec result straight into the
 * dma-buf-backed region. \p sink_va is the VA the segment advertises (may be
 * NULL for a pure-VRAM sink); the MR is taken from the fd at \p sink_offset.
 * Same submission/cancel/token contract as kvdev_rados_nkvx_front_forward().
 */
int kvdev_rados_nkvx_front_forward_dmabuf(struct nkvx_front *front,
					  const void *key, uint8_t key_len,
					  uint32_t op_id, bool read_only,
					  uint8_t runtime,
					  const char *module_key, const char *module_ns,
					  const uint8_t *sha256, bool sha256_valid,
					  uint64_t caps,
					  const void *input, uint32_t input_len,
					  void *sink_va, uint32_t sink_len,
					  int sink_fd, uint64_t sink_offset,
					  spdk_kvdev_io_completion_cb cb_fn, void *cb_arg,
					  uint64_t *out_token);

/**
 * Forward ONE base-layer KV RETRIEVE to the remote executor (slice spdk-7sr.4 / S3)
 * over the additive `nkvx_kv` RPC. The front (bdev_kvrados) owns no librados; the
 * executor resolves oid=hex(key), serves the value from the shared TB4 cache, and
 * returns it inline (small) or PUSHes it into \p output_buf (large).
 *
 *   \p key/\p key_len   the object key (ADR-0014: parsed from the in-payload header
 *                       by the bdev; 1..255 bytes).
 *   \p read_only        the per-namespace read-only bit, carried on the wire
 *                       (ADR-0008); Retrieve is non-mutating so it is always allowed.
 *   \p output_buf/_len  the tenant host output buffer (DPTR) and its capacity (osize).
 *
 * On success (return 0) the RPC is in flight and \p cb_fn fires exactly once from a
 * later progress tick: up to \p output_buf_len bytes of the value land in
 * \p output_buf (inline-copied by the bridge or PUSHed by the executor), then
 * cb_fn(cb_arg, status, TRUE_value_len) runs (DW0 = the true value length;
 * BUFFER_TOO_SMALL when it exceeds osize, KEY_NOT_EXIST when absent). \p output_buf
 * must remain valid until \p cb_fn fires. On a synchronous failure returns negative
 * and \p cb_fn is NOT called.
 */
int kvdev_rados_nkvx_front_retrieve(struct nkvx_front *front,
				    const void *key, uint8_t key_len,
				    bool read_only,
				    void *output_buf, uint32_t output_buf_len,
				    spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * Forward ONE base-layer KV STORE (slice S4) over the `nkvx_kv` RPC. \p value/
 * \p value_len is the value to store (gathered by the bdev from the host SGL after
 * the ADR-0014 key header); \p store_flags carries SIKE/SINKE
 * (enum spdk_kvdev_store_flags). The executor writes through librados and
 * invalidates the TB4 cache. \p value must remain valid until \p cb_fn fires.
 * cb_fn(cb_arg, status, 0) runs on completion (no DW0 for Store; KEY_EXIST on a
 * SINKE conflict, KEY_NOT_EXIST on a SIKE conflict, READ_ONLY on a read-only ns).
 * Large values (> NKVX_INLINE_MAX) need the value-PULL path and currently report
 * NOT_SUPPORTED. On a synchronous failure returns negative and cb_fn is NOT called.
 */
int kvdev_rados_nkvx_front_store(struct nkvx_front *front,
				 const void *key, uint8_t key_len,
				 bool read_only, uint8_t store_flags,
				 const void *value, uint32_t value_len,
				 spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * Forward ONE base-layer KV DELETE (slice S4). Mutating: read-only gated at the
 * executor. cb_fn(cb_arg, status, 0): SUCCESS when removed, KEY_NOT_EXIST when
 * absent, READ_ONLY on a read-only ns. Synchronous failure: negative, no cb_fn.
 */
int kvdev_rados_nkvx_front_delete(struct nkvx_front *front,
				  const void *key, uint8_t key_len,
				  bool read_only,
				  spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * Forward ONE base-layer KV EXIST (slice S4). Non-mutating. cb_fn(cb_arg, status,
 * value_len): SUCCESS + the stored value length in DW0 when present, KEY_NOT_EXIST
 * when absent. Synchronous failure: negative, no cb_fn.
 */
int kvdev_rados_nkvx_front_exist(struct nkvx_front *front,
				 const void *key, uint8_t key_len,
				 bool read_only,
				 spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * Forward ONE base-layer KV LIST (slice S4). Non-mutating. \p start_key/
 * \p start_key_len is the start position (key_len 0 => from the first key). The
 * NRK-prefixed listing lands in \p output_buf (inline-copied or PUSHed), bounded by
 * \p output_buf_len (osize). cb_fn(cb_arg, status, TRUE_listing_len):
 * BUFFER_TOO_SMALL when the full listing exceeds osize (DW0 = true length).
 * \p output_buf must remain valid until cb_fn fires. Synchronous failure: negative,
 * no cb_fn.
 */
int kvdev_rados_nkvx_front_list(struct nkvx_front *front,
				const void *start_key, uint8_t start_key_len,
				bool read_only,
				void *output_buf, uint32_t output_buf_len,
				spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/** Sentinel "no in-flight forward to cancel" token (a live token is nonzero). */
#define KVDEV_RADOS_NKVX_TOKEN_NONE 0ull

/**
 * Cancel the ONE in-flight two-tier Exec forward identified by \p token (Slice
 * C6c, bead spdk-v3w) — the live per-command (tenant NVMe ABORT) path. Routes
 * the targeted call through the same UAF-safe two-phase begin-cancel handshake
 * as kvdev_rados_nkvx_front_cancel_all(), but for a single command. Idempotent
 * and race-safe: a token of KVDEV_RADOS_NKVX_TOKEN_NONE, or one whose forward
 * already completed (and was reaped), is a no-op returning false. The caller
 * still drives the per-channel progress poller to resolve the handshake and fire
 * the tenant cb_fn (ABORTED) exactly once.
 *
 * \return true if a matching in-flight forward was found and (re)entered cancel;
 *         false if there was nothing to cancel.
 */
bool kvdev_rados_nkvx_front_cancel(struct nkvx_front *front, uint64_t token);

/**
 * Cancel ALL in-flight two-tier Exec forwards on \p front (Slice C6a
 * channel-destroy teardown drain). BEST-EFFORT, origin-side only: HG_Cancel
 * drives each forward to a terminal completion, but the executor may still PUSH
 * into the result_sink afterwards (no executor-side cancel awareness — bead
 * spdk-5ia). Safe only at channel-destroy (front+executor torn down); NOT a
 * per-command abort. The caller progresses until
 * kvdev_rados_nkvx_front_outstanding() reaches 0.
 */
void kvdev_rados_nkvx_front_cancel_all(struct nkvx_front *front);

/**
 * Number of two-tier Exec forwards currently in flight on \p front (Slice C6a).
 * Used by channel-destroy to drain: cancel all, then progress until this is 0.
 */
unsigned kvdev_rados_nkvx_front_outstanding(struct nkvx_front *front);

/**
 * TEARDOWN-ONLY forced completion of every still-in-flight forward when the
 * bounded cancel+drain did not reach 0 (a wedged/dead executor). Fires each
 * tenant cb_fn with \p status (so the io completes instead of hanging) and
 * detaches the call so kvdev_rados_nkvx_front_outstanding() reaches 0 for a clean
 * destroy. Handle/ctx teardown is deferred to the eventual Mercury completion /
 * HG_Finalize (memory-safe; see nkvx_front_fail_all_pending). The cross-process
 * late-PUSH residual is bead spdk-5ia.
 */
void kvdev_rados_nkvx_front_fail_all_pending(struct nkvx_front *front,
					     enum spdk_kvdev_io_status status);

#ifdef __cplusplus
}
#endif

#endif /* KVDEV_RADOS_NKVX_FRONT_H */
