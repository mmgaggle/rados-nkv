/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/kvdev.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/queue.h"

#include "kvdev_rados_nkvx.h"
#include "kvdev_rados_nkvx_wasm.h"

#include <openssl/evp.h>

/*
 * rados-nkvx executor worker (TB1). See kvdev_rados_nkvx.h for scope.
 *
 * Off-reactor dispatch, the load-bearing piece:
 *   - One dedicated pthread (a pool of one in TB1) consumes a mutex+cond work
 *     queue. KV Exec module runs happen there, NEVER on the polled SPDK reactor.
 *   - Each job records the SPDK thread that submitted it (spdk_get_thread()).
 *     When the worker finishes the module, it hands the result back with
 *     spdk_thread_send_msg(origin, ...), so the kvdev completion fires on the
 *     ORIGINATING SPDK thread — never from the worker thread, mirroring how the
 *     librados poller keeps completion delivery on the SPDK thread.
 *
 * This is exactly the impedance match the plan calls the hardest part of slice
 * #1: a multi-hundred-µs module run must not head-of-line-block co-resident
 * Retrieve traffic on the reactor.
 */

/*
 * A worker job is either a module RUN (the original off-reactor compute) or a
 * one-time module COMPILE (spdk-5wi): the verified-hash gate stays on the reactor,
 * but the Cranelift compile of a sha256-cache miss is handed here so it never
 * head-of-line-blocks the poller. Both kinds share the queue, the origin-thread
 * hand-back, and the off-reactor-proof tid capture.
 */
enum kvdev_rados_nkvx_job_kind {
	NKVX_JOB_RUN = 0,
	NKVX_JOB_COMPILE,
};

struct kvdev_rados_nkvx_job {
	enum kvdev_rados_nkvx_job_kind	kind;
	char				module[32];
	char				obj_key[256];	/* identity key (oid) for the cache */
	void				*obj_pin;	/* pinned cache handle (spdk-ii0 D2) or NULL */
	/*
	 * TB3 (spdk-fbm): the verified module binding for this Exec. has_mod tracks
	 * whether a real binding was supplied (the live datapath always supplies one;
	 * the in-tree TB1/TB2 dispatch tests do not). mod.bytes points at the
	 * librados-fetched .wasm carried on the io; it is valid for the run's
	 * lifetime (the reactor frees it after the worker completes). The 32-byte
	 * sha256 / caps are copied by value.
	 */
	bool				has_mod;
	struct kvdev_rados_nkvx_module	mod;
	const void			*object;
	size_t				object_len;
	void				*out;
	uint32_t			out_len;
	kvdev_rados_nkvx_done_fn	done_fn;
	void				*done_arg;
	struct spdk_thread		*origin;	/* SPDK thread to complete on */
	pthread_t			submit_tid;	/* OS thread that submitted (reactor) */

	/*
	 * NKVX_JOB_COMPILE only (spdk-5wi): the verified module bytes to compile +
	 * cache. sha256 is copied by value; bytes points at caller-owned memory that
	 * MUST outlive the job (the io frees it after compiled_fn fires on the
	 * reactor). compiled_fn is the origin-thread completion.
	 */
	uint8_t				compile_sha256[SPDK_KV_EXEC_SHA256_LEN];
	const void			*compile_bytes;
	size_t				compile_bytes_len;
	kvdev_rados_nkvx_compiled_fn	compiled_fn;

	/* Filled in by the worker, read back on the SPDK thread. */
	int				kvstatus;
	uint32_t			result_len;
	pthread_t			run_tid;	/* OS thread the module ACTUALLY ran on */

	STAILQ_ENTRY(kvdev_rados_nkvx_job) link;
};

static struct {
	pthread_t			tid;
	pthread_mutex_t			mutex;
	pthread_cond_t			cond;
	STAILQ_HEAD(, kvdev_rados_nkvx_job) queue;
	bool				running;
	bool				stop;
} g_nkvx = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,
};

/* ==========================================================================
 * spdk-k3z: cross-consumer RESULT cache.
 *
 * Distinct from the compiled-module (sha256), object (TB4), and warm-instance
 * caches. Keyed by a sha256 over (runtime+module || module-content-sha256 ||
 * object-content-sha256 || input) so that an identical request from ANY consumer
 * is served from the memoized COMPUTED RESULT with no off-reactor module run.
 *
 * Concurrency: the lookup/insert/reset all run ON THE REACTOR (reactor-side
 * probe-before-dispatch + reactor-side insert in the run completion), but the
 * cache carries its own mutex anyway so the deterministic off-reactor unit test
 * (which drives it from a single thread) and any future multi-reactor caller stay
 * race-free.
 * ========================================================================== */

