/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * rados-nkvx executor backend (Slice C5a.1): the librados cold-fill + module run
 * that the standalone executor service (nkvx_service.c) invokes from its Mercury
 * handler. This is the "object/cache path on the executor" the design relocates
 * here (docs/design/slice-c-exec-rpc-mercury.md §2): the executor owns its own
 * librados (cold path only, ADR-0009), resolves oid=hex(key), cold-fills the
 * object, and runs the module — the front never ships object bytes.
 *
 * C5a.1 scope: the BUILT-IN modules (bytecount/identity) over a synchronous
 * librados cold-fill, replacing the C2 NOT_SUPPORTED stub for the built-in
 * route. Real-wasm (module fetch + sha256 verify + run_cached cold-fill-once +
 * the TB4 cache) is C5a.2; cross-hop invalidation is C5b.
 */

#ifndef NKVX_EXECUTOR_H
#define NKVX_EXECUTOR_H

#include "nkvx_exec_rpc.h"	/* nkvx_exec_in_t / nkvx_exec_out_t / nkvx_kv_in_t */

#ifdef __cplusplus
extern "C" {
#endif

struct nkvx_executor;

/**
 * The bytes a module produced for ONE Exec, decoupled from how they reach the
 * front (Slice C7). nkvx_executor_run() owns the COMPUTE; the Mercury service
 * handler owns the DELIVERY (inline in the response vs a front-sink bulk PUSH),
 * because the bulk RMA is a transport concern that needs the Mercury handle and
 * context the backend deliberately does not see (design §1.3 / §4).
 *
 *   - status   : the kvdev outcome (mapped to the wire int32 by the handler).
 *   - result_len: the TRUE result length (truncation semantics, ADR-0014 /
 *                 design §1.2) — always the full length the module produced,
 *                 even when only result_len <= osize bytes were materialized.
 *   - buf      : the delivered bytes — min(result_len, osize) of them — freshly
 *                malloc'd (NULL when nothing is delivered). Caller frees via
 *                nkvx_exec_result_free().
 *   - buf_len  : number of valid bytes at buf == min(result_len, osize).
 *
 * Delivery rule the handler applies (design §1.3): buf_len <= NKVX_INLINE_MAX
 * (or no result_sink) -> inline in the response; otherwise PUSH buf into the
 * front's WRITE-registered result_sink and respond with an empty inline body.
 */
struct nkvx_exec_result {
	enum spdk_kvdev_io_status	status;
	uint32_t			result_len;
	void				*buf;
	uint32_t			buf_len;
};

/** Release the delivered-bytes buffer a result holds (idempotent). */
void nkvx_exec_result_free(struct nkvx_exec_result *res);

/**
 * Open the executor's librados handle (cold path only, ADR-0009) and an ioctx on
 * \p pool (== the tenant subsystem's pool) with \p ns set (== KV namespace).
 *
 * \param conf  ceph.conf path (NULL -> librados default search).
 * \param user  ceph client id, e.g. "admin" (NULL -> "admin").
 * \param pool  rados pool holding the KV objects (required).
 * \param ns    rados namespace (NULL/"" -> default namespace).
 * \return 0 and *out set on success; negative errno-style otherwise.
 */
int nkvx_executor_open(const char *conf, const char *user, const char *pool,
		       const char *ns, struct nkvx_executor **out);

/**
 * TEST-ONLY: open an executor backed by an in-memory object table instead of
 * librados (slice spdk-7sr.4 / S3 verification). Used by the na+sm loopback test
 * when no Ceph cluster is reachable so the RETRIEVE datapath
 * (front->RPC->executor->cache->result) can be exercised end-to-end without
 * librados. Objects are added with nkvx_executor_mem_put(); oid=hex(key) must match
 * the front's encoding. The real datapath uses nkvx_executor_open() (librados).
 */
int nkvx_executor_open_mem(struct nkvx_executor **out);

/** TEST-ONLY: insert/replace an in-memory object (a copy is taken). \p oid is the
 *  hex-encoded key (== nkvx_key_to_oid output). */
int nkvx_executor_mem_put(struct nkvx_executor *ex, const char *oid,
			  const void *value, size_t len);

/** Tear down the executor's librados handle (or the in-memory table). */
void nkvx_executor_close(struct nkvx_executor *ex);

/**
 * Run ONE decoded Exec request: resolve oid=hex(in->key), cold-fill the object
 * from RADOS, run the requested module, and populate \p res (status, the TRUE
 * result_len, and a freshly-allocated buffer of the min(result_len, osize)
 * delivered bytes). The caller delivers those bytes — inline in the response or
 * via the front-sink bulk PUSH (design §1.3) — then releases them with
 * nkvx_exec_result_free().
 *
 * The result is delivery-agnostic: this routine NEVER inspects in->result_sink
 * or caps the result at NKVX_INLINE_MAX. A large result is materialized in full
 * (up to osize) and handed back; the Mercury service handler decides inline vs
 * bulk.
 *
 * Per-request input is NOT consumed by any current execution path. The built-in
 * (bytecount/identity) and wasm runs operate solely on the cold-filled object at
 * oid=hex(key); they never read in->input_inline or in->input_len. Input is
 * reserved in the contract — it is delivered to this tier (inline, or PULLed by
 * the handler when in->input_len > NKVX_INLINE_MAX) — but no current module
 * reads it. Wiring input into execution is future work (a separate bead).
 *
 * Always returns 0 with the outcome carried in res->status (mapped to a tenant
 * CQE on the front); a negative return is reserved for an internal contract
 * violation (res left zeroed/FAILED).
 */
int nkvx_executor_run(struct nkvx_executor *ex, const nkvx_exec_in_t *in,
		      struct nkvx_exec_result *res);

/**
 * Run ONE decoded base-layer KV verb (slice spdk-7sr.4 / S3). Currently the
 * RETRIEVE verb is implemented: resolve oid=hex(in->key), serve the value from the
 * shared TB4 object cache on a hit (zero librados) or rados_read + cold-fill it on a
 * miss, and populate \p res with the TRUE value length (CQE DW0 / truncation
 * semantics) and a freshly-allocated buffer of the min(result_len, osize) delivered
 * bytes. The caller (Mercury handler) delivers those bytes inline or via the
 * result_sink PUSH and frees them with nkvx_exec_result_free().
 *
 *   - key missing                     -> res->status = KEY_NOT_EXIST, no body.
 *   - value_len > osize (truncation)  -> res->status = BUFFER_TOO_SMALL, DW0 = true
 *                                        length, body = osize bytes.
 *   - other verbs (Store/Delete/Exist/List) -> NOT_SUPPORTED for now (S4).
 *
 * read_only is carried on the wire (ADR-0008) but Retrieve is non-mutating, so it is
 * always permitted. Always returns 0 with the outcome in res->status; a negative
 * return is reserved for an internal contract violation.
 */
int nkvx_executor_kv(struct nkvx_executor *ex, const nkvx_kv_in_t *in,
		     struct nkvx_exec_result *res);

#ifdef __cplusplus
}
#endif

#endif /* NKVX_EXECUTOR_H */
