/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * rados-nkvx: the sandboxed near-data Exec executor (TB1 walking skeleton).
 *
 * This is the NEW local-execution path mandated by ADR-0009: KV Exec runs in an
 * in-process sandboxed executor, NOT as a Ceph object-class in the OSD. The
 * existing rados_read_op_exec/cls path in kvdev_rados.c is deliberately left
 * untouched and is being phased out.
 *
 * TB1 scope (intentionally thin):
 *   - A CPU-issued KV Exec selects a hardcoded built-in module by op-ID (via a
 *     "nkvx:" binding prefix the NVMf allowlist forwards verbatim).
 *   - The object is cold-filled from RADOS via plain librados aio (the same
 *     harvest-on-the-SPDK-thread poller pattern the rest of the kvdev uses).
 *   - The module runs OFF the SPDK reactor thread on a dedicated worker, with
 *     async completion handed back to the originating SPDK thread. This
 *     off-reactor hand-off is the load-bearing piece (rados-nkvx-plan §3.2:
 *     "Off-reactor execution ... harder than the sandbox").
 *
 * Explicitly OUT of TB1 scope (later tracer bullets): allowlist/sidecar, caps
 * (fuel/epoch/memory), content-addressed raw-bdev cache, hash-verify,
 * warm-instance reuse, multi-executor distribution. (Today the front forwards
 * every Exec to a single configured executor endpoint with no per-object
 * routing; the planned model is mon/mgr-registered executor liveness + hash
 * across the live set, CRUSH vs Maglev TBD, route-to-primary rejected for EC.
 * See beads spdk-aee / docs/two-tier.md.)
 *
 * wasmtime SEAM: see kvdev_rados_nkvx_run_module(). TB1 runs a trivial built-in
 * in C because wasmtime is not yet linked into the SPDK build; the seam is
 * marked so TB2 can swap in a real (tiny) wasmtime invocation without disturbing
 * the off-reactor dispatch proven here.
 */

#ifndef SPDK_KVDEV_RADOS_NKVX_H
#define SPDK_KVDEV_RADOS_NKVX_H

#include "spdk/stdinc.h"

#include "kvdev_rados_nkvx_wasm.h"	/* struct kvdev_rados_nkvx_module */

/* Binding-string prefix that routes a KV Exec to the nkvx local executor instead
 * of the legacy cls path. The text after the prefix names the built-in module. */
#define KVDEV_RADOS_NKVX_BINDING_PREFIX "nkvx:"

/*
 * Legacy "nkvx:<module>" allowlist strings are decoded by the structured-binding
 * parser (nvmf_rpc_item_to_kv_exec_binding) as a cls binding with this module
 * namespace and module_key = <module>: e.g. "nkvx:bytecount" => runtime=cls,
 * module_namespace="nkvx", module_key="bytecount". kvdev_rados_exec recognises
 * this namespace and routes such a binding to the in-process executor (the
 * built-in C modules carry no untrusted bytes, so they need no sha256 anchor),
 * restoring the pre-structured-binding "nkvx:" routing for the built-ins.
 */
#define KVDEV_RADOS_NKVX_CLS_NAMESPACE "nkvx"

/* Built-in module names (TB1: statically bound, selected by the binding). */
#define KVDEV_RADOS_NKVX_MODULE_BYTECOUNT "bytecount"
#define KVDEV_RADOS_NKVX_MODULE_IDENTITY  "identity"

/*
 * Real-wasm module prefix (TB-WIMP / ADR-0013). A module name of the form
 * "wasm:<name>" routes the Exec to the dlopen-backed wasmtime runtime instead of
 * a C built-in. The full binding is therefore "nkvx:wasm:<name>"; <name> selects
 * a precompiled <name>.wasm from the nkvx wasm module directory (see
 * KVDEV_RADOS_NKVX_WASM_DIR_ENV).
 *
 * Graceful degradation (ADR-0013): when SPDK was built --without-wasm, or
 * libwasmtime.so cannot be dlopen'd at runtime, a wasm module fails with a
 * DISTINCT "runtime unavailable" status (SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED) —
 * never a crash. The C built-ins above always work regardless.
 */
#define KVDEV_RADOS_NKVX_MODULE_WASM_PREFIX "wasm:"

/* Env var naming the directory that holds precompiled <name>.wasm modules. The
 * e2e and unit tests set this; if unset the executor falls back to a path
 * relative to the SPDK build tree. */
#define KVDEV_RADOS_NKVX_WASM_DIR_ENV "SPDK_NKVX_WASM_DIR"