struct nkvx_result_entry {
	struct kvdev_rados_nkvx_result_key	key;
	int					kvstatus;
	uint32_t				result_len;	/* TRUE module result length */
	uint32_t				bytes_len;	/* bytes actually stored (<= result_len) */
	uint8_t					*bytes;		/* malloc'd copy of the result */
	TAILQ_ENTRY(nkvx_result_entry)		link;		/* LRU: head=oldest, tail=newest */
};

static struct {
	pthread_mutex_t				mutex;
	TAILQ_HEAD(, nkvx_result_entry)		entries;
	uint64_t				count;
	uint64_t				max_count;
	bool					inited;
	/* Disable knob, read once from the env. */
	int					enabled;	/* -1 unknown, 0 off, 1 on */
	struct kvdev_rados_nkvx_result_stats	stats;
} g_nkvx_result = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.enabled = -1,
};

/* Lazily read the env knobs (enabled + cap). Caller holds g_nkvx_result.mutex. */
static void
nkvx_result_init_once(void)
{
	const char *env;

	if (g_nkvx_result.inited) {
		return;
	}
	TAILQ_INIT(&g_nkvx_result.entries);

	g_nkvx_result.enabled = 1;
	env = getenv(KVDEV_RADOS_NKVX_RESULT_CACHE_ENV);
	if (env != NULL && (env[0] == '0' || env[0] == 'n' || env[0] == 'N' ||
			    env[0] == 'f' || env[0] == 'F')) {
		g_nkvx_result.enabled = 0;
	}

	g_nkvx_result.max_count = KVDEV_RADOS_NKVX_RESULT_CACHE_MAX_COUNT_DEFAULT;
	env = getenv(KVDEV_RADOS_NKVX_RESULT_CACHE_MAX_COUNT_ENV);
	if (env != NULL) {
		char *end = NULL;
		unsigned long long v = strtoull(env, &end, 10);

		if (end != env) {
			g_nkvx_result.max_count = (uint64_t)v;	/* 0 => unbounded */
		}
	}
	g_nkvx_result.inited = true;
}

bool
kvdev_rados_nkvx_result_cache_enabled(void)
{
	bool en;

	pthread_mutex_lock(&g_nkvx_result.mutex);
	nkvx_result_init_once();
	en = g_nkvx_result.enabled == 1;
	pthread_mutex_unlock(&g_nkvx_result.mutex);
	return en;
}

void
kvdev_rados_nkvx_result_get_stats(struct kvdev_rados_nkvx_result_stats *out)
{
	if (out == NULL) {
		return;
	}
	pthread_mutex_lock(&g_nkvx_result.mutex);
	nkvx_result_init_once();
	*out = g_nkvx_result.stats;
	pthread_mutex_unlock(&g_nkvx_result.mutex);
}

void
kvdev_rados_nkvx_result_cache_reset(void)
{
	struct nkvx_result_entry *e;

	pthread_mutex_lock(&g_nkvx_result.mutex);
	nkvx_result_init_once();
	while ((e = TAILQ_FIRST(&g_nkvx_result.entries)) != NULL) {
		TAILQ_REMOVE(&g_nkvx_result.entries, e, link);
		free(e->bytes);
		free(e);
	}
	g_nkvx_result.count = 0;
	memset(&g_nkvx_result.stats, 0, sizeof(g_nkvx_result.stats));
	/*
	 * Re-read the env knobs on reset so a test (or a runtime reconfigure) can
	 * toggle the disable knob / cap and have it take effect at a clean boundary.
	 * Production only ever resets at teardown, so this is harmless there.
	 */
	g_nkvx_result.inited = false;
	nkvx_result_init_once();
	pthread_mutex_unlock(&g_nkvx_result.mutex);
}

/*
 * Fold one length-prefixed component into the running digest. The 8-byte LE
 * length prefix makes the encoding injective: no concatenation of one field can
 * be mistaken for a different split of the components, so logically-different
 * requests cannot collide by aliasing field boundaries.
 */
static int
nkvx_result_hash_update(EVP_MD_CTX *ctx, const void *buf, size_t len)
{
	uint64_t le = (uint64_t)len;
	uint8_t lenbuf[8];
	int i;

	for (i = 0; i < 8; i++) {
		lenbuf[i] = (uint8_t)(le >> (8 * i));
	}
	if (EVP_DigestUpdate(ctx, lenbuf, sizeof(lenbuf)) != 1) {
		return -1;
	}
	if (len > 0 && buf != NULL && EVP_DigestUpdate(ctx, buf, len) != 1) {
		return -1;
	}
	return 0;
}

int
kvdev_rados_nkvx_result_key(const char *module,
			    const uint8_t *mod_sha256,
			    const void *object, size_t object_len,
			    const void *input, size_t input_len,
			    struct kvdev_rados_nkvx_result_key *out_key)
{
	EVP_MD_CTX *ctx;
	uint8_t obj_hash[SPDK_KV_EXEC_SHA256_LEN];
	uint8_t mod_marker;
	unsigned int mdlen = 0;
	int rc = -EIO;

	if (module == NULL || out_key == NULL) {
		return -EINVAL;
	}

	/*
	 * Object CONTENT identity: hash the actual bytes (not the oid). This is the
	 * load-bearing correctness property — an oid overwritten with different
	 * content yields a different object hash, so a result computed over the old
	 * content can never be returned for the new content (no stale hit).
	 */
	if (EVP_Digest(object_len > 0 ? object : "", object_len, obj_hash, &mdlen,
		       EVP_sha256(), NULL) != 1 || mdlen != SPDK_KV_EXEC_SHA256_LEN) {
		return -EIO;
	}

	ctx = EVP_MD_CTX_new();
	if (ctx == NULL) {
		return -ENOMEM;
	}
	if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
		goto out;
	}

	/* runtime + module name (the "wasm:" prefix distinguishes the runtime). */
	if (nkvx_result_hash_update(ctx, module, strlen(module)) != 0) {
		goto out;
	}
	/*
	 * Module IDENTITY: the bound content sha256 for verified wasm. Built-in C
	 * modules carry no untrusted bytes (their identity IS their name, already
	 * folded above); mark that distinctly so a built-in and a hypothetical wasm
	 * module of the same name never alias.
	 */
	mod_marker = mod_sha256 != NULL ? 1 : 0;
	if (nkvx_result_hash_update(ctx, &mod_marker, 1) != 0) {
		goto out;
	}
	if (mod_sha256 != NULL &&
	    nkvx_result_hash_update(ctx, mod_sha256, SPDK_KV_EXEC_SHA256_LEN) != 0) {
		goto out;
	}
	/* object content identity */
	if (nkvx_result_hash_update(ctx, obj_hash, sizeof(obj_hash)) != 0) {
		goto out;
	}
	/* canonicalized request: the per-invocation input bytes */
	if (nkvx_result_hash_update(ctx, input, input_len) != 0) {
		goto out;
	}

	if (EVP_DigestFinal_ex(ctx, out_key->digest, &mdlen) != 1 ||
	    mdlen != SPDK_KV_EXEC_SHA256_LEN) {
		goto out;
	}
	rc = 0;
out:
	EVP_MD_CTX_free(ctx);
	return rc;
}

/* Caller holds the mutex. Linear scan (keys are 32 B, cache is small/bounded). */
static struct nkvx_result_entry *
nkvx_result_lookup_locked(const struct kvdev_rados_nkvx_result_key *key)
{
	struct nkvx_result_entry *e;

	TAILQ_FOREACH(e, &g_nkvx_result.entries, link) {
		if (memcmp(e->key.digest, key->digest, SPDK_KV_EXEC_SHA256_LEN) == 0) {
			return e;
		}
	}
	return NULL;
}