/*
 * Valgrind/Memcheck taint-clear, compiled in ONLY for the unit-test build
 * (-DSPDK_UNIT_TEST=1; see mk/spdk.unittest.mk). Production builds never define
 * SPDK_UNIT_TEST, so this collapses to nothing and the object code is byte-for-
 * byte identical to a build without it — SPDK proper carries no valgrind dep.
 *
 * Why it exists (spdk-5pq): the dlopen'd libwasmtime.so uses a setjmp/longjmp
 * trampoline whose unwind leaves Memcheck-"uninitialised" bytes in stack slots
 * the off-reactor worker frames then legitimately reuse. Reading them is a
 * BENIGN false positive, but its error stack is byte-identical to a real
 * uninitialised-read bug, so no .supp rule can silence it without also hiding a
 * genuine bug. We instead clear the taint at the source — right where the worker
 * frame re-reads the reused slot after the wasm call returns. VALGRIND_MAKE_MEM_
 * DEFINED is itself a no-op unless the binary actually runs under Memcheck, so
 * even the UT binary's normal (non-valgrind) runs are unaffected.
 */
#if defined(SPDK_UNIT_TEST) && defined(__has_include)
#if __has_include(<valgrind/memcheck.h>) && !defined(NVALGRIND)
#include <valgrind/memcheck.h>
#define NKVX_VALGRIND_MAKE_DEFINED(addr, len) \
	((void)VALGRIND_MAKE_MEM_DEFINED((addr), (len)))
/*
 * Clear the longjmp taint on ONE named local object. A frame-window heuristic
 * (clearing N bytes below __builtin_frame_address(0)) cannot be trusted: the
 * wasmtime setjmp/longjmp trampoline poisons whichever stack slots the unwound
 * frames happen to occupy, and those slots are NOT guaranteed to lie inside any
 * fixed window below the frame base (the compiler may place spilled scalars at or
 * above it, or in func_call's result region). Instead mark the EXACT object that
 * is read after the call: &var / sizeof(var) names precisely the address whose
 * read Memcheck flags, so the clear always covers the read it is meant to clear,
 * and nothing else. Every object cleared this way is one the function has already
 * (re)assigned a deterministic value before any meaningful use, so this changes
 * no behaviour — it only drops the benign false positive. It marks just that one
 * object, so a genuine uninitialised read of any OTHER nkvx local is still caught.
 */
#define NKVX_VALGRIND_MAKE_OBJ_DEFINED(var) \
	NKVX_VALGRIND_MAKE_DEFINED(&(var), sizeof(var))
#define NKVX_VG_HAVE 1
#endif
#endif
#ifndef NKVX_VALGRIND_MAKE_DEFINED
#define NKVX_VALGRIND_MAKE_DEFINED(addr, len) ((void)0)
#endif
#ifndef NKVX_VALGRIND_MAKE_OBJ_DEFINED
#define NKVX_VALGRIND_MAKE_OBJ_DEFINED(var) ((void)0)
#endif

/*
 * Re-define ONLY the compiler-owned slots at the top of THIS frame (stack-protector
 * canary + saved callee registers) after a wasm call: the wasmtime longjmp leaves a
 * benign Memcheck shadow on them and the epilogue then reads them. Implemented as a
 * noinline helper (in kvdev_rados_nkvx.c) so the marking runs from a clean frame.
 * It touches only a narrow top-of-frame band, NOT the locals -- a genuine
 * uninitialised read of a local is still reported.
 */
#if defined(NKVX_VG_HAVE)
void nkvx_vg_scrub_frame_top(void *frame_base);
#define NKVX_VALGRIND_SCRUB_FRAME_TOP() \
	nkvx_vg_scrub_frame_top(__builtin_frame_address(0))
#else
#define NKVX_VALGRIND_SCRUB_FRAME_TOP() ((void)0)
#endif

struct kvdev_rados_nkvx_worker;

/*
 * spdk-k3z: cross-consumer RESULT cache.
 *
 * A NEW, distinct cache from the sha256 compiled-module cache, the oid-keyed
 * content-addressed OBJECT cache (TB4), and the (module,oid) warm-instance cache.
 * It memoizes the COMPUTED RESULT of an Exec so that an identical (runtime, module
 * identity, object CONTENT identity, canonicalized request incl. input bytes)
 * issued by ANY consumer is served WITHOUT re-running the module off-reactor.
 *
 * Why object CONTENT identity (not the bare oid): an oid can be overwritten in
 * place (Store) — keying on the oid alone would return a STALE result for new
 * content. We key on a sha256 of the actual object bytes the module will run on,
 * consistent with the TB4 content-addressed object cache's invalidation-on-write
 * intent: different content => different key => MISS, never a stale hit.
 *
 * The lookup happens ON THE REACTOR, BEFORE the off-reactor dispatch (mirroring
 * the spdk-fbm cache-probe short-circuit), so a HIT completes with NO off-reactor
 * work. The insert happens on the reactor in the run completion after a miss.
 *
 * Disable knob: env var SPDK_NKVX_RESULT_CACHE=0 turns the cache OFF (every Exec
 * recomputes; results are identical, just not memoized). Read once, lazily.
 */