bool
kvdev_rados_nkvx_result_lookup(const struct kvdev_rados_nkvx_result_key *key,
			       void *out, uint32_t out_len,
			       uint32_t *result_len, int *kvstatus)
{
	struct nkvx_result_entry *e;

	if (key == NULL) {
		return false;
	}

	pthread_mutex_lock(&g_nkvx_result.mutex);
	nkvx_result_init_once();
	if (g_nkvx_result.enabled != 1) {
		pthread_mutex_unlock(&g_nkvx_result.mutex);
		return false;
	}

	e = nkvx_result_lookup_locked(key);
	if (e == NULL) {
		g_nkvx_result.stats.misses++;
		pthread_mutex_unlock(&g_nkvx_result.mutex);
		return false;
	}

	/* LRU touch: move to the tail (most-recently-used). */
	TAILQ_REMOVE(&g_nkvx_result.entries, e, link);
	TAILQ_INSERT_TAIL(&g_nkvx_result.entries, e, link);

	/* Reproduce the memoized outcome EXACTLY, including any truncation: copy the
	 * stored bytes (which fit the original host buffer) clamped to this caller's
	 * buffer, but always report the TRUE result length so truncation is detected
	 * identically to a recompute. */
	if (result_len != NULL) {
		*result_len = e->result_len;
	}
	if (kvstatus != NULL) {
		*kvstatus = e->kvstatus;
	}
	if (out != NULL && out_len > 0 && e->bytes_len > 0) {
		uint32_t copy = e->bytes_len < out_len ? e->bytes_len : out_len;

		memcpy(out, e->bytes, copy);
	}
	g_nkvx_result.stats.hits++;
	pthread_mutex_unlock(&g_nkvx_result.mutex);
	return true;
}

void
kvdev_rados_nkvx_result_insert(const struct kvdev_rados_nkvx_result_key *key,
			       int kvstatus, uint32_t result_len,
			       const void *result_bytes, uint32_t result_bytes_len)
{
	struct nkvx_result_entry *e;
	uint8_t *copy = NULL;

	if (key == NULL) {
		return;
	}
	/* Only memoize DETERMINISTIC outcomes of actually running the module. A
	 * transient/environmental failure (NOMEM, runtime unavailable, ...) must NOT
	 * be cached as if it were the module's answer. */
	if (kvstatus != SPDK_KVDEV_IO_STATUS_SUCCESS &&
	    kvstatus != SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL) {
		return;
	}

	pthread_mutex_lock(&g_nkvx_result.mutex);
	nkvx_result_init_once();
	if (g_nkvx_result.enabled != 1) {
		pthread_mutex_unlock(&g_nkvx_result.mutex);
		return;
	}

	/* Idempotent: a concurrent/duplicate insert just refreshes LRU position. */
	e = nkvx_result_lookup_locked(key);
	if (e != NULL) {
		TAILQ_REMOVE(&g_nkvx_result.entries, e, link);
		TAILQ_INSERT_TAIL(&g_nkvx_result.entries, e, link);
		pthread_mutex_unlock(&g_nkvx_result.mutex);
		return;
	}

	if (result_bytes_len > 0 && result_bytes != NULL) {
		copy = malloc(result_bytes_len);
		if (copy == NULL) {
			/* Out of memory: skip caching, never a wrong answer. */
			pthread_mutex_unlock(&g_nkvx_result.mutex);
			return;
		}
		memcpy(copy, result_bytes, result_bytes_len);
	}

	e = calloc(1, sizeof(*e));
	if (e == NULL) {
		free(copy);
		pthread_mutex_unlock(&g_nkvx_result.mutex);
		return;
	}
	e->key = *key;
	e->kvstatus = kvstatus;
	e->result_len = result_len;
	e->bytes = copy;
	e->bytes_len = copy != NULL ? result_bytes_len : 0;
	TAILQ_INSERT_TAIL(&g_nkvx_result.entries, e, link);
	g_nkvx_result.count++;
	g_nkvx_result.stats.inserts++;

	/* LRU eviction from the head (oldest) when past the cap (0 == unbounded). */
	while (g_nkvx_result.max_count != 0 && g_nkvx_result.count > g_nkvx_result.max_count) {
		struct nkvx_result_entry *victim = TAILQ_FIRST(&g_nkvx_result.entries);

		if (victim == NULL) {
			break;
		}
		TAILQ_REMOVE(&g_nkvx_result.entries, victim, link);
		free(victim->bytes);
		free(victim);
		g_nkvx_result.count--;
		g_nkvx_result.stats.evictions++;
	}
	pthread_mutex_unlock(&g_nkvx_result.mutex);
}

/*
 * spdk-5pq: valgrind-only longjmp-taint scrubber for the small COMPILER-OWNED slots
 * at the top of a frame -- the stack-protector canary and saved callee-registers
 * that sit just below the saved frame pointer. libwasmtime's setjmp/longjmp leaves
 * a benign Memcheck shadow on these (they are written in the prologue, before the
 * wasm call, and the longjmp's stack churn re-poisons them), and the function's
 * epilogue then reads them, producing a BENIGN uninit-value report indistinguishable
 * by stack from a real bug. We re-define ONLY that narrow top-of-frame band; the
 * function's actual locals sit far below it (and are cleared by name elsewhere), so
 * a genuine uninitialised read of a local is still reported. This is a SEPARATE
 * noinline function so the marking client request issues from a clean frame and
 * targets the caller's frame by absolute address.
 *
 * Defined ONLY for the valgrind unit-test build (NKVX_VG_HAVE); production never
 * references it (NKVX_VALGRIND_SCRUB_FRAME_TOP() is ((void)0) there) and does not
 * even emit the symbol, so production object code is unaffected.
 */
#if defined(NKVX_VG_HAVE)
__attribute__((noinline)) void
nkvx_vg_scrub_frame_top(void *frame_base)
{
	/* Canary + saved registers occupy a small band just below the saved frame
	 * pointer; the saved %rbp / return address sit at/just above it. Cover
	 * [frame_base - 64, frame_base + 16): the compiler's spill/canary slots only,
	 * never the locals far below. */
	NKVX_VALGRIND_MAKE_DEFINED((char *)frame_base - 64, 64 + 16);
#if defined(__x86_64__) && defined(__GLIBC__)
	/*
	 * The stack-protector epilogue compares the (now-defined) canary slot against
	 * the master guard at %fs:0x28 (glibc/x86-64 TLS). The wasmtime longjmp leaves
	 * a benign Memcheck shadow on that TLS word too, so the compare is flagged even
	 * with a clean canary slot. Re-define just those 8 bytes via the thread pointer.
	 */
	NKVX_VALGRIND_MAKE_DEFINED((char *)__builtin_thread_pointer() + 0x28,
				   sizeof(void *));
#endif
	(void)frame_base;
}
#endif /* NKVX_VG_HAVE */

/*
 * wasmtime SEAM (TB1).
 *
 * Run one built-in module against the cold-filled object bytes and write the
 * result into the host output buffer. This runs ON THE WORKER THREAD.
 *
 * TB1 implements the two trivial built-ins directly in C:
 *   - "bytecount": result is the object length as a little-endian uint64_t (8B).
 *   - "identity":  result is the object bytes copied through, truncated to the
 *                  host output buffer (true length reported so the host can
 *                  detect truncation, matching Retrieve semantics).
 *
 * TB2 replaces the body below with a real (tiny) wasmtime invocation:
 *   wasm_engine_new -> wasmtime_module_new(precompiled .wasm) ->
 *   wasmtime_instance_new (no WASI imports) -> call the module export with the
 *   object in linear memory -> copy the Arrow/raw result out. The object/out
 *   contract and the off-reactor threading proven here do not change — only this
 *   function's body does. Marked clearly so the swap is local.
 */
/*
 * Cold-fill shim (TB4): the executor's content-addressed cache invokes this only
 * on a MISS to populate the cache slot. Here the bytes were already cold-filled
 * from RADOS upstream (kvdev_rados.c) and are carried on the job; we copy them
 * into the cache's page-rounded buffer. On a HIT the cache reuses its buffer and
 * this is NOT called — so a repeat Exec of the same object skips re-instantiation
 * and is served zero-copy from the warm instance.
 */
struct nkvx_fill_src {
	const void	*object;
	size_t		object_len;
};

static int
kvdev_rados_nkvx_fill_from_job(void *buf, size_t cap, size_t *out_len, void *arg)
{
	struct nkvx_fill_src *src = arg;

	if (src->object_len > cap) {
		return -1;
	}
	if (src->object_len > 0 && src->object != NULL) {
		memcpy(buf, src->object, src->object_len);
	}
	*out_len = src->object_len;
	return 0;
}