#define KVDEV_RADOS_NKVX_RESULT_CACHE_ENV "SPDK_NKVX_RESULT_CACHE"

/* LRU cap (entries) for the result cache; env-overridable. 0 disables the cap. */
#define KVDEV_RADOS_NKVX_RESULT_CACHE_MAX_COUNT_ENV "SPDK_NKVX_RESULT_CACHE_MAX_COUNT"
#define KVDEV_RADOS_NKVX_RESULT_CACHE_MAX_COUNT_DEFAULT 1024u

/* sha256 digest length reused for all result-cache hashes. */
#include "spdk/kvdev.h"		/* SPDK_KV_EXEC_SHA256_LEN */

/*
 * A fully-formed result-cache key. The 32-byte digest is computed by
 * kvdev_rados_nkvx_result_key from the canonical component stream
 * (runtime/module/module-sha256/object-content-sha256/input). The caller
 * (reactor side) builds it once it has the object bytes in hand and uses it for
 * both the probe and (on a miss) the insert.
 */
struct kvdev_rados_nkvx_result_key {
	uint8_t digest[SPDK_KV_EXEC_SHA256_LEN];
};

/*
 * Build the canonical result-cache key digest from its components. All inputs are
 * folded length-prefixed into a single sha256 so logically-identical requests
 * collide and logically-different ones (different module, module hash, object
 * content, or input) do not.
 *
 * \param module        module name (the text after "nkvx:", e.g. "bytecount" or
 *                       "wasm:checksum"); also encodes the runtime (wasm: prefix).
 * \param mod_sha256    bound module content hash (32 B) for verified wasm, or NULL
 *                       for built-in C modules (which carry no untrusted bytes).
 * \param object        object bytes the module will run on (content identity).
 * \param object_len    length of \c object.
 * \param input         per-request input bytes (may be NULL).
 * \param input_len     length of \c input.
 * \param out_key       receives the computed key.
 *
 * Returns 0 on success, negative errno on a digest failure (caller then treats it
 * as "uncacheable" and just recomputes — never a wrong answer).
 */
int kvdev_rados_nkvx_result_key(const char *module,
				const uint8_t *mod_sha256,
				const void *object, size_t object_len,
				const void *input, size_t input_len,
				struct kvdev_rados_nkvx_result_key *out_key);

/*
 * Probe the result cache. Runs ON THE REACTOR before dispatch. On a HIT, copies
 * the memoized result into \c out (up to \c out_len), sets \c *result_len to the
 * TRUE result length and \c *kvstatus to the memoized status, and returns true —
 * the caller completes the Exec immediately with NO off-reactor work. On a MISS
 * (or when the cache is disabled) returns false and touches nothing.
 */
bool kvdev_rados_nkvx_result_lookup(const struct kvdev_rados_nkvx_result_key *key,
				    void *out, uint32_t out_len,
				    uint32_t *result_len, int *kvstatus);

/*
 * Insert a computed result under \c key. Runs ON THE REACTOR in the run
 * completion after a miss. Only SUCCESS / BUFFER_TOO_SMALL results (deterministic
 * outcomes of running the module) are memoized with their bytes; transient
 * failures are never cached. No-op when the cache is disabled. \c result is the
 * TRUE result the module produced (\c result_bytes may be shorter if the host
 * buffer truncated it — only the bytes actually present are stored, with the true
 * length recorded so a later hit reports the same truncation).
 */
void kvdev_rados_nkvx_result_insert(const struct kvdev_rados_nkvx_result_key *key,
				    int kvstatus, uint32_t result_len,
				    const void *result_bytes, uint32_t result_bytes_len);

/* True iff the result cache is enabled (env knob). Read-once, lazily. */
bool kvdev_rados_nkvx_result_cache_enabled(void);

/* Drop every result-cache entry (test teardown / shutdown). */
void kvdev_rados_nkvx_result_cache_reset(void);

/*
 * Test/observability counters for the result cache. hits/misses drive the
 * measured hit-rate; inserts/evictions prove bounded growth.
 */
struct kvdev_rados_nkvx_result_stats {
	uint64_t	hits;
	uint64_t	misses;
	uint64_t	inserts;
	uint64_t	evictions;
};
void kvdev_rados_nkvx_result_get_stats(struct kvdev_rados_nkvx_result_stats *out);