static int
kvdev_rados_nkvx_run_module(const char *module, const struct kvdev_rados_nkvx_module *mod,
			    const char *obj_key, void *obj_pin,
			    const void *object, size_t object_len,
			    void *out, uint32_t out_len, uint32_t *result_len)
{
	/*
	 * Real-wasm path. A module named "wasm:<name>" routes to the dlopen-backed
	 * wasmtime runtime. TB4 (ADR-0013):
	 *   - obj_pin set: the datapath probe-hit ALREADY pinned this object version
	 *     (spdk-ii0 D2). Serve it directly via run_pinned — no librados refetch,
	 *     race-safe against a concurrent Store/Delete invalidation — then release
	 *     the pin. This is the warm/served-locally path.
	 *   - obj_key set, no pin: cache miss probe; run through the identity cache
	 *     with a cold-fill callback (cold-fill-once, served locally thereafter).
	 *   - no key: plain-copy run (or the --without-wasm stub).
	 * If the runtime is unavailable this returns NOT_SUPPORTED (never a crash);
	 * the C built-ins below remain fully functional regardless.
	 */
	if (strncmp(module, KVDEV_RADOS_NKVX_MODULE_WASM_PREFIX,
		    strlen(KVDEV_RADOS_NKVX_MODULE_WASM_PREFIX)) == 0) {
		const char *name = module + strlen(KVDEV_RADOS_NKVX_MODULE_WASM_PREFIX);

		int rc;

		if (obj_pin != NULL) {
			rc = kvdev_rados_nkvx_wasm_run_pinned(name, mod, obj_pin,
							      out, out_len, result_len);

			kvdev_rados_nkvx_wasm_cache_unpin(obj_pin);
		} else if (obj_key != NULL && obj_key[0] != '\0') {
			struct nkvx_fill_src src = { .object = object, .object_len = object_len };

			rc = kvdev_rados_nkvx_wasm_run_cached(name, mod, obj_key, object_len,
							      kvdev_rados_nkvx_fill_from_job, &src,
							      out, out_len, result_len);
		} else {
			rc = kvdev_rados_nkvx_wasm_run(name, mod, object, object_len, out, out_len,
						       result_len);
		}
		/*
		 * spdk-5pq: the wasm executor above ran a wasmtime setjmp/longjmp; its
		 * unwind leaves a BENIGN Memcheck shadow on this dispatcher frame's
		 * stack-protector canary, which the epilogue then reads. Clear that narrow
		 * compiler-owned band (not the locals). No-op outside the valgrind UT.
		 */
		NKVX_VALGRIND_SCRUB_FRAME_TOP();
		return rc;
	}

	/* A pin on a non-wasm built-in path: release it, the built-ins recompute
	 * over the carried object bytes (or empty) and never touch the cache. */
	if (obj_pin != NULL) {
		kvdev_rados_nkvx_wasm_cache_unpin(obj_pin);
	}

	if (strcmp(module, KVDEV_RADOS_NKVX_MODULE_BYTECOUNT) == 0) {
		uint64_t count = (uint64_t)object_len;
		uint32_t copy = (uint32_t)spdk_min(sizeof(count), (size_t)out_len);

		/* Always report the true result length (8 bytes); copy what fits. */
		if (copy > 0 && out != NULL) {
			memcpy(out, &count, copy);
		}
		*result_len = (uint32_t)sizeof(count);
		return sizeof(count) > out_len ?
		       SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL :
		       SPDK_KVDEV_IO_STATUS_SUCCESS;
	}

	if (strcmp(module, KVDEV_RADOS_NKVX_MODULE_IDENTITY) == 0) {
		uint32_t copy = (uint32_t)spdk_min(object_len, (size_t)out_len);

		if (copy > 0 && out != NULL && object != NULL) {
			memcpy(out, object, copy);
		}
		*result_len = (uint32_t)spdk_min(object_len, (size_t)UINT32_MAX);
		return object_len > out_len ?
		       SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL :
		       SPDK_KVDEV_IO_STATUS_SUCCESS;
	}

	/* Unknown built-in: no static binding for this op-ID. */
	*result_len = 0;
	return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
}

/* Runs ON THE ORIGINATING SPDK THREAD: deliver the result and free the job. */
static void
kvdev_rados_nkvx_complete_on_spdk(void *ctx)
{
	struct kvdev_rados_nkvx_job *job = ctx;

	/*
	 * Off-reactor PROOF (deterministic, not timing-based). This callback runs
	 * back on the originating SPDK reactor thread, so pthread_self() here is the
	 * reactor's OS thread. job->run_tid is the OS thread the module body actually
	 * ran on (captured inside the worker). The two MUST differ: that is the
	 * load-bearing evidence the compute did NOT execute on the polled reactor.
	 * A functional test greps these and asserts run_tid != reactor_tid.
	 */
	pthread_t reactor_tid = pthread_self();

	SPDK_NOTICELOG("nkvx: off-reactor proof %s=%s reactor_tid=0x%lx "
		       "run_tid=0x%lx off_reactor=%s\n",
		       job->kind == NKVX_JOB_COMPILE ? "compile" : "module",
		       job->module,
		       (unsigned long)reactor_tid,
		       (unsigned long)job->run_tid,
		       pthread_equal(reactor_tid, job->run_tid) ? "NO" : "YES");

	if (job->kind == NKVX_JOB_COMPILE) {
		job->compiled_fn(job->done_arg, job->kvstatus);
	} else {
		job->done_fn(job->done_arg, job->kvstatus, job->result_len);
	}
	free(job);
}

static void *
kvdev_rados_nkvx_worker_main(void *arg)
{
	(void)arg;

	pthread_mutex_lock(&g_nkvx.mutex);
	while (true) {
		struct kvdev_rados_nkvx_job *job;

		while (STAILQ_EMPTY(&g_nkvx.queue) && !g_nkvx.stop) {
			pthread_cond_wait(&g_nkvx.cond, &g_nkvx.mutex);
		}
		if (g_nkvx.stop && STAILQ_EMPTY(&g_nkvx.queue)) {
			break;
		}
		job = STAILQ_FIRST(&g_nkvx.queue);
		STAILQ_REMOVE_HEAD(&g_nkvx.queue, link);
		pthread_mutex_unlock(&g_nkvx.mutex);

		/* Record the OS thread the module body runs on. This is THIS worker
		 * thread, distinct from the SPDK reactor that submitted the job; the
		 * SPDK-thread completion (kvdev_rados_nkvx_complete_on_spdk) compares it
		 * against the reactor's thread id as deterministic off-reactor proof. */
		job->run_tid = pthread_self();

		/* The actual off-reactor compute. A COMPILE job runs the one-time
		 * Cranelift compile + cache-insert here (spdk-5wi); the reactor already
		 * gated the bytes against the bound hash, and module_insert is idempotent
		 * and self-synchronized (its own modcache mutex), so a concurrent run that
		 * also misses the same hash races safely into the same cache slot. */
		if (job->kind == NKVX_JOB_COMPILE) {
			job->kvstatus = kvdev_rados_nkvx_wasm_module_insert(job->compile_sha256,
					job->compile_bytes, job->compile_bytes_len);
		} else {
			job->kvstatus = kvdev_rados_nkvx_run_module(job->module,
					job->has_mod ? &job->mod : NULL, job->obj_key,
					job->obj_pin, job->object, job->object_len, job->out,
					job->out_len, &job->result_len);
		}

		/*
		 * spdk-5pq: the wasm run above reaches libwasmtime's setjmp/longjmp
		 * trampoline; its unwind leaves a BENIGN Memcheck shadow on the reused
		 * `job` pointer and on this frame's stack-protector canary, both read
		 * below / at function exit. Clear the reused `job` by name, and the
		 * compiler-owned canary slot via the narrow top-of-frame scrubber. Both
		 * hold deterministic values here, so this preserves behaviour and only
		 * drops the false positive; a genuine uninitialised read of any other
		 * local is still reported. No-op outside the valgrind UT (see header).
		 */
		NKVX_VALGRIND_MAKE_OBJ_DEFINED(job);
		NKVX_VALGRIND_SCRUB_FRAME_TOP();

		/* Hand the result back to the SPDK thread that submitted it; the
		 * kvdev completion fires there, never on this worker thread. */
		spdk_thread_send_msg(job->origin, kvdev_rados_nkvx_complete_on_spdk, job);

		pthread_mutex_lock(&g_nkvx.mutex);
	}
	pthread_mutex_unlock(&g_nkvx.mutex);
	return NULL;
}

int
kvdev_rados_nkvx_start(void)
{
	int rc;

	pthread_mutex_lock(&g_nkvx.mutex);
	if (g_nkvx.running) {
		pthread_mutex_unlock(&g_nkvx.mutex);
		return 0;
	}
	STAILQ_INIT(&g_nkvx.queue);
	g_nkvx.stop = false;

	rc = pthread_create(&g_nkvx.tid, NULL, kvdev_rados_nkvx_worker_main, NULL);
	if (rc != 0) {
		pthread_mutex_unlock(&g_nkvx.mutex);
		SPDK_ERRLOG("nkvx: failed to start executor worker: %s\n", spdk_strerror(rc));
		return -rc;
	}
	g_nkvx.running = true;
	pthread_mutex_unlock(&g_nkvx.mutex);
	SPDK_NOTICELOG("nkvx: executor worker started (TB1: 1 thread, built-in modules)\n");
	return 0;
}