/*
 * Start/stop the executor worker pool (one dedicated pthread in TB1). Called
 * from module_init/module_fini. Idempotent.
 */
int kvdev_rados_nkvx_start(void);
void kvdev_rados_nkvx_stop(void);

/*
 * Dispatch a built-in module run OFF the SPDK reactor.
 *
 * \param module      built-in module name (after the "nkvx:" prefix).
 * \param mod         OPTIONAL verified module binding (TB3 / spdk-fbm): the bound
 *                    sha256, the librados-fetched .wasm bytes (fetch-miss path), and
 *                    the per-invocation caps word. NULL on the legacy built-in /
 *                    in-tree-test path; non-NULL on the live wasm datapath, where it
 *                    is the deny-by-default authorization anchor (only bytes that
 *                    hash to mod->sha256 are ever compiled/run). \c mod->bytes must
 *                    outlive the run (the caller frees them after done_fn fires).
 * \param obj_key     stable identity key for the object (the oid). Keys the
 *                    executor's identity-addressed object cache and, with the
 *                    module, the warm-instance cache (TB4 / spdk-ii0). May be NULL
 *                    or "" to bypass the cache (plain-copy run).
 * \param obj_pin     OPTIONAL pinned cache handle from
 *                    kvdev_rados_nkvx_wasm_cache_pin (probe-hit on the datapath,
 *                    spdk-ii0 D2). When non-NULL the worker serves exactly this
 *                    pinned object version (no librados refetch, race-safe against
 *                    a concurrent Store/Delete invalidation) and releases the pin
 *                    when the run finishes. NULL on the cold-read path.
 * \param object      object bytes cold-filled from RADOS (owned by caller; must
 *                    remain valid until done_fn fires). Ignored when obj_pin set.
 * \param object_len  length of \c object.
 * \param out         host output buffer to receive the module result.
 * \param out_len     capacity of \c out.
 * \param done_fn     completion, invoked ON the originating SPDK thread with the
 *                    kvdev status and the true result length.
 * \param done_arg    opaque context for \c done_fn.
 *
 * Returns 0 if the job was queued (done_fn will fire later), or negative errno
 * if it could not be queued (done_fn will NOT fire).
 */
typedef void (*kvdev_rados_nkvx_done_fn)(void *done_arg, int kvstatus, uint32_t out_len);

int kvdev_rados_nkvx_dispatch(const char *module, const struct kvdev_rados_nkvx_module *mod,
			      const char *obj_key, void *obj_pin,
			      const void *object, size_t object_len,
			      void *out, uint32_t out_len,
			      kvdev_rados_nkvx_done_fn done_fn, void *done_arg);

/*
 * Compile-completion callback for kvdev_rados_nkvx_dispatch_compile. Invoked ON
 * the originating SPDK thread with the module-insert status (an
 * SPDK_KVDEV_IO_STATUS_* code: SUCCESS once the compiled artifact is cached,
 * FAILED/NOT_SUPPORTED on a compile/runtime failure).
 */
typedef void (*kvdev_rados_nkvx_compiled_fn)(void *done_arg, int kvstatus);

/*
 * Dispatch the one-time Cranelift compile of a VERIFIED module OFF the SPDK
 * reactor (spdk-5wi). The reactor side has ALREADY run the integrity gate
 * (kvdev_rados_nkvx_wasm_module_verify) so the bytes are blessed; this hands the
 * compile to the executor worker — which calls kvdev_rados_nkvx_wasm_module_insert
 * (verify is idempotent there) to compile + cache the artifact by hash — so the
 * compile never head-of-line-blocks the poller. The result is handed back with
 * done_fn ON the originating SPDK thread, exactly like the run dispatch.
 *
 * \param sha256      bound content hash; copied by value.
 * \param bytes       the verified .wasm bytes. Caller-owned; MUST remain valid
 *                    until done_fn fires (the worker only reads them).
 * \param bytes_len   length of \c bytes.
 * \param done_fn     completion, invoked ON the originating SPDK thread.
 * \param done_arg    opaque context for \c done_fn.
 *
 * Returns 0 if the job was queued (done_fn will fire later), or negative errno if
 * it could not be queued (done_fn will NOT fire).
 */
int kvdev_rados_nkvx_dispatch_compile(const uint8_t sha256[SPDK_KV_EXEC_SHA256_LEN],
				      const void *bytes, size_t bytes_len,
				      kvdev_rados_nkvx_compiled_fn done_fn, void *done_arg);

#endif /* SPDK_KVDEV_RADOS_NKVX_H */