void
kvdev_rados_nkvx_stop(void)
{
	pthread_mutex_lock(&g_nkvx.mutex);
	if (!g_nkvx.running) {
		pthread_mutex_unlock(&g_nkvx.mutex);
		return;
	}
	g_nkvx.stop = true;
	pthread_cond_broadcast(&g_nkvx.cond);
	pthread_mutex_unlock(&g_nkvx.mutex);

	pthread_join(g_nkvx.tid, NULL);

	/* Tear down process-wide wasm runtime resources owned by the executor
	 * (spdk-0k1): stop+join the epoch ticker so it does not outlive the executor
	 * (no 1-thread leak on a stop/restart). Done after the worker join so no
	 * in-flight run can still be arming the ticker. A later restart re-creates
	 * the ticker lazily on the first capped run. */
	kvdev_rados_nkvx_wasm_runtime_teardown();

	pthread_mutex_lock(&g_nkvx.mutex);
	g_nkvx.running = false;
	pthread_mutex_unlock(&g_nkvx.mutex);
	SPDK_NOTICELOG("nkvx: executor worker stopped\n");
}

int
kvdev_rados_nkvx_dispatch(const char *module, const struct kvdev_rados_nkvx_module *mod,
			  const char *obj_key, void *obj_pin,
			  const void *object, size_t object_len,
			  void *out, uint32_t out_len,
			  kvdev_rados_nkvx_done_fn done_fn, void *done_arg)
{
	struct kvdev_rados_nkvx_job *job;
	struct spdk_thread *origin = spdk_get_thread();

	if (origin == NULL) {
		/* Must be dispatched from an SPDK thread so we can hand the
		 * completion back to it. */
		return -EINVAL;
	}

	if (module == NULL) {
		return -EINVAL;
	}

	job = calloc(1, sizeof(*job));
	if (job == NULL) {
		return -ENOMEM;
	}
	snprintf(job->module, sizeof(job->module), "%s", module);
	if (obj_key != NULL) {
		snprintf(job->obj_key, sizeof(job->obj_key), "%s", obj_key);
	}
	if (mod != NULL) {
		job->has_mod = true;
		job->mod = *mod;	/* sha256/caps by value; bytes pointer carried (caller-owned) */
	}
	job->obj_pin = obj_pin;
	job->object = object;
	job->object_len = object_len;
	job->out = out;
	job->out_len = out_len;
	job->done_fn = done_fn;
	job->done_arg = done_arg;
	job->origin = origin;
	job->submit_tid = pthread_self();

	pthread_mutex_lock(&g_nkvx.mutex);
	if (!g_nkvx.running) {
		pthread_mutex_unlock(&g_nkvx.mutex);
		free(job);
		return -ENODEV;
	}
	STAILQ_INSERT_TAIL(&g_nkvx.queue, job, link);
	pthread_cond_signal(&g_nkvx.cond);
	pthread_mutex_unlock(&g_nkvx.mutex);
	return 0;
}

int
kvdev_rados_nkvx_dispatch_compile(const uint8_t sha256[SPDK_KV_EXEC_SHA256_LEN],
				  const void *bytes, size_t bytes_len,
				  kvdev_rados_nkvx_compiled_fn done_fn, void *done_arg)
{
	struct kvdev_rados_nkvx_job *job;
	struct spdk_thread *origin = spdk_get_thread();

	if (origin == NULL) {
		/* Must be dispatched from an SPDK thread so we can hand the completion
		 * back to it (same contract as kvdev_rados_nkvx_dispatch). */
		return -EINVAL;
	}
	if (sha256 == NULL || bytes == NULL || bytes_len == 0 || done_fn == NULL) {
		return -EINVAL;
	}

	job = calloc(1, sizeof(*job));
	if (job == NULL) {
		return -ENOMEM;
	}
	job->kind = NKVX_JOB_COMPILE;
	memcpy(job->compile_sha256, sha256, SPDK_KV_EXEC_SHA256_LEN);
	job->compile_bytes = bytes;		/* caller-owned; must outlive the job */
	job->compile_bytes_len = bytes_len;
	job->compiled_fn = done_fn;
	job->done_arg = done_arg;
	job->origin = origin;
	job->submit_tid = pthread_self();

	pthread_mutex_lock(&g_nkvx.mutex);
	if (!g_nkvx.running) {
		pthread_mutex_unlock(&g_nkvx.mutex);
		free(job);
		return -ENODEV;
	}
	STAILQ_INSERT_TAIL(&g_nkvx.queue, job, link);
	pthread_cond_signal(&g_nkvx.cond);
	pthread_mutex_unlock(&g_nkvx.mutex);
	return 0;
}
