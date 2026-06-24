/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/config.h"	/* SPDK_CONFIG_WASM (must precede the #if below) */
#include "spdk/log.h"
#include "spdk/kvdev.h"
#include "spdk/util.h"

#include "kvdev_rados_nkvx.h"		/* KVDEV_RADOS_NKVX_WASM_DIR_ENV */
#include "kvdev_rados_nkvx_wasm.h"

/*
 * TB4 acceptance counters (spdk-ii0). Defined UNCONDITIONALLY (outside the
 * SPDK_CONFIG_WASM guard) so the unit test can read them in both --with-wasm and
 * --without-wasm builds. The cache code (under the guard) bumps them. Guarded by
 * the cache mutex when wasm is built; harmless zero in the stub build.
 */
static struct kvdev_rados_nkvx_wasm_stats g_nkvx_wasm_stats;

void
kvdev_rados_nkvx_wasm_get_stats(struct kvdev_rados_nkvx_wasm_stats *out)
{
	if (out != NULL) {
		*out = g_nkvx_wasm_stats;
	}
}

/*
 * dlopen-backed wasmtime runtime for the rados-nkvx Exec path. See the header
 * for the design contract (ADR-0013). This file is only meaningfully compiled
 * when SPDK_CONFIG_WASM is set; with --without-wasm it collapses to a stub that
 * always reports the wasm runtime unavailable (NOT_SUPPORTED), so a build never
 * requires anything wasm.
 */

#if defined(SPDK_CONFIG_WASM)

#include <dlfcn.h>

/* OpenSSL for the SHA-256 module-integrity gate (ADR-0010). -lcrypto is linked
 * unconditionally into every SPDK app, so this adds no new link dependency. */
#include <openssl/evp.h>
#include <openssl/crypto.h>

/* Vendored wasmtime C-API headers (signatures only; no link dependency). */
#include "wasmtime.h"

/*
 * Module ABI offsets — must match test/nvmf/kv/wasm/bytecount.c:
 *   - object bytes are copied into linear memory at WASM_OBJ_OFF,
 *   - the module writes its result at WASM_RES_OFF and returns the result length.
 * A single 64KiB page of linear memory is plenty for the tracer-bullet object.
 */
#define WASM_OBJ_OFF 256u
#define WASM_RES_OFF 8u

/*
 * Per-invocation resource caps (TB2: the Phase-1 safety exit). A runaway or
 * over-allocating module must be CONTAINED, never crash/hang the target.
 *
 *   - fuel_ceiling: units of wasmtime "fuel" the module may consume before it
 *     traps (compute-runaway kill). 0 => fuel disabled for this run.
 *   - epoch_deadline_ticks: how many epoch ticks beyond the current epoch the
 *     module may run before it is interrupted (wall-clock-runaway kill — catches
 *     what fuel under-counts). 0 => epoch deadline disabled for this run.
 *   - max_memory_bytes: linear-memory ceiling enforced by the store limiter, so
 *     a memory.grow past the cap fails (contained, not an OOM of the target).
 *     0 => memory unlimited for this run.
 *
 * SOURCE OF CAPS (this slice): provisional defaults below, each overridable per
 * invocation by an environment variable (read once on every run, so they ARE
 * configurable per invocation). Wiring caps to the per-op allowlist binding is
 * TB3 (the binding-encoding freeze is in progress) — NOT this slice. The plumbing
 * in nkvx_wasm_caps_load() is exactly where the TB3 allowlist source will replace
 * the env/default source.
 */
struct nkvx_wasm_caps {
	uint64_t	fuel_ceiling;
	uint64_t	epoch_deadline_ticks;
	uint64_t	max_memory_bytes;
};

/* Provisional defaults (TB2). Tuned so the checked-in test modules behave as the
 * acceptance matrix requires: bytecount completes well under these; the runaway
 * modules blow past them. */
#define NKVX_WASM_DEFAULT_FUEL		(100ull * 1000ull * 1000ull)	/* 100M fuel units */
#define NKVX_WASM_DEFAULT_EPOCH_TICKS	(10ull)				/* 10 ticks @ tick interval */
#define NKVX_WASM_DEFAULT_MAX_MEMORY	(8ull * 1024ull * 1024ull)	/* 8 MiB linear memory */

/* Env overrides (per-invocation; TB3 replaces these with the allowlist source). */
#define NKVX_WASM_ENV_FUEL		"SPDK_NKVX_WASM_FUEL"
#define NKVX_WASM_ENV_EPOCH_TICKS	"SPDK_NKVX_WASM_EPOCH_TICKS"
#define NKVX_WASM_ENV_MAX_MEMORY	"SPDK_NKVX_WASM_MAX_MEMORY"

/* Epoch ticker interval. The background timer thread bumps the engine epoch at
 * this cadence; epoch_deadline_ticks * this interval is the effective wall-clock
 * budget. 10ms * 10 ticks => ~100ms wall-clock ceiling by default. */
#define NKVX_WASM_EPOCH_TICK_NS		(10ull * 1000ull * 1000ull)	/* 10 ms */

/*
 * dlsym table — the symbols a capped off-reactor run needs (ADR-0013: keep the
 * set minimal, but TB2 adds the config/fuel/epoch/limiter symbols). Version-gated
 * by symbol-presence probing (no wasmtime version function exists); a missing
 * symbol => the table fails to load and we fall back to "runtime unavailable".
 */
struct nkvx_wasm_api {
	/* Config + engine: a config carries the fuel/epoch feature toggles, so the
	 * engine is created via wasm_engine_new_with_config (NOT wasm_engine_new). */
	wasm_config_t *(*config_new)(void);
	void (*config_consume_fuel_set)(wasm_config_t *, bool);
	void (*config_epoch_interruption_set)(wasm_config_t *, bool);
	wasm_engine_t *(*engine_new_with_config)(wasm_config_t *);
	void (*engine_delete)(wasm_engine_t *);
	/* Wall-clock interrupt source: the timer thread calls this. */
	void (*engine_increment_epoch)(wasm_engine_t *);

	/*
	 * TB4 zero-copy custom MemoryCreator (ADR-0013): when set on the config,
	 * wasmtime calls our new_memory callback to obtain the linear-memory
	 * backing for on-demand instances, letting us alias the cached object
	 * buffer (no copy-in). Works ONLY with the on-demand strategy.
	 */
	void (*config_host_memory_creator_set)(wasm_config_t *,
					       wasmtime_memory_creator_t *);

	/*
	 * Bounds-check codegen control (ADR-0013 sandbox escape fix, spdk-ii0 B1).
	 * A custom (host) linear memory backed by our exact-sized zero-copy buffer
	 * has NO guard region, so wasmtime's default STATIC bounds-check elision
	 * (which relies on a large guarded reservation) would leave out-of-bounds
	 * guest accesses uncaught. Setting reservation=0 and guard=0 forces DYNAMIC
	 * (explicit) bounds checks, so every access is checked against the memory's
	 * current REPORTED length and OOB always traps. may_move=false keeps the
	 * backing fixed (we refuse grow on an immutable zero-copy object anyway).
	 * Note the reported length is the module's declared minimum, not the object
	 * backing (spdk-ii0 D1, see nkvx_zc_new_memory), so the slack between a
	 * small module and a larger backing is unreachable too.
	 */
	void (*config_memory_reservation_set)(wasm_config_t *, uint64_t);
	void (*config_memory_guard_size_set)(wasm_config_t *, uint64_t);
	void (*config_memory_may_move_set)(wasm_config_t *, bool);

	/* Construct a real wasmtime_error_t for host-callback failures (e.g. a
	 * refused memory.grow) instead of a fabricated sentinel pointer. */
	wasmtime_error_t *(*error_new)(const char *);

	wasmtime_store_t *(*store_new)(wasm_engine_t *, void *, void *);
	wasmtime_context_t *(*store_context)(wasmtime_store_t *);
	void (*store_delete)(wasmtime_store_t *);
	/* Linear-memory (and friends) ceiling. */
	void (*store_limiter)(wasmtime_store_t *, int64_t, int64_t, int64_t,
			      int64_t, int64_t);

	/* Per-invocation caps applied to the store's context. */
	wasmtime_error_t *(*context_set_fuel)(wasmtime_context_t *, uint64_t);
	void (*context_set_epoch_deadline)(wasmtime_context_t *, uint64_t);

	wasmtime_error_t *(*module_new)(wasm_engine_t *, const uint8_t *, size_t,
					wasmtime_module_t **);
	void (*module_delete)(wasmtime_module_t *);
	/*
	 * TB3 content-addressed module cache (spdk-fbm): serialize a compiled module
	 * to a portable blob, deserialize it into another engine. Lets the sha256
	 * cache hold the COMPILED artifact and skip recompilation on a hash hit,
	 * across distinct data objects/engines.
	 */
	wasmtime_error_t *(*module_serialize)(wasmtime_module_t *, wasm_byte_vec_t *);
	wasmtime_error_t *(*module_deserialize)(wasm_engine_t *, const uint8_t *, size_t,
						wasmtime_module_t **);

	wasmtime_error_t *(*instance_new)(wasmtime_context_t *, const wasmtime_module_t *,
					  const wasmtime_extern_t *, size_t,
					  wasmtime_instance_t *, wasm_trap_t **);
	bool (*instance_export_get)(wasmtime_context_t *, const wasmtime_instance_t *,
				    const char *, size_t, wasmtime_extern_t *);

	uint8_t *(*memory_data)(const wasmtime_context_t *, const wasmtime_memory_t *);
	size_t (*memory_data_size)(const wasmtime_context_t *, const wasmtime_memory_t *);

	wasmtime_error_t *(*func_call)(wasmtime_context_t *, const wasmtime_func_t *,
				       const wasmtime_val_t *, size_t,
				       wasmtime_val_t *, size_t, wasm_trap_t **);

	void (*error_message)(const wasmtime_error_t *, wasm_byte_vec_t *);
	void (*error_delete)(wasmtime_error_t *);
	void (*trap_message)(const wasm_trap_t *, wasm_byte_vec_t *);
	void (*trap_delete)(wasm_trap_t *);
	void (*byte_vec_delete)(wasm_byte_vec_t *);
};

/*
 * Lazy, cached load of libwasmtime.so + the symbol table. Loaded once on first
 * use; protected by a once-init. g_api stays NULL if the library or any required
 * symbol is missing — that is the fail-soft signal.
 */
static struct nkvx_wasm_api g_api;
static const struct nkvx_wasm_api *g_api_ready;
static pthread_once_t g_api_once = PTHREAD_ONCE_INIT;
static void *g_lib_handle;

/* Candidate paths for libwasmtime.so, tried in order. The bare soname covers the
 * ldconfig/LD_LIBRARY_PATH case; the explicit paths cover the documented
 * install location and the in-tree vendored copy. */
static const char *g_lib_candidates[] = {
	"libwasmtime.so",
	"/usr/local/lib/libwasmtime.so",
	"/usr/lib/libwasmtime.so",
	"/usr/lib64/libwasmtime.so",
};

static bool
nkvx_wasm_resolve(void *h)
{
	bool ok = true;

#define SYM(field, name) do { \
		*(void **)(&g_api.field) = dlsym(h, name); \
		if (g_api.field == NULL) { \
			SPDK_ERRLOG("nkvx/wasm: missing symbol %s in libwasmtime.so\n", name); \
			ok = false; \
		} \
	} while (0)

	SYM(config_new, "wasm_config_new");
	SYM(config_consume_fuel_set, "wasmtime_config_consume_fuel_set");
	SYM(config_epoch_interruption_set, "wasmtime_config_epoch_interruption_set");
	SYM(engine_new_with_config, "wasm_engine_new_with_config");
	SYM(engine_delete, "wasm_engine_delete");
	SYM(engine_increment_epoch, "wasmtime_engine_increment_epoch");
	SYM(config_host_memory_creator_set, "wasmtime_config_host_memory_creator_set");
	SYM(config_memory_reservation_set, "wasmtime_config_memory_reservation_set");
	SYM(config_memory_guard_size_set, "wasmtime_config_memory_guard_size_set");
	SYM(config_memory_may_move_set, "wasmtime_config_memory_may_move_set");
	SYM(error_new, "wasmtime_error_new");
	SYM(store_new, "wasmtime_store_new");
	SYM(store_context, "wasmtime_store_context");
	SYM(store_delete, "wasmtime_store_delete");
	SYM(store_limiter, "wasmtime_store_limiter");
	SYM(context_set_fuel, "wasmtime_context_set_fuel");
	SYM(context_set_epoch_deadline, "wasmtime_context_set_epoch_deadline");
	SYM(module_new, "wasmtime_module_new");
	SYM(module_delete, "wasmtime_module_delete");
	SYM(module_serialize, "wasmtime_module_serialize");
	SYM(module_deserialize, "wasmtime_module_deserialize");
	SYM(instance_new, "wasmtime_instance_new");
	SYM(instance_export_get, "wasmtime_instance_export_get");
	SYM(memory_data, "wasmtime_memory_data");
	SYM(memory_data_size, "wasmtime_memory_data_size");
	SYM(func_call, "wasmtime_func_call");
	SYM(error_message, "wasmtime_error_message");
	SYM(error_delete, "wasmtime_error_delete");
	SYM(trap_message, "wasm_trap_message");
	SYM(trap_delete, "wasm_trap_delete");
	SYM(byte_vec_delete, "wasm_byte_vec_delete");
#undef SYM

	return ok;
}

static void
nkvx_wasm_load_once(void)
{
	void *h = NULL;
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(g_lib_candidates); i++) {
		h = dlopen(g_lib_candidates[i], RTLD_NOW | RTLD_LOCAL);
		if (h != NULL) {
			SPDK_NOTICELOG("nkvx/wasm: loaded %s\n", g_lib_candidates[i]);
			break;
		}
	}
	if (h == NULL) {
		SPDK_WARNLOG("nkvx/wasm: libwasmtime.so not found (wasm Exec path "
			     "unavailable; built-ins still work): %s\n", dlerror());
		return;
	}

	if (!nkvx_wasm_resolve(h)) {
		SPDK_ERRLOG("nkvx/wasm: incompatible libwasmtime.so (missing symbols); "
			    "wasm Exec path unavailable\n");
		dlclose(h);
		return;
	}

	g_lib_handle = h;
	g_api_ready = &g_api;
}

static const struct nkvx_wasm_api *
nkvx_wasm_api(void)
{
	pthread_once(&g_api_once, nkvx_wasm_load_once);
	return g_api_ready;
}

/*
 * EPOCH TICKER THREAD (the wall-clock interrupt source).
 *
 * Fuel under-counts wall-clock (a tight loop of cheap ops, or a host trap loop,
 * can burn real time without burning much fuel). Epoch interruption catches that:
 * a background thread bumps the engine-local epoch at a fixed cadence, and any
 * run whose store has set an epoch deadline traps once enough ticks elapse.
 *
 * On-demand (ADR-0013) means at most one engine is live at a time on the single
 * executor worker, so the ticker tracks a single "current engine" pointer under a
 * mutex. nkvx_wasm_epoch_register()/_unregister() bracket each run; the thread is
 * started once (lazily) on the first armed run. If a second engine ever overlapped,
 * the register call would simply replace the pointer — acceptable because each
 * run also has its own fuel ceiling as a second, independent guard.
 *
 * TEARDOWN (spdk-0k1): the ticker is NOT a process-lifetime leak. Executor stop
 * (kvdev_rados_nkvx_stop -> kvdev_rados_nkvx_wasm_runtime_teardown) calls
 * nkvx_wasm_epoch_stop(), which signals the thread to exit and JOINS it, clearing
 * \c started so a later executor (re)start lazily re-creates a fresh ticker — no
 * leaked thread on restart, no double-start. The loop waits on the condvar with a
 * tick-interval timeout (instead of a bare nanosleep) so stop is observed promptly
 * and the thread never touches an engine after stop (the join completes before the
 * caller tears any engine down). The warm-path epoch arming (spdk-ii0 D3) is
 * unaffected: register lazily restarts the ticker after a stop/restart.
 */
static struct {
	pthread_mutex_t		mutex;
	pthread_cond_t		cond;
	pthread_t		tid;
	bool			started;
	bool			stop;		/* set by nkvx_wasm_epoch_stop to exit */
	wasm_engine_t		*engine;	/* engine to tick, or NULL when idle */
	const struct nkvx_wasm_api *api;
} g_epoch = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,
};

static void *
nkvx_wasm_epoch_thread(void *arg)
{
	(void)arg;

	pthread_mutex_lock(&g_epoch.mutex);
	while (!g_epoch.stop) {
		struct timespec ts;

		/* Wait up to one tick interval, but wake immediately on stop. Using
		 * the condvar (vs a bare nanosleep) makes teardown prompt and bounds
		 * the join latency to at most one tick. clock_gettime + timedwait is
		 * the portable cancellable sleep. */
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += (long)(NKVX_WASM_EPOCH_TICK_NS % 1000000000ull);
		ts.tv_sec += (time_t)(NKVX_WASM_EPOCH_TICK_NS / 1000000000ull);
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_nsec -= 1000000000L;
			ts.tv_sec += 1;
		}
		pthread_cond_timedwait(&g_epoch.cond, &g_epoch.mutex, &ts);

		if (g_epoch.stop) {
			break;
		}
		if (g_epoch.engine != NULL && g_epoch.api != NULL) {
			/* Bump the epoch of the in-flight run's engine. A run that has
			 * set an epoch deadline traps once its tick budget elapses.
			 * Held under the mutex, so a concurrent _unregister (which also
			 * takes the mutex) can never let us touch a torn-down engine. */
			g_epoch.api->engine_increment_epoch(g_epoch.engine);
		}
	}
	pthread_mutex_unlock(&g_epoch.mutex);
	return NULL;
}

/* Start the ticker thread once; arm it for this run's engine. */
static void
nkvx_wasm_epoch_register(const struct nkvx_wasm_api *api, wasm_engine_t *engine)
{
	pthread_mutex_lock(&g_epoch.mutex);
	if (!g_epoch.started) {
		g_epoch.stop = false;
		if (pthread_create(&g_epoch.tid, NULL, nkvx_wasm_epoch_thread, NULL) == 0) {
			g_epoch.started = true;
		} else {
			SPDK_ERRLOG("nkvx/wasm: epoch ticker thread create failed; "
				    "wall-clock cap inactive (fuel cap still applies)\n");
		}
	}
	g_epoch.api = api;
	g_epoch.engine = engine;
	pthread_mutex_unlock(&g_epoch.mutex);
}

/* Disarm the ticker (the engine is about to be torn down). */
static void
nkvx_wasm_epoch_unregister(wasm_engine_t *engine)
{
	pthread_mutex_lock(&g_epoch.mutex);
	if (g_epoch.engine == engine) {
		g_epoch.engine = NULL;
	}
	pthread_mutex_unlock(&g_epoch.mutex);
}

/*
 * Stop and JOIN the epoch ticker thread (spdk-0k1). Called from the executor
 * teardown path. Idempotent: a no-op when the ticker was never started. Clears
 * \c started so a later executor restart re-creates a fresh ticker (no leak, no
 * double-start). The join guarantees the thread is gone before the caller deletes
 * any engine, so the ticker never advances a stale/freed engine after stop. The
 * engine pointer is cleared first so a wakeup-before-stop tick is a safe no-op.
 */
static void
nkvx_wasm_epoch_stop(void)
{
	pthread_t tid;
	bool joinable;

	pthread_mutex_lock(&g_epoch.mutex);
	joinable = g_epoch.started;
	if (joinable) {
		g_epoch.engine = NULL;
		g_epoch.api = NULL;
		g_epoch.stop = true;
		tid = g_epoch.tid;
		pthread_cond_broadcast(&g_epoch.cond);
	}
	pthread_mutex_unlock(&g_epoch.mutex);

	if (joinable) {
		pthread_join(tid, NULL);
		pthread_mutex_lock(&g_epoch.mutex);
		g_epoch.started = false;
		g_epoch.stop = false;
		pthread_mutex_unlock(&g_epoch.mutex);
	}
}

/*
 * Tear down process-wide wasm runtime resources owned by the executor (spdk-0k1).
 * Currently: stop+join the epoch ticker thread. Called from kvdev_rados_nkvx_stop
 * so the ticker does not outlive the executor (no 1-thread leak on a restart). The
 * dlopen'd libwasmtime handle and the compiled-module/object caches are
 * deliberately NOT dropped here (the caches have their own reset entry points and
 * may legitimately survive an executor cycle); this hook is specifically the
 * thread-lifetime teardown the ticker needs.
 */
void
kvdev_rados_nkvx_wasm_runtime_teardown(void)
{
	nkvx_wasm_epoch_stop();
}

/* Parse a uint64 env override; returns def when unset/empty/unparseable. */
static uint64_t
nkvx_wasm_env_u64(const char *name, uint64_t def)
{
	const char *v = getenv(name);
	char *end = NULL;
	unsigned long long parsed;

	if (v == NULL || v[0] == '\0') {
		return def;
	}
	errno = 0;
	parsed = strtoull(v, &end, 0);
	if (errno != 0 || end == v || (end != NULL && *end != '\0')) {
		SPDK_WARNLOG("nkvx/wasm: bad value '%s' for %s; using default %" PRIu64 "\n",
			     v, name, def);
		return def;
	}
	return (uint64_t)parsed;
}

/*
 * Per-invocation caps TIER encoding (TB3 / spdk-fbm). ADR-0014 froze the binding's
 * \c caps as a single uint64 word, so we cannot carry three independent 64-bit
 * fuel/epoch/memory values; instead the low bits select a named TIER whose
 * fuel/epoch/memory triple is fixed here. \c caps == 0 means "executor defaults"
 * (env-overridable), preserving the TB2/TB4 behaviour. Higher tiers are
 * progressively more generous (the control plane blesses a module with a tier per
 * op-ID). Unknown tiers fall back to the default tier.
 */
#define NKVX_WASM_CAPS_TIER_MASK	0xFFull
enum nkvx_wasm_caps_tier {
	NKVX_WASM_CAPS_TIER_DEFAULT	= 0,	/* defaults (env-overridable) */
	NKVX_WASM_CAPS_TIER_SMALL	= 1,	/* tight: short compute, small mem */
	NKVX_WASM_CAPS_TIER_MEDIUM	= 2,	/* default-equivalent fixed triple */
	NKVX_WASM_CAPS_TIER_LARGE	= 3,	/* generous: long compute, large mem */
};
/* The public caps contract (include/spdk/kvdev.h) is what the control plane
 * validates against; keep its tier ceiling in lockstep with the enum above so a
 * new tier cannot be admitted by the RPC layer without a matching case here. */
SPDK_STATIC_ASSERT(NKVX_WASM_CAPS_TIER_LARGE == SPDK_KV_EXEC_CAPS_TIER_MAX,
		   "caps tier ceiling out of sync with the public contract");

/*
 * Load the per-invocation caps for THIS run from the allowlist binding's \c caps
 * word (TB3 source, ADR-0010/0014). When \c caps_word == 0 the executor defaults
 * apply, each still env-overridable (so existing tests and ad-hoc tuning keep
 * working). A non-zero word selects a fixed capability tier — the env overrides do
 * NOT apply to an explicit tier, so the control plane's choice is authoritative.
 */
static void
nkvx_wasm_caps_load(struct nkvx_wasm_caps *caps, uint64_t caps_word)
{
	switch (caps_word & NKVX_WASM_CAPS_TIER_MASK) {
	case NKVX_WASM_CAPS_TIER_DEFAULT:
		/* Defaults, env-overridable (TB2/TB4 behaviour preserved). */
		caps->fuel_ceiling = nkvx_wasm_env_u64(NKVX_WASM_ENV_FUEL,
						       NKVX_WASM_DEFAULT_FUEL);
		caps->epoch_deadline_ticks = nkvx_wasm_env_u64(NKVX_WASM_ENV_EPOCH_TICKS,
					     NKVX_WASM_DEFAULT_EPOCH_TICKS);
		caps->max_memory_bytes = nkvx_wasm_env_u64(NKVX_WASM_ENV_MAX_MEMORY,
					 NKVX_WASM_DEFAULT_MAX_MEMORY);
		return;
	case NKVX_WASM_CAPS_TIER_SMALL:
		caps->fuel_ceiling = 10ull * 1000ull * 1000ull;		/* 10M */
		caps->epoch_deadline_ticks = 5ull;			/* ~50ms */
		caps->max_memory_bytes = 1ull * 1024ull * 1024ull;	/* 1 MiB */
		return;
	case NKVX_WASM_CAPS_TIER_LARGE:
		caps->fuel_ceiling = 1000ull * 1000ull * 1000ull;	/* 1B */
		caps->epoch_deadline_ticks = 100ull;			/* ~1s */
		caps->max_memory_bytes = 64ull * 1024ull * 1024ull;	/* 64 MiB */
		return;
	case NKVX_WASM_CAPS_TIER_MEDIUM:
	default:
		caps->fuel_ceiling = NKVX_WASM_DEFAULT_FUEL;
		caps->epoch_deadline_ticks = NKVX_WASM_DEFAULT_EPOCH_TICKS;
		caps->max_memory_bytes = NKVX_WASM_DEFAULT_MAX_MEMORY;
		return;
	}
}

/*
 * spdk-5pq: the wasmtime JIT runs <name> behind a setjmp/longjmp trampoline; when
 * it longjmps back, the Memcheck SHADOW of the (correctly restored, callee-saved)
 * frame pointer -- and of func_call's return value / *trap / *results outputs -- is
 * left "uninitialised". Reading any of them then trips a BENIGN uninit-value report
 * whose stack is byte-identical to a real bug, so no .supp rule can silence it
 * without also masking a genuine bug. We clear those shadows AT THE SOURCE.
 *
 * The hard part is the frame pointer: in the direct-call case the very first
 * %rbp-relative access after the call (the spill of func_call's return value) is
 * flagged, and a clear placed AFTER it is too late. nkvx_vg_after_func_call solves
 * this NARROWLY -- it marks only specific objects, never a frame-window heuristic
 * (which would also hide real uninitialised reads). It receives func_call's return
 * value BY REGISTER (so capturing it needs no frame-pointer-relative store), and:
 *   1. marks just that 8-byte value's shadow defined (it is whatever func_call
 *      returned -- we drop only the shadow, never a byte), and
 *   2. marks just the 8-byte slot at THIS helper's frame base, where its prologue
 *      pushed the caller's %rbp; so on return (pop %rbp) the caller's frame pointer
 *      shadow is defined and the caller's return-value store is not falsely flagged.
 * Nothing else is touched, so a genuine uninitialised read of any other local is
 * still reported. No-op outside the unit-test build (see header).
 */
#if defined(NKVX_VG_HAVE)
static __attribute__((noinline)) void *
nkvx_vg_after_func_call(void *ret)
{
	NKVX_VALGRIND_MAKE_DEFINED(&ret, sizeof(ret));
	/* The slot at this frame's base holds the caller's pushed %rbp; defining just
	 * those 8 bytes launders the caller's frame pointer shadow on our return. */
	NKVX_VALGRIND_MAKE_DEFINED(__builtin_frame_address(0), sizeof(void *));
	return ret;
}

static __attribute__((noinline)) wasmtime_error_t *
nkvx_wasm_func_call(const struct nkvx_wasm_api *api, wasmtime_context_t *ctx,
		    const wasmtime_func_t *fn, const wasmtime_val_t *args, size_t nargs,
		    wasmtime_val_t *results, size_t nresults, wasm_trap_t **trap)
{
	/*
	 * nkvx_vg_after_func_call launders THIS function's frame pointer (so the return
	 * store and our epilogue are clean) and defines the return value's shadow.
	 */
	wasmtime_error_t *err = nkvx_vg_after_func_call(
		api->func_call(ctx, fn, args, nargs, results, nresults, trap));

	/*
	 * The longjmp also poisoned the compiler-owned slots of THIS frame: our own
	 * stack-protector canary (read by our epilogue) and -- at our frame base -- the
	 * saved CALLER %rbp (restored by `pop %rbp` on return). Now that our own %rbp is
	 * laundered (above), the narrow top-of-frame scrub re-defines that band, so both
	 * our epilogue and the caller's frame pointer are clean. The caller clears its
	 * own named locals (and func_call's *trap/results outputs) itself.
	 */
	NKVX_VALGRIND_SCRUB_FRAME_TOP();
	return err;
}
#else  /* production: a plain, inlinable forward -- byte-for-byte a direct call */
static inline wasmtime_error_t *
nkvx_wasm_func_call(const struct nkvx_wasm_api *api, wasmtime_context_t *ctx,
		    const wasmtime_func_t *fn, const wasmtime_val_t *args, size_t nargs,
		    wasmtime_val_t *results, size_t nresults, wasm_trap_t **trap)
{
	return api->func_call(ctx, fn, args, nargs, results, nresults, trap);
}
#endif

/*
 * Classify a wasmtime trap as a resource-cap kill (fuel/epoch) versus an ordinary
 * module trap. The wasmtime C API exposes the trap message; cap-induced traps
 * carry a recognisable message ("all fuel consumed", "epoch deadline" / "interrupt").
 * On match we surface ABORTED; otherwise FAILED. Either way the worker returns
 * cleanly — the target never crashes.
 */
static bool
nkvx_wasm_trap_is_cap(const struct nkvx_wasm_api *api, wasm_trap_t *trap)
{
	wasm_byte_vec_t msg;
	bool is_cap = false;

	api->trap_message(trap, &msg);
	/*
	 * spdk-5pq: on the wasmtime setjmp/longjmp path the trap and its message
	 * carry the BENIGN longjmp taint; clear it over the returned message vector
	 * and its bytes so the classification below (and is_cap) is not falsely
	 * flagged. No-op outside the valgrind UT (see header). The message content is
	 * whatever wasmtime wrote -- we only drop the shadow, never change a byte.
	 */
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(msg);
	if (msg.data != NULL && msg.size > 0) {
		NKVX_VALGRIND_MAKE_DEFINED(msg.data, msg.size);
	}
	if (msg.data != NULL && msg.size > 0) {
		/* Case-insensitive substring search for the known cap messages. */
		static const char *needles[] = {
			"fuel", "epoch", "interrupt",
		};
		size_t i;

		for (i = 0; i < SPDK_COUNTOF(needles); i++) {
			size_t nlen = strlen(needles[i]);

			if (msg.size >= nlen) {
				size_t j;

				for (j = 0; j + nlen <= msg.size; j++) {
					if (strncasecmp(msg.data + j, needles[i], nlen) == 0) {
						is_cap = true;
						break;
					}
				}
			}
			if (is_cap) {
				break;
			}
		}
		SPDK_NOTICELOG("nkvx/wasm: trap message: %.*s (cap=%s)\n",
			       (int)msg.size, msg.data, is_cap ? "YES" : "no");
	}
	api->byte_vec_delete(&msg);
	return is_cap;
}

/* Log and free a wasmtime error. */
static void
nkvx_wasm_log_error(const struct nkvx_wasm_api *api, const char *what, wasmtime_error_t *err)
{
	wasm_byte_vec_t msg;

	api->error_message(err, &msg);
	/* spdk-5pq: clear the benign wasmtime longjmp taint over the message vector
	 * and its bytes so logging it is not falsely flagged. No-op outside the UT. */
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(msg);
	if (msg.data != NULL && msg.size > 0) {
		NKVX_VALGRIND_MAKE_DEFINED(msg.data, msg.size);
	}
	SPDK_ERRLOG("nkvx/wasm: %s: %.*s\n", what, (int)msg.size, msg.data);
	api->byte_vec_delete(&msg);
	api->error_delete(err);
}

/* Resolve <name>.wasm under the nkvx wasm module directory into pathbuf. */
static int
nkvx_wasm_module_path(const char *name, char *pathbuf, size_t pathbuf_len)
{
	const char *dir = getenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV);

	if (dir == NULL || dir[0] == '\0') {
		/* No directory configured: cannot locate the module. The e2e/unit
		 * tests set SPDK_NKVX_WASM_DIR explicitly. */
		SPDK_ERRLOG("nkvx/wasm: %s unset; cannot locate '%s.wasm'\n",
			    KVDEV_RADOS_NKVX_WASM_DIR_ENV, name);
		return -1;
	}
	if ((size_t)snprintf(pathbuf, pathbuf_len, "%s/%s.wasm", dir, name) >= pathbuf_len) {
		SPDK_ERRLOG("nkvx/wasm: module path too long for '%s'\n", name);
		return -1;
	}
	return 0;
}

static uint8_t *
nkvx_wasm_read_file(const char *path, size_t *out_len)
{
	FILE *f;
	long sz;
	uint8_t *buf;

	f = fopen(path, "rb");
	if (f == NULL) {
		SPDK_ERRLOG("nkvx/wasm: cannot open module '%s': %s\n", path, strerror(errno));
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		SPDK_ERRLOG("nkvx/wasm: cannot size module '%s'\n", path);
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)sz);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		SPDK_ERRLOG("nkvx/wasm: short read of module '%s'\n", path);
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*out_len = (size_t)sz;
	return buf;
}

/* ==========================================================================
 * TB3: content-addressed (sha256) COMPILED-module cache + the integrity gate
 * (spdk-fbm / ADR-0010). The sha256 is the SOLE authorization + integrity anchor:
 * fetched .wasm bytes are run ONLY if they hash to the control-plane's bound
 * sha256, and the compiled artifact is cached by that hash so a repeat Exec of the
 * same module skips refetch+recompile. This cache is DISTINCT from the oid object
 * cache and the (module,oid) warm-instance cache below.
 * ========================================================================== */

/*
 * Compute SHA-256 of (buf,len) into out[32] using OpenSSL EVP. Returns true on
 * success. A digest failure (should not happen for sha256) is treated as a verify
 * failure by the caller, so unverified bytes are never run.
 */
static bool
nkvx_sha256(const void *buf, size_t len, uint8_t out[SPDK_KV_EXEC_SHA256_LEN])
{
	unsigned int mdlen = 0;

	if (EVP_Digest(buf, len, out, &mdlen, EVP_sha256(), NULL) != 1) {
		SPDK_ERRLOG("nkvx/wasm: SHA-256 digest failed\n");
		return false;
	}
	return mdlen == SPDK_KV_EXEC_SHA256_LEN;
}

/*
 * The integrity gate (ADR-0010). Hash the fetched bytes and constant-time-compare
 * the full 32-byte digest against the bound sha256. Returns true ONLY on an exact
 * match. The caller MUST NOT compile or run bytes for which this returns false.
 */
static bool
nkvx_wasm_verify(const void *bytes, size_t len, const uint8_t bound[SPDK_KV_EXEC_SHA256_LEN])
{
	uint8_t got[SPDK_KV_EXEC_SHA256_LEN];

	if (!nkvx_sha256(bytes, len, got)) {
		return false;
	}
	/* CRYPTO_memcmp is constant-time; a full-length compare either way. */
	return CRYPTO_memcmp(got, bound, SPDK_KV_EXEC_SHA256_LEN) == 0;
}

/*
 * One cached COMPILED module, keyed by the content hash. We store the SERIALIZED
 * compilation artifact (engine-portable blob) so a hit deserializes cheaply into
 * the per-object zero-copy engine instead of recompiling. The serialized blob is
 * produced ONLY from verified bytes (see module_insert), so the mere existence of
 * an entry is an authorization fact: its key IS the blessed hash.
 */
struct nkvx_module_entry {
	uint8_t				sha256[SPDK_KV_EXEC_SHA256_LEN];
	uint8_t				*serialized;	/* portable compiled blob, owned */
	size_t				serialized_len;
	STAILQ_ENTRY(nkvx_module_entry)	link;
};

/*
 * EVICTION (spdk-wwy): the sha256 module cache is bounded by an LRU policy too
 * (count + serialized bytes), env-overridable. A module entry has NO external pin
 * (a lookup COPIES the blob out under the mutex and deserializes without holding
 * it, so eviction can never free a blob in use), so this is a plain LRU drop —
 * MRU at the tail, evict from the head. Re-running an evicted module simply
 * re-verifies+recompiles from the fetched bytes on the next miss.
 */
#define NKVX_MOD_CACHE_MAX_COUNT_DEFAULT	128ull
#define NKVX_MOD_CACHE_MAX_BYTES_DEFAULT	(256ull * 1024ull * 1024ull)	/* 256 MiB */
#define NKVX_ENV_MOD_CACHE_MAX_COUNT		"SPDK_NKVX_MOD_CACHE_MAX_COUNT"
#define NKVX_ENV_MOD_CACHE_MAX_BYTES		"SPDK_NKVX_MOD_CACHE_MAX_BYTES"

static struct {
	pthread_mutex_t				mutex;
	STAILQ_HEAD(, nkvx_module_entry)	modules;
	uint64_t				count;
	uint64_t				bytes;
	bool					inited;
} g_modcache = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
};

static void
nkvx_modcache_init_once(void)
{
	if (!g_modcache.inited) {
		STAILQ_INIT(&g_modcache.modules);
		g_modcache.count = 0;
		g_modcache.bytes = 0;
		g_modcache.inited = true;
	}
}

/* Find a cached compiled module by hash (mutex held). LRU touch on hit is done by
 * the caller where it has write intent; lookup itself is read-only. */
static struct nkvx_module_entry *
nkvx_modcache_lookup(const uint8_t sha256[SPDK_KV_EXEC_SHA256_LEN])
{
	struct nkvx_module_entry *e;

	STAILQ_FOREACH(e, &g_modcache.modules, link) {
		if (memcmp(e->sha256, sha256, SPDK_KV_EXEC_SHA256_LEN) == 0) {
			return e;
		}
	}
	return NULL;
}

/* Enforce the module-cache caps (mutex held): evict LRU entries from the head past
 * the count/byte caps. Plain LRU (no pins). Call AFTER inserting the new entry. */
static void
nkvx_modcache_enforce_caps(void)
{
	uint64_t max_count = nkvx_wasm_env_u64(NKVX_ENV_MOD_CACHE_MAX_COUNT,
					       NKVX_MOD_CACHE_MAX_COUNT_DEFAULT);
	uint64_t max_bytes = nkvx_wasm_env_u64(NKVX_ENV_MOD_CACHE_MAX_BYTES,
					       NKVX_MOD_CACHE_MAX_BYTES_DEFAULT);

	if (max_count == 0 && max_bytes == 0) {
		return;
	}
	while ((max_count != 0 && g_modcache.count > max_count) ||
	       (max_bytes != 0 && g_modcache.bytes > max_bytes)) {
		struct nkvx_module_entry *e = STAILQ_FIRST(&g_modcache.modules);

		if (e == NULL) {
			break;
		}
		STAILQ_REMOVE_HEAD(&g_modcache.modules, link);
		g_modcache.count--;
		g_modcache.bytes -= e->serialized_len;
		free(e->serialized);
		free(e);
		g_nkvx_wasm_stats.mod_evictions++;
	}
}

bool
kvdev_rados_nkvx_wasm_module_cached(const uint8_t sha256[SPDK_KV_EXEC_SHA256_LEN])
{
	bool found;

	if (sha256 == NULL) {
		return false;
	}
	pthread_mutex_lock(&g_modcache.mutex);
	nkvx_modcache_init_once();
	found = nkvx_modcache_lookup(sha256) != NULL;
	pthread_mutex_unlock(&g_modcache.mutex);
	return found;
}

int
kvdev_rados_nkvx_wasm_module_verify(const uint8_t sha256[SPDK_KV_EXEC_SHA256_LEN],
				    const void *bytes, size_t bytes_len)
{
	/* The gate (ADR-0010), compile-free: reactor-callable. We still require the
	 * runtime so a build/host that cannot compile reports the same distinct
	 * "unavailable" status the inline path used to. */
	if (nkvx_wasm_api() == NULL) {
		return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
	}
	if (sha256 == NULL || bytes == NULL || bytes_len == 0) {
		return SPDK_KVDEV_IO_STATUS_INVALID;
	}
	if (!nkvx_wasm_verify(bytes, bytes_len, sha256)) {
		SPDK_ERRLOG("nkvx/wasm: module hash MISMATCH — rejecting (bytes never compiled/run)\n");
		return SPDK_KVDEV_IO_STATUS_INVALID;
	}
	return SPDK_KVDEV_IO_STATUS_SUCCESS;
}

int
kvdev_rados_nkvx_wasm_module_insert(const uint8_t sha256[SPDK_KV_EXEC_SHA256_LEN],
				    const void *bytes, size_t bytes_len)
{
	const struct nkvx_wasm_api *api = nkvx_wasm_api();
	wasm_config_t *config = NULL;
	wasm_engine_t *engine = NULL;
	wasmtime_module_t *module_h = NULL;
	wasmtime_error_t *err = NULL;
	wasm_byte_vec_t blob;
	struct nkvx_module_entry *e = NULL;
	int status = SPDK_KVDEV_IO_STATUS_FAILED;

	if (api == NULL) {
		return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
	}
	if (sha256 == NULL || bytes == NULL || bytes_len == 0) {
		return SPDK_KVDEV_IO_STATUS_INVALID;
	}

	/* THE GATE (ADR-0010): verify BEFORE compiling. Unverified bytes never reach
	 * the compiler. A mismatch is an authorization failure, surfaced distinctly. */
	if (!nkvx_wasm_verify(bytes, bytes_len, sha256)) {
		SPDK_ERRLOG("nkvx/wasm: module hash MISMATCH — rejecting (bytes never compiled/run)\n");
		return SPDK_KVDEV_IO_STATUS_INVALID;
	}

	/* Idempotent: if a prior Exec already cached this hash, we are done. */
	pthread_mutex_lock(&g_modcache.mutex);
	nkvx_modcache_init_once();
	if (nkvx_modcache_lookup(sha256) != NULL) {
		pthread_mutex_unlock(&g_modcache.mutex);
		return SPDK_KVDEV_IO_STATUS_SUCCESS;
	}
	pthread_mutex_unlock(&g_modcache.mutex);

	/*
	 * Compile under a dedicated engine whose Tunables MATCH the per-object
	 * zero-copy engine (reservation=0/guard=0/may_move=false, fuel + epoch
	 * enabled) so the serialized artifact deserializes into that engine. The
	 * host_memory_creator is NOT set here (it does not affect serialization
	 * compatibility and is per-object).
	 */
	config = api->config_new();
	if (config == NULL) {
		return SPDK_KVDEV_IO_STATUS_FAILED;
	}
	api->config_consume_fuel_set(config, true);
	api->config_epoch_interruption_set(config, true);
	api->config_memory_reservation_set(config, 0);
	api->config_memory_guard_size_set(config, 0);
	api->config_memory_may_move_set(config, false);

	engine = api->engine_new_with_config(config);
	config = NULL;
	if (engine == NULL) {
		return SPDK_KVDEV_IO_STATUS_FAILED;
	}

	err = api->module_new(engine, bytes, bytes_len, &module_h);
	if (err != NULL) {
		nkvx_wasm_log_error(api, "module_new (verified module)", err);
		goto out;
	}

	memset(&blob, 0, sizeof(blob));
	err = api->module_serialize(module_h, &blob);
	if (err != NULL) {
		nkvx_wasm_log_error(api, "module_serialize", err);
		goto out;
	}

	e = calloc(1, sizeof(*e));
	if (e == NULL) {
		api->byte_vec_delete(&blob);
		status = SPDK_KVDEV_IO_STATUS_NOMEM;
		goto out;
	}
	e->serialized = malloc(blob.size);
	if (e->serialized == NULL) {
		api->byte_vec_delete(&blob);
		free(e);
		e = NULL;
		status = SPDK_KVDEV_IO_STATUS_NOMEM;
		goto out;
	}
	memcpy(e->serialized, blob.data, blob.size);
	e->serialized_len = blob.size;
	memcpy(e->sha256, sha256, SPDK_KV_EXEC_SHA256_LEN);
	api->byte_vec_delete(&blob);

	pthread_mutex_lock(&g_modcache.mutex);
	nkvx_modcache_init_once();
	if (nkvx_modcache_lookup(sha256) != NULL) {
		/* Lost a race: another insert won. Drop ours, success either way. */
		pthread_mutex_unlock(&g_modcache.mutex);
		free(e->serialized);
		free(e);
		e = NULL;
		status = SPDK_KVDEV_IO_STATUS_SUCCESS;
		goto out;
	}
	STAILQ_INSERT_TAIL(&g_modcache.modules, e, link);	/* MRU at tail */
	g_modcache.count++;
	g_modcache.bytes += e->serialized_len;
	/* Bound the module cache (spdk-wwy): evict LRU entries past the cap. The
	 * just-inserted MRU entry is at the tail, so it is never the victim. */
	nkvx_modcache_enforce_caps();
	pthread_mutex_unlock(&g_modcache.mutex);
	e = NULL;	/* owned by the cache now */
	status = SPDK_KVDEV_IO_STATUS_SUCCESS;

out:
	if (module_h != NULL) {
		api->module_delete(module_h);
	}
	if (engine != NULL) {
		api->engine_delete(engine);
	}
	if (e != NULL) {
		free(e->serialized);
		free(e);
	}
	return status;
}

void
kvdev_rados_nkvx_wasm_module_cache_reset(void)
{
	struct nkvx_module_entry *e;

	pthread_mutex_lock(&g_modcache.mutex);
	nkvx_modcache_init_once();
	while ((e = STAILQ_FIRST(&g_modcache.modules)) != NULL) {
		STAILQ_REMOVE_HEAD(&g_modcache.modules, link);
		free(e->serialized);
		free(e);
	}
	g_modcache.count = 0;
	g_modcache.bytes = 0;
	pthread_mutex_unlock(&g_modcache.mutex);
}

uint64_t
kvdev_rados_nkvx_wasm_module_cache_count(void)
{
	uint64_t n;

	pthread_mutex_lock(&g_modcache.mutex);
	nkvx_modcache_init_once();
	n = g_modcache.count;
	pthread_mutex_unlock(&g_modcache.mutex);
	return n;
}

/*
 * Obtain a compiled module for \c mod, ready to instantiate in \c engine. On a
 * sha256-cache hit, deserialize the cached artifact (no recompile). On a miss,
 * VERIFY + compile + cache via module_insert, then deserialize. Returns a freshly
 * owned wasmtime_module_t in *out_mod (caller module_delete's it), or a negative
 * SPDK_KVDEV_IO_STATUS_* on failure (mismatch/compile/missing bytes). NEVER
 * returns a module for unverified bytes.
 */
static int
nkvx_module_obtain(const struct nkvx_wasm_api *api, wasm_engine_t *engine,
		   const struct kvdev_rados_nkvx_module *mod, wasmtime_module_t **out_mod)
{
	struct nkvx_module_entry *e;
	uint8_t *blob = NULL;
	size_t blob_len = 0;
	wasmtime_error_t *err;
	wasmtime_module_t *m = NULL;
	int rc;

	*out_mod = NULL;

	pthread_mutex_lock(&g_modcache.mutex);
	nkvx_modcache_init_once();
	e = nkvx_modcache_lookup(mod->sha256);
	if (e != NULL) {
		/* Copy the blob out under the lock so a concurrent reset can't free it
		 * under us; deserialize without holding the modcache mutex. */
		blob = malloc(e->serialized_len);
		if (blob == NULL) {
			pthread_mutex_unlock(&g_modcache.mutex);
			return SPDK_KVDEV_IO_STATUS_NOMEM;
		}
		memcpy(blob, e->serialized, e->serialized_len);
		blob_len = e->serialized_len;
		/* LRU touch: a reused module is hot — move it to the tail (mutex held). */
		STAILQ_REMOVE(&g_modcache.modules, e, nkvx_module_entry, link);
		STAILQ_INSERT_TAIL(&g_modcache.modules, e, link);
	}
	pthread_mutex_unlock(&g_modcache.mutex);

	if (blob == NULL) {
		/* Miss: verify + compile + cache from the fetched bytes. */
		if (mod->bytes == NULL || mod->bytes_len == 0) {
			SPDK_ERRLOG("nkvx/wasm: module-cache miss but no fetched bytes supplied\n");
			return SPDK_KVDEV_IO_STATUS_FAILED;
		}
		rc = kvdev_rados_nkvx_wasm_module_insert(mod->sha256, mod->bytes, mod->bytes_len);
		if (rc != SPDK_KVDEV_IO_STATUS_SUCCESS) {
			return rc;	/* mismatch (INVALID) / compile (FAILED) — never run */
		}
		pthread_mutex_lock(&g_modcache.mutex);
		e = nkvx_modcache_lookup(mod->sha256);
		if (e != NULL) {
			blob = malloc(e->serialized_len);
			if (blob != NULL) {
				memcpy(blob, e->serialized, e->serialized_len);
				blob_len = e->serialized_len;
			}
		}
		pthread_mutex_unlock(&g_modcache.mutex);
		if (blob == NULL) {
			return SPDK_KVDEV_IO_STATUS_FAILED;
		}
	}

	err = api->module_deserialize(engine, blob, blob_len, &m);
	free(blob);
	if (err != NULL) {
		nkvx_wasm_log_error(api, "module_deserialize", err);
		return SPDK_KVDEV_IO_STATUS_FAILED;
	}
	*out_mod = m;
	return SPDK_KVDEV_IO_STATUS_SUCCESS;
}

/* ==========================================================================
 * TB4: content-addressed object cache + zero-copy DMA + warm-instance cache
 * (spdk-ii0 / ADR-0013). The executor owns this store; it never calls librados.
 * ========================================================================== */

#define NKVX_WASM_PAGE 65536u		/* wasm linear-memory page size */

/*
 * A cached object: the content-addressed buffer, keyed by obj_key. The buffer is
 * page-rounded and OWNS the bytes; it is what backs wasm linear memory zero-copy.
 * mem_cap is the full backing size handed to wasmtime (object bytes start at
 * WASM_OBJ_OFF inside it, mirroring the plain-copy ABI so the SAME .wasm modules
 * work unchanged); obj_len is the true object length the module is told.
 */
struct nkvx_obj_entry {
	char			obj_key[256];
	uint8_t			*mem;		/* page-rounded backing buffer */
	size_t			mem_cap;	/* allocated/back-able bytes */
	size_t			mem_size;	/* current "committed" size for wasm */
	size_t			obj_len;	/* true object length */
	bool			filled;		/* cold fill completed */
	/*
	 * Invalidation / probe-hit carry-ref (spdk-ii0 D2). refcount counts the
	 * live PINS on this entry's buffer (a datapath probe-hit takes one, the
	 * warm entries that alias the buffer each take one). dead means the entry
	 * has been invalidated (Store/Delete): it is unlinked from the lookup list
	 * so a subsequent Exec MISSES and cold-fills fresh (no stale read), while
	 * the buffer survives until the last pin drops (no use-after-free for an
	 * Exec that pinned this version before the mutation). An entry is freed when
	 * refcount==0 AND (dead OR explicit reset).
	 */
	int			refcount;
	bool			dead;
	STAILQ_ENTRY(nkvx_obj_entry) link;
};

/*
 * A warm wasm engine+compiled-module, keyed by (module, obj_key). Caches the
 * EXPENSIVE artifacts — the engine (with the per-object zero-copy MemoryCreator
 * wired into its config) and the compiled module — so a repeat Exec of the same
 * pair skips engine creation + module compile/deserialize. Pinned to the object
 * entry whose buffer backs its memory.
 *
 * STATE ISOLATION (spdk-yc1): the per-Exec STORE + INSTANCE are deliberately NOT
 * cached. Each Exec instantiates a fresh store+instance from the warm engine +
 * compiled module and deletes the store when done. Re-instantiation resets every
 * wasm GLOBAL to its declared init and RE-RUNS the module's data-segment
 * initializers, so module state never leaks from one Exec to the next. The
 * expensive compile/engine stay warm (instantiation is cheap relative to compile),
 * so this keeps the warm-cache perf win while making each Exec start from a clean
 * instance. A warm HIT still means "engine+compiled module reused" (warm_hits
 * increments); only the cheap, isolation-critical instance is rebuilt per run.
 */
struct nkvx_warm_entry {
	char			module[64];
	char			obj_key[256];
	wasm_engine_t		*engine;
	wasmtime_module_t	*module_h;
	struct nkvx_obj_entry	*obj;		/* backing object (zero-copy alias) */
	STAILQ_ENTRY(nkvx_warm_entry) link;
};

/*
 * EVICTION (spdk-wwy). The object + warm caches are bounded by an LRU policy so
 * they cannot grow without bound (important for 64 MiB partitions — a handful of
 * objects would otherwise pin gigabytes). Each list is kept in LRU order:
 * most-recently-USED at the TAIL, least-recently-used at the HEAD; a touch on
 * access re-links the entry to the tail. On insert past the cap we evict from the
 * head.
 *
 * RACE SAFETY (spdk-ii0 D2 carry-ref): eviction MUST reuse the existing pin/unref
 * machinery — it must NOT free a PINNED object entry (a probe-hit Exec carries a
 * pin from cache_pin through to the worker run, and a warm entry pins the object it
 * aliases). Evicting an object therefore (1) tears down ALL its warm entries first
 * (each drops its object pin), then (2) marks the object dead and runs it through
 * nkvx_obj_unref: an unpinned object is removed+freed immediately, a still-pinned
 * one stays dead-on-list and is freed by the LAST unpin — exactly as invalidation
 * does. A dead object is invisible to nkvx_obj_lookup, so the probe→dispatch path
 * is unaffected (it already pins; a pinned entry is never freed under it). No
 * use-after-free, no eviction of in-use bytes.
 *
 * Caps are env-overridable so tests and tuning can drive them; 0 disables a cap.
 */
#define NKVX_OBJ_CACHE_MAX_COUNT_DEFAULT	256ull
#define NKVX_OBJ_CACHE_MAX_BYTES_DEFAULT	(512ull * 1024ull * 1024ull)	/* 512 MiB */
#define NKVX_WARM_CACHE_MAX_COUNT_DEFAULT	256ull

#define NKVX_ENV_OBJ_CACHE_MAX_COUNT	"SPDK_NKVX_OBJ_CACHE_MAX_COUNT"
#define NKVX_ENV_OBJ_CACHE_MAX_BYTES	"SPDK_NKVX_OBJ_CACHE_MAX_BYTES"
#define NKVX_ENV_WARM_CACHE_MAX_COUNT	"SPDK_NKVX_WARM_CACHE_MAX_COUNT"

static struct {
	pthread_mutex_t				mutex;
	STAILQ_HEAD(, nkvx_obj_entry)		objects;
	STAILQ_HEAD(, nkvx_warm_entry)		warm;
	uint64_t				obj_count;	/* LIVE+dead objects on list */
	uint64_t				obj_bytes;	/* sum of mem_cap on list */
	uint64_t				warm_count;
	bool					inited;
} g_cache = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
};

static void
nkvx_cache_init_once(void)
{
	if (!g_cache.inited) {
		STAILQ_INIT(&g_cache.objects);
		STAILQ_INIT(&g_cache.warm);
		g_cache.obj_count = 0;
		g_cache.obj_bytes = 0;
		g_cache.warm_count = 0;
		g_cache.inited = true;
	}
}

/* Round x up to a multiple of a. */
static size_t
nkvx_round_up(size_t x, size_t a)
{
	return (x + a - 1) & ~(a - 1);
}

static void nkvx_warm_free(const struct nkvx_wasm_api *api, struct nkvx_warm_entry *w);

/* Find a LIVE (non-dead) cached object by key (mutex held). A dead entry has been
 * invalidated and is awaiting its last pin to drop; it must NOT be served to a new
 * Exec (that would be a stale read), so lookup skips it and the caller cold-fills
 * a fresh version. */
static struct nkvx_obj_entry *
nkvx_obj_lookup(const char *obj_key)
{
	struct nkvx_obj_entry *e;

	STAILQ_FOREACH(e, &g_cache.objects, link) {
		if (!e->dead && strcmp(e->obj_key, obj_key) == 0) {
			return e;
		}
	}
	return NULL;
}

/* Free an object entry's backing + wrapper (mutex held; refcount must be 0). */
static void
nkvx_obj_free(struct nkvx_obj_entry *o)
{
	free(o->mem);
	free(o);
}

/* Link an object onto the cache list as MOST-recently-used (tail) and account it
 * (mutex held). */
static void
nkvx_obj_list_add(struct nkvx_obj_entry *o)
{
	STAILQ_INSERT_TAIL(&g_cache.objects, o, link);
	g_cache.obj_count++;
	g_cache.obj_bytes += o->mem_cap;
}

/* Unlink an object from the cache list and un-account it (mutex held). Does not
 * free. */
static void
nkvx_obj_list_remove(struct nkvx_obj_entry *o)
{
	STAILQ_REMOVE(&g_cache.objects, o, nkvx_obj_entry, link);
	assert(g_cache.obj_count > 0);
	g_cache.obj_count--;
	g_cache.obj_bytes -= o->mem_cap;
}

/* LRU touch: move a live object to the tail (most-recently-used) so eviction takes
 * the genuinely-coldest entry from the head (mutex held). */
static void
nkvx_obj_touch(struct nkvx_obj_entry *o)
{
	STAILQ_REMOVE(&g_cache.objects, o, nkvx_obj_entry, link);
	STAILQ_INSERT_TAIL(&g_cache.objects, o, link);
}

/* Drop one pin on an object entry (mutex held). When the last pin drops and the
 * entry is dead (invalidated OR evicted), free it now — this is the carry-ref
 * guarantee that an in-flight Exec which pinned a version before a Store/eviction
 * can finish over that version without a use-after-free, while the entry is already
 * invisible to new Execs. */
static void
nkvx_obj_unref(struct nkvx_obj_entry *o)
{
	assert(o->refcount > 0);
	o->refcount--;
	if (o->refcount == 0 && o->dead) {
		/* A dead entry stays ON the objects list (invisible to lookup via the
		 * dead flag) until its last pin drops, so cache_reset can always find
		 * and free it. Remove + free it now that no pin remains. */
		nkvx_obj_list_remove(o);
		nkvx_obj_free(o);
	}
}

/* Tear down a warm entry: unlink + un-account it, free the wasm artifacts, and
 * drop the pin it held on its backing object (mutex held). The object unref may
 * free a dead/evicted object once its last pin drops (carry-ref). */
static void
nkvx_warm_destroy(const struct nkvx_wasm_api *api, struct nkvx_warm_entry *w)
{
	struct nkvx_obj_entry *obj = w->obj;

	STAILQ_REMOVE(&g_cache.warm, w, nkvx_warm_entry, link);
	assert(g_cache.warm_count > 0);
	g_cache.warm_count--;
	if (api != NULL) {
		nkvx_warm_free(api, w);
	} else {
		free(w);
	}
	if (obj != NULL) {
		nkvx_obj_unref(obj);
	}
}

/* LRU touch a warm entry to the tail (mutex held). */
static void
nkvx_warm_touch(struct nkvx_warm_entry *w)
{
	STAILQ_REMOVE(&g_cache.warm, w, nkvx_warm_entry, link);
	STAILQ_INSERT_TAIL(&g_cache.warm, w, link);
}

/*
 * Invalidate every cache entry for obj_key (mutex held): drop ALL warm instances
 * pinned to it (they alias the buffer; tearing each down drops its object pin) and
 * mark the object entry dead. A dead entry is skipped by nkvx_obj_lookup, so a
 * subsequent Exec MISSES and cold-fills a fresh version — never a stale hit. The
 * entry's buffer survives (and stays on the list) until the last outstanding pin
 * drops (carry-ref: an in-flight probe-hit Exec finishes over its pinned version
 * without a use-after-free), then nkvx_obj_unref removes + frees it. An unpinned
 * entry hits refcount 0 immediately below and is freed right away. */
static void
nkvx_cache_invalidate_locked(const struct nkvx_wasm_api *api, const char *obj_key)
{
	struct nkvx_warm_entry *w, *wtmp;
	struct nkvx_obj_entry *o, *otmp;

	/* Warm instances first: each aliases an object buffer and holds a pin on it,
	 * so tearing the warm entry down (and unref'ing its object) drops that pin. */
	STAILQ_FOREACH_SAFE(w, &g_cache.warm, link, wtmp) {
		if (strcmp(w->obj_key, obj_key) == 0) {
			nkvx_warm_destroy(api, w);
		}
	}

	STAILQ_FOREACH_SAFE(o, &g_cache.objects, link, otmp) {
		if (!o->dead && strcmp(o->obj_key, obj_key) == 0) {
			/* Take a temporary pin so the mark+maybe-free goes through the
			 * single unref path: refcount>0 entries stay dead-on-list, an
			 * otherwise-unpinned entry is removed + freed immediately. */
			o->dead = true;
			o->refcount++;
			nkvx_obj_unref(o);
		}
	}
}

/*
 * Evict ONE object entry e (mutex held), race-safely. First tear down ALL warm
 * entries that pin e (dropping their pins), then mark e dead and run it through the
 * single unref path: an unpinned e is removed+freed now, a still-pinned e (a
 * probe-hit Exec carries a pin) stays dead-on-list and is freed by its last unpin.
 * NEVER frees pinned bytes — same carry-ref discipline as invalidation. The caller
 * must have selected e from the LRU head among LIVE entries.
 */
static void
nkvx_obj_evict_locked(const struct nkvx_wasm_api *api, struct nkvx_obj_entry *e)
{
	struct nkvx_warm_entry *w, *wtmp;

	STAILQ_FOREACH_SAFE(w, &g_cache.warm, link, wtmp) {
		if (w->obj == e) {
			nkvx_warm_destroy(api, w);
		}
	}
	e->dead = true;
	e->refcount++;
	nkvx_obj_unref(e);	/* freed now if unpinned; deferred to last unpin if pinned */
	g_nkvx_wasm_stats.obj_evictions++;
}

/*
 * Enforce the object-cache caps (mutex held): while the LIVE object count or live
 * bytes exceed the (env-overridable) caps, evict the least-recently-used LIVE
 * object from the head. Dead entries are awaiting their last unpin and are skipped
 * (they neither serve lookups nor can be re-evicted). A live-but-PINNED LRU entry
 * is evicted safely (marked dead, freed on last unpin); we still advance past it so
 * a fully-pinned cache cannot loop forever. Call AFTER inserting the new entry.
 */
static void
nkvx_obj_cache_enforce_caps(const struct nkvx_wasm_api *api)
{
	uint64_t max_count = nkvx_wasm_env_u64(NKVX_ENV_OBJ_CACHE_MAX_COUNT,
					       NKVX_OBJ_CACHE_MAX_COUNT_DEFAULT);
	uint64_t max_bytes = nkvx_wasm_env_u64(NKVX_ENV_OBJ_CACHE_MAX_BYTES,
					       NKVX_OBJ_CACHE_MAX_BYTES_DEFAULT);
	struct nkvx_obj_entry *o, *otmp;

	if (max_count == 0 && max_bytes == 0) {
		return;		/* caps disabled */
	}

	/* Count only LIVE entries against the count cap (dead ones are transient). */
	for (;;) {
		uint64_t live = 0;
		struct nkvx_obj_entry *victim = NULL;

		STAILQ_FOREACH(o, &g_cache.objects, link) {
			if (!o->dead) {
				live++;
				if (victim == NULL) {
					victim = o;	/* LRU live = head-most live */
				}
			}
		}
		bool over_count = (max_count != 0 && live > max_count);
		bool over_bytes = (max_bytes != 0 && g_cache.obj_bytes > max_bytes);

		if (!over_count && !over_bytes) {
			break;
		}
		if (victim == NULL) {
			break;		/* nothing live to evict */
		}
		(void)otmp;
		nkvx_obj_evict_locked(api, victim);
		/* Guard against a pathological all-pinned set: if obj_bytes did not drop
		 * (victim was pinned, deferred free) and it was the only live entry, stop. */
		if (live == 1) {
			break;
		}
	}
}

/* Enforce the warm-cache count cap (mutex held): evict LRU warm entries from the
 * head. A warm entry holds no external pin (only the object pin it drops on
 * destroy), so this is a plain LRU drop — no carry-ref needed. Call AFTER insert. */
static void
nkvx_warm_cache_enforce_caps(const struct nkvx_wasm_api *api)
{
	uint64_t max_count = nkvx_wasm_env_u64(NKVX_ENV_WARM_CACHE_MAX_COUNT,
					       NKVX_WARM_CACHE_MAX_COUNT_DEFAULT);

	if (max_count == 0) {
		return;
	}
	while (g_cache.warm_count > max_count) {
		struct nkvx_warm_entry *w = STAILQ_FIRST(&g_cache.warm);

		if (w == NULL) {
			break;
		}
		nkvx_warm_destroy(api, w);
		g_nkvx_wasm_stats.warm_evictions++;
	}
}

/*
 * Find a warm instance by (module, object) (mutex held). Matching on the OBJECT
 * POINTER (not just the key) is essential for D2 correctness: after an
 * invalidation a dead (pinned) object and a fresh object can briefly share the
 * same key, and a warm instance is bound to exactly one object's buffer. Keying
 * on the pointer guarantees a run never reuses a warm instance aliasing the wrong
 * (e.g. dead) object's bytes.
 */
static struct nkvx_warm_entry *
nkvx_warm_lookup(const char *module, const struct nkvx_obj_entry *obj)
{
	struct nkvx_warm_entry *e;

	STAILQ_FOREACH(e, &g_cache.warm, link) {
		if (strcmp(e->module, module) == 0 && e->obj == obj) {
			return e;
		}
	}
	return NULL;
}

/*
 * The zero-copy MemoryCreator (ADR-0013). Per run we hand wasmtime a pointer to
 * the cached object's page-rounded buffer through these callbacks; wasmtime uses
 * it verbatim as the linear-memory backing, so wasmtime_memory_data() returns the
 * SAME pointer the cache holds — no copy-in.
 *
 * Growth is refused: a TB4 zero-copy object is immutable and exactly sized to its
 * (committed) page-rounded length, so a memory.grow returns an error (contained,
 * not a crash) — the modules in this path do not grow.
 */
struct nkvx_zc_mem {
	uint8_t		*base;
	size_t		size;
	size_t		cap;
	/* When the module declares MORE linear memory than the object-sized
	 * zero-copy backing provides, base points at a private, page-aligned buffer
	 * we own (object bytes copied in, tail zeroed) instead of the cached object
	 * buffer; finalize must free it. Zero-copy (owned == false) is the common
	 * path; this fallback preserves correctness for memory-hungry modules on
	 * small objects (spdk-ii0 backing-size defect). */
	bool		owned;
};

static uint8_t *
nkvx_zc_get(void *env, size_t *byte_size, size_t *byte_capacity)
{
	struct nkvx_zc_mem *m = env;

	*byte_size = m->size;
	*byte_capacity = m->cap;
	return m->base;
}

static wasmtime_error_t *
nkvx_zc_grow(void *env, size_t new_size)
{
	struct nkvx_zc_mem *m = env;

	/* Allow a no-op / shrink-to-fit; refuse real growth of an immutable object. */
	if (new_size <= m->size) {
		return NULL;
	}
	if (new_size <= m->cap) {
		m->size = new_size;
		return NULL;
	}
	/* Cannot grow a zero-copy DMA backing past its reservation: return a real
	 * wasmtime error so wasmtime can format/free it normally. wasmtime turns
	 * this into a trap, contained by the run. */
	return g_api.error_new("nkvx: cannot grow zero-copy linear memory past its backing");
}

static void
nkvx_zc_finalize(void *env)
{
	struct nkvx_zc_mem *m = env;

	/* The cache owns the zero-copy backing (do NOT free it). Only the private
	 * fallback buffer (owned) is ours to release. Always free the small wrapper. */
	if (m != NULL && m->owned) {
		free(m->base);
	}
	free(m);
}

/*
 * new_memory: wasmtime asks for a fresh linear memory for the on-demand instance.
 * We return the cached object's buffer as the backing (zero-copy).
 *
 * Two safety properties combine to make this equivalent to a normal wasm linear
 * memory (OOB always traps):
 *   1. The engine config forces DYNAMIC bounds checks (reservation=0, guard=0),
 *      so every guest access is checked against the memory's REPORTED length even
 *      though our buffer has no guard page.
 *   2. The reported length (byte_size) is the module's own declared, page-rounded
 *      minimum -- NOT the (possibly larger) object backing (spdk-ii0 D1). So a
 *      module that declares fewer pages than the backing holds cannot reach the
 *      slack: an access past its declared minimum traps, exactly as it would
 *      against a real linear memory of that size. The full backing is still the
 *      allocation (byte_capacity) so the alias stays zero-copy and memory.grow
 *      may extend the reported length up to the backing (nkvx_zc_grow).
 * Growth beyond the backing is refused by nkvx_zc_grow. (See the oob.wasm and
 * the slackwrite.wasm regression tests, which trap on an access past the declared
 * memory.)
 */
struct nkvx_zc_ctx {
	uint8_t		*base;
	size_t		size;
	size_t		cap;
};

static wasmtime_error_t *
nkvx_zc_new_memory(void *env, const wasm_memorytype_t *ty, size_t minimum,
		   size_t maximum, size_t reserved_size_in_bytes,
		   size_t guard_size_in_bytes, wasmtime_linear_memory_t *memory_ret)
{
	struct nkvx_zc_ctx *cctx = env;
	struct nkvx_zc_mem *m;

	(void)ty;
	(void)maximum;
	(void)reserved_size_in_bytes;
	(void)guard_size_in_bytes;

	m = calloc(1, sizeof(*m));
	if (m == NULL) {
		return g_api.error_new("nkvx: out of memory allocating linear-memory wrapper");
	}

	if (minimum <= cctx->cap) {
		/*
		 * Common path: the object-sized backing already covers the module's
		 * declared minimum -> alias it ZERO-COPY (no copy-in).
		 *
		 * SANDBOX SEMANTICS (spdk-ii0 D1): the REPORTED size (byte_size, what
		 * wasmtime dynamic-bounds-checks every guest access against) MUST be the
		 * module's declared, page-rounded minimum -- NOT the full object backing.
		 * If we reported the whole backing, a module that declared e.g. 1 page
		 * could read/write the slack between its declared size and a larger
		 * object backing without trapping (contained to the object's own buffer,
		 * but a violation of "OOB always traps"). We therefore set m->size =
		 * minimum (already page-rounded by wasmtime) while keeping the full
		 * backing as m->cap so the alias stays zero-copy and memory.grow can
		 * still extend up to the backing (nkvx_zc_grow). An access past the
		 * declared minimum now traps. */
		m->base = cctx->base;
		m->size = minimum;
		m->cap = cctx->cap;
		m->owned = false;
	} else {
		/* The module declares MORE linear memory than the object-sized backing
		 * provides (e.g. a 2-page module run against a sub-page object). Zero-copy
		 * is impossible here, so fall back to a private, page-aligned buffer of the
		 * module's minimum: copy the cold-filled object bytes (preserving their
		 * WASM_OBJ_OFF layout) and zero the tail. Correctness over zero-copy for
		 * this case (spdk-ii0 backing-size defect); the common/large-object path
		 * stays zero-copy. */
		size_t need = nkvx_round_up(minimum, NKVX_WASM_PAGE);
		uint8_t *priv = NULL;

		if (posix_memalign((void **)&priv, NKVX_WASM_PAGE, need) != 0) {
			free(m);
			return g_api.error_new("nkvx: out of memory growing linear memory for module");
		}
		memcpy(priv, cctx->base, cctx->size);
		memset(priv + cctx->size, 0, need - cctx->size);
		m->base = priv;
		m->size = minimum;
		m->cap = need;
		m->owned = true;
		SPDK_NOTICELOG("nkvx/wasm: module min %zuB exceeds object backing %zuB; using a "
			       "private %zuB linear memory (not zero-copy)\n",
			       minimum, cctx->cap, need);
	}

	memory_ret->env = m;
	memory_ret->get_memory = nkvx_zc_get;
	memory_ret->grow_memory = nkvx_zc_grow;
	memory_ret->finalizer = nkvx_zc_finalize;
	return NULL;
}

/*
 * Build and cache the WARM artifacts (engine + compiled module) for a
 * (module, object), with the cached object's buffer wired in zero-copy via the
 * MemoryCreator on the engine config. The per-Exec store + instance are NOT built
 * here (state isolation, spdk-yc1) — they are created fresh on every run in
 * nkvx_run_on_object_locked. On success *out_warm points at the cached warm entry
 * (owned by the cache). Caps are applied per-run by the caller, not here.
 */
static int
nkvx_warm_build(const struct nkvx_wasm_api *api, const char *module,
		const struct kvdev_rados_nkvx_module *mod,
		struct nkvx_obj_entry *obj, struct nkvx_warm_entry **out_warm)
{
	char path[1024];
	uint8_t *wasm = NULL;
	size_t wasm_len = 0;
	wasm_config_t *config = NULL;
	wasm_engine_t *engine = NULL;
	wasmtime_module_t *module_h = NULL;
	wasmtime_error_t *err = NULL;
	struct nkvx_warm_entry *w = NULL;
	struct nkvx_zc_ctx *zc = NULL;
	wasmtime_memory_creator_t creator;
	int rc = -1;

	/*
	 * TB3 (spdk-fbm): the VERIFIED path supplies \c mod (the bound sha256 + the
	 * fetched bytes). We obtain the compiled module from the sha256 cache instead
	 * of reading an unverified file from disk. The legacy filesystem-load-by-name
	 * path (mod == NULL) is retained only for the in-tree unit tests that drive
	 * the executor directly; the live datapath always passes a verified \c mod.
	 */
	if (mod == NULL) {
		if (nkvx_wasm_module_path(module, path, sizeof(path)) != 0) {
			return -1;
		}
		wasm = nkvx_wasm_read_file(path, &wasm_len);
		if (wasm == NULL) {
			return -1;
		}
	}

	/* Per-binding zero-copy context: hand the cache buffer to the MemoryCreator.
	 * Owned by the config's creator finalizer once set. */
	zc = calloc(1, sizeof(*zc));
	if (zc == NULL) {
		goto out;
	}
	zc->base = obj->mem;
	zc->size = obj->mem_size;
	zc->cap = obj->mem_cap;

	config = api->config_new();
	if (config == NULL) {
		goto out;
	}
	/*
	 * Enable the FUEL feature on the WARM engine's config (an engine property;
	 * the warm engine is reused across invocations), so each call can (re)set a
	 * fuel ceiling. ALSO enable EPOCH INTERRUPTION (spdk-ii0 D3, closes spdk-9jl):
	 * the warm path must have the same wall-clock backstop as run() to catch a
	 * runaway that fuel under-counts. CRITICAL (spdk-9jl): epoch interruption
	 * WITHOUT a deadline traps immediately, so a per-run epoch deadline is set on
	 * EVERY call (and the ticker armed only when the epoch cap is active) in
	 * nkvx_run_on_object_locked — never left armed with the default 0 deadline.
	 */
	api->config_consume_fuel_set(config, true);
	api->config_epoch_interruption_set(config, true);
	/*
	 * Force DYNAMIC bounds-checks for the zero-copy host memory (spdk-ii0 B1).
	 * Our MemoryCreator hands wasmtime an exact-sized buffer with NO guard
	 * region; with the default static elision an out-of-bounds guest access
	 * would not be caught. reservation=0 + guard=0 makes wasmtime emit an
	 * explicit check against the memory length on every access, so OOB always
	 * traps (contained, not a host-heap clobber). may_move=false keeps the
	 * backing pinned (grow on the immutable object is refused regardless).
	 */
	api->config_memory_reservation_set(config, 0);
	api->config_memory_guard_size_set(config, 0);
	api->config_memory_may_move_set(config, false);
	creator.env = zc;
	creator.new_memory = nkvx_zc_new_memory;
	creator.finalizer = free;		/* frees the zc ctx when engine drops */
	api->config_host_memory_creator_set(config, &creator);
	zc = NULL;				/* ownership handed to the creator/engine */

	engine = api->engine_new_with_config(config);
	config = NULL;
	if (engine == NULL) {
		goto out;
	}

	if (mod != NULL) {
		/* VERIFIED path (ADR-0010): deserialize the sha256-cached compiled module
		 * (verify+compile happened in module_insert on the cache-miss). This never
		 * yields a module for unverified bytes. */
		int orc = nkvx_module_obtain(api, engine, mod, &module_h);

		if (orc != SPDK_KVDEV_IO_STATUS_SUCCESS) {
			SPDK_ERRLOG("nkvx/wasm: could not obtain verified module '%s' (status %d)\n",
				    module, orc);
			goto out;
		}
	} else {
		err = api->module_new(engine, wasm, wasm_len, &module_h);
		if (err != NULL) {
			nkvx_wasm_log_error(api, "module_new", err);
			goto out;
		}
	}

	w = calloc(1, sizeof(*w));
	if (w == NULL) {
		goto out;
	}
	snprintf(w->module, sizeof(w->module), "%s", module);
	snprintf(w->obj_key, sizeof(w->obj_key), "%s", obj->obj_key);
	w->engine = engine;
	w->module_h = module_h;
	w->obj = obj;
	/* The warm engine's MemoryCreator aliases obj->mem zero-copy on every
	 * instantiation: pin the object so an invalidation cannot free the buffer out
	 * from under this warm entry (spdk-ii0 D2). The pin is dropped when the warm
	 * entry is torn down. */
	obj->refcount++;

	STAILQ_INSERT_TAIL(&g_cache.warm, w, link);	/* MRU at tail */
	g_cache.warm_count++;
	*out_warm = w;

	/* Bound the warm cache (spdk-wwy): evict LRU warm entries past the cap. The
	 * just-inserted MRU entry is at the tail, so it is never the eviction victim. */
	nkvx_warm_cache_enforce_caps(api);

	/* Ownership transferred to the warm entry; do not tear down below. */
	engine = NULL;
	module_h = NULL;
	rc = 0;

out:
	if (module_h != NULL) {
		api->module_delete(module_h);
	}
	if (engine != NULL) {
		api->engine_delete(engine);
	}
	free(zc);
	free(wasm);
	return rc;
}

static void
nkvx_warm_free(const struct nkvx_wasm_api *api, struct nkvx_warm_entry *w)
{
	if (w->module_h != NULL) {
		api->module_delete(w->module_h);
	}
	if (w->engine != NULL) {
		api->engine_delete(w->engine);
	}
	free(w);
}

/*
 * Run module `name` against object entry `obj` (mutex held). Builds or reuses the
 * WARM engine+compiled module bound to THIS object, then instantiates a FRESH
 * store+instance for THIS Exec (state isolation, spdk-yc1), applies the
 * per-invocation caps, calls the module, extracts the result, and deletes the
 * store. Returns an SPDK_KVDEV_IO_STATUS_*. Shared by the key-based run_cached
 * (cold-fill path) and the pinned run (probe-skip path) so both arm identical caps
 * and zero-copy bookkeeping.
 *
 * STATE ISOLATION (spdk-yc1): the per-Exec store+instance are created here and torn
 * down before return. Re-instantiating resets every wasm GLOBAL to its declared
 * init and re-runs the module's data-segment initializers, so globals/scratch from
 * a prior Exec of the same (module,object) never leak into this one. The expensive
 * compile + engine stay warm (see nkvx_warm_entry), so the warm-cache perf win is
 * preserved while each Exec starts from a clean instance. Note the OBJECT region of
 * linear memory is the cached object buffer (zero-copy or private fallback) and is
 * intentionally the same bytes across Execs of the SAME object — that is the
 * object's own content, not cross-Exec state; only the module's instance state
 * (globals, declared data segments) is reset.
 */
static int
nkvx_run_on_object_locked(const struct nkvx_wasm_api *api, const char *name,
			  const struct kvdev_rados_nkvx_module *mod,
			  struct nkvx_obj_entry *obj,
			  void *out, uint32_t out_len, uint32_t *result_len)
{
	struct nkvx_warm_entry *warm;
	struct nkvx_wasm_caps caps;
	uint8_t *mem_base;
	size_t mem_size;
	wasmtime_store_t *store = NULL;
	wasmtime_context_t *ctx;
	wasmtime_instance_t instance;
	wasmtime_extern_t mem_ext, fn_ext;
	wasmtime_error_t *err = NULL;
	wasm_trap_t *trap = NULL;
	bool epoch_armed = false;
	wasmtime_val_t args[2], results[1];
	int status = SPDK_KVDEV_IO_STATUS_FAILED;

	/* ---- warm engine + compiled-module cache, keyed by (module, object) -- */
	warm = nkvx_warm_lookup(name, obj);
	if (warm != NULL) {
		g_nkvx_wasm_stats.warm_hits++;
		nkvx_warm_touch(warm);		/* LRU: a reused warm entry is hot */
	} else {
		if (nkvx_warm_build(api, name, mod, obj, &warm) != 0) {
			SPDK_ERRLOG("nkvx/wasm: warm build failed for '%s'/'%s'\n", name, obj->obj_key);
			return SPDK_KVDEV_IO_STATUS_FAILED;
		}
	}

	/*
	 * FRESH per-Exec store + instance (spdk-yc1). The warm engine carries the
	 * zero-copy MemoryCreator on its config, so this instantiation re-aliases the
	 * cached object buffer (zero-copy) while resetting all module globals/data.
	 */
	store = api->store_new(warm->engine, NULL, NULL);
	if (store == NULL) {
		SPDK_ERRLOG("nkvx/wasm: store_new failed for '%s'\n", name);
		return SPDK_KVDEV_IO_STATUS_FAILED;
	}
	ctx = api->store_context(store);

	nkvx_wasm_caps_load(&caps, mod != NULL ? mod->caps : 0);

	/* MEMORY CAP belt-and-suspenders on the warm path (the real bound is
	 * nkvx_zc_grow refusing growth past the backing; see spdk-90x). */
	if (caps.max_memory_bytes > 0) {
		api->store_limiter(store, (int64_t)caps.max_memory_bytes, -1, -1, -1, -1);
	}

	err = api->instance_new(ctx, warm->module_h, NULL, 0, &instance, &trap);
	if (err != NULL) {
		nkvx_wasm_log_error(api, "instance_new (warm)", err);
		goto out;
	}
	if (trap != NULL) {
		SPDK_ERRLOG("nkvx/wasm: warm instantiation trapped\n");
		api->trap_delete(trap);
		trap = NULL;
		goto out;
	}
	if (!api->instance_export_get(ctx, &instance, "memory", strlen("memory"), &mem_ext) ||
	    mem_ext.kind != WASMTIME_EXTERN_MEMORY) {
		SPDK_ERRLOG("nkvx/wasm: module '%s' has no exported 'memory'\n", name);
		goto out;
	}
	if (!api->instance_export_get(ctx, &instance, name, strlen(name), &fn_ext) ||
	    fn_ext.kind != WASMTIME_EXTERN_FUNC) {
		SPDK_ERRLOG("nkvx/wasm: module '%s' has no exported function '%s'\n", name, name);
		goto out;
	}

	/*
	 * READ-ONLY-EXECUTOR GUARANTEE (ADR-0008 D3, enforced at the mutation point).
	 * A module that exports the reserved write-intent symbol declares it intends to
	 * mutate the stored object; on a read-only namespace it is rejected with
	 * READ_ONLY *before* the entry function runs, so it can never mutate. The v1
	 * imports-free ABI cannot actually write back (no shipped module exports this),
	 * but probing here makes the guarantee hold by construction the moment a
	 * write-capable module exists, rather than relying on the "Exec never mutates"
	 * assumption ADR-0008 retired. A read-only module runs normally; mod==NULL (the
	 * in-tree/test plain path) is read-only-unconstrained and never reaches here.
	 */
	if (mod != NULL && mod->read_only) {
		wasmtime_extern_t wr_ext;

		if (api->instance_export_get(ctx, &instance,
					     KVDEV_RADOS_NKVX_WASM_WRITE_EXPORT,
					     strlen(KVDEV_RADOS_NKVX_WASM_WRITE_EXPORT),
					     &wr_ext) &&
		    wr_ext.kind == WASMTIME_EXTERN_FUNC) {
			SPDK_ERRLOG("nkvx/wasm: write-capable module '%s' (exports '%s') rejected "
				    "on a read-only namespace -> READ_ONLY (ADR-0008 D3)\n",
				    name, KVDEV_RADOS_NKVX_WASM_WRITE_EXPORT);
			status = SPDK_KVDEV_IO_STATUS_READ_ONLY;
			goto out;
		}
	}

	/* ---- zero-copy proof: linear memory aliases the cache buffer -------- */
	mem_base = api->memory_data(ctx, &mem_ext.of.memory);
	mem_size = api->memory_data_size(ctx, &mem_ext.of.memory);
	g_nkvx_wasm_stats.last_mem_base = mem_base;
	g_nkvx_wasm_stats.last_cache_base = obj->mem;

	/*
	 * The warm engine ALWAYS has the fuel feature enabled (see nkvx_warm_build),
	 * so the store starts each instantiation with 0 fuel and would trap unless we
	 * (re)set it every call. Set the per-invocation ceiling, or a very large
	 * value when the caps disable fuel (so an "unlimited" run still proceeds). */
	{
		uint64_t fuel = caps.fuel_ceiling > 0 ? caps.fuel_ceiling : UINT64_MAX;

		err = api->context_set_fuel(ctx, fuel);
		if (err != NULL) {
			nkvx_wasm_log_error(api, "context_set_fuel", err);
			goto out;
		}
	}

	/*
	 * EPOCH / WALL-CLOCK CAP (spdk-ii0 D3, closes spdk-9jl): arm the per-run epoch
	 * deadline and the background ticker on the warm engine so a fuel-undercounting
	 * runaway is stopped by wall-clock, matching run()'s ~100ms backstop. The warm
	 * engine has epoch interruption enabled at build time, so a deadline MUST be
	 * set on EVERY call or the engine traps immediately (spdk-9jl). When the caps
	 * enable the epoch cap we set the real per-run deadline and arm the ticker for
	 * THIS engine; when the caps disable it we set an effectively-infinite deadline
	 * (and do NOT arm the ticker) so the run proceeds unbounded by wall-clock but
	 * never trips an unset deadline. The ticker is disarmed right after the call,
	 * before the mutex is dropped, so it never advances a stale/reused engine. */
	if (caps.epoch_deadline_ticks > 0) {
		api->context_set_epoch_deadline(ctx, caps.epoch_deadline_ticks);
		nkvx_wasm_epoch_register(api, warm->engine);
		epoch_armed = true;
	} else {
		api->context_set_epoch_deadline(ctx, UINT64_MAX);
	}

	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)WASM_OBJ_OFF;
	args[1].kind = WASMTIME_I32;
	args[1].of.i32 = (int32_t)obj->obj_len;
	err = nkvx_wasm_func_call(api, ctx, &fn_ext.of.func, args, 2, results, 1, &trap);
	/*
	 * spdk-5pq: clear the benign wasmtime longjmp shadow on the named locals this
	 * frame reads on the way out (err and the frame pointer were already cleared
	 * inside nkvx_wasm_func_call). Per-object, never a frame window. No-op outside
	 * the unit-test build (see header).
	 */
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(api);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(trap);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(results);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(mem_base);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(mem_size);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(status);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(store);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(epoch_armed);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(warm);
	NKVX_VALGRIND_SCRUB_FRAME_TOP();	/* canary + saved-reg band (see header) */

	/* Disarm the ticker as soon as the call returns; the engine is reused (not
	 * deleted), so we only need to stop it pointing at this engine. */
	if (epoch_armed) {
		nkvx_wasm_epoch_unregister(warm->engine);
		epoch_armed = false;
	}

	if (err != NULL) {
		nkvx_wasm_log_error(api, "func_call", err);
		goto out;
	}
	if (trap != NULL) {
		bool cap = nkvx_wasm_trap_is_cap(api, trap);
		/* spdk-5pq: cap is derived from the wasmtime trap object, which carries
		 * the benign longjmp shadow; define it so the status/log selection below
		 * is not falsely flagged. No-op outside the UT. */
		NKVX_VALGRIND_MAKE_OBJ_DEFINED(cap);

		api->trap_delete(trap);
		trap = NULL;
		/* Log parity with the plain run() path so a cap kill on the cached path
		 * is equally observable (a contained abort, never a crash). */
		SPDK_ERRLOG("nkvx/wasm: module '%s' trapped during execution (%s)\n",
			    name, cap ? "RESOURCE CAP — aborted" : "fault");
		status = cap ? SPDK_KVDEV_IO_STATUS_ABORTED : SPDK_KVDEV_IO_STATUS_FAILED;
		goto out;
	}
	if (results[0].kind != WASMTIME_I32) {
		goto out;
	}

	{
		uint32_t produced = (uint32_t)results[0].of.i32;
		uint32_t copy;

		if ((size_t)WASM_RES_OFF + produced > mem_size) {
			goto out;
		}
		*result_len = produced;
		copy = (uint32_t)spdk_min(produced, out_len);
		if (copy > 0 && out != NULL) {
			memcpy(out, mem_base + WASM_RES_OFF, copy);
		}
		status = (produced > out_len) ?
			 SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL :
			 SPDK_KVDEV_IO_STATUS_SUCCESS;
	}

out:
	/* Disarm the ticker before deleting the store/instance if a goto skipped the
	 * normal disarm (e.g. context_set_fuel failed after arming — cannot happen as
	 * arming is after, but keep it defensive against future reorders). */
	if (epoch_armed) {
		nkvx_wasm_epoch_unregister(warm->engine);
	}
	/* Tear down the per-Exec store+instance so the NEXT Exec starts clean (resets
	 * globals + data segments). The warm engine + compiled module survive. */
	if (store != NULL) {
		api->store_delete(store);
	}
	return status;
}

int
kvdev_rados_nkvx_wasm_run_cached(const char *name,
				 const struct kvdev_rados_nkvx_module *mod,
				 const char *obj_key, size_t object_len,
				 kvdev_rados_nkvx_fill_fn fill, void *fill_arg,
				 void *out, uint32_t out_len, uint32_t *result_len)
{
	const struct nkvx_wasm_api *api = nkvx_wasm_api();
	struct nkvx_obj_entry *obj;
	int status;

	*result_len = 0;

	if (api == NULL) {
		SPDK_WARNLOG("nkvx/wasm: runtime unavailable for cached module '%s'\n", name);
		return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
	}
	if (obj_key == NULL || obj_key[0] == '\0' || fill == NULL) {
		return SPDK_KVDEV_IO_STATUS_INVALID;
	}

	pthread_mutex_lock(&g_cache.mutex);
	nkvx_cache_init_once();

	/* ---- content-addressed object cache (cold-fill-once) ---------------- */
	obj = nkvx_obj_lookup(obj_key);
	if (obj != NULL && obj->filled) {
		/* HIT: served locally, NO librados refetch (fill not called). LRU touch
		 * so a frequently-used object is not evicted as if it were cold. */
		g_nkvx_wasm_stats.content_hits++;
		nkvx_obj_touch(obj);
	} else {
		/* MISS: allocate the page-rounded backing and cold-fill ONCE. The
		 * object bytes live at WASM_OBJ_OFF so the same .wasm ABI applies. */
		size_t need = nkvx_round_up((size_t)WASM_OBJ_OFF + object_len, NKVX_WASM_PAGE);
		size_t got = 0;

		if (need == 0) {
			need = NKVX_WASM_PAGE;
		}
		obj = calloc(1, sizeof(*obj));
		if (obj == NULL) {
			pthread_mutex_unlock(&g_cache.mutex);
			return SPDK_KVDEV_IO_STATUS_NOMEM;
		}
		/* Page-aligned backing so it is a valid wasm linear-memory base. */
		if (posix_memalign((void **)&obj->mem, NKVX_WASM_PAGE, need) != 0) {
			free(obj);
			pthread_mutex_unlock(&g_cache.mutex);
			return SPDK_KVDEV_IO_STATUS_NOMEM;
		}
		memset(obj->mem, 0, need);
		obj->mem_cap = need;
		obj->mem_size = need;
		snprintf(obj->obj_key, sizeof(obj->obj_key), "%s", obj_key);

		/* COLD FILL: the ONLY librados touch, into the cache buffer at the
		 * object offset. Counted so the test can assert exactly one. */
		if (fill(obj->mem + WASM_OBJ_OFF, need - WASM_OBJ_OFF, &got, fill_arg) != 0) {
			free(obj->mem);
			free(obj);
			pthread_mutex_unlock(&g_cache.mutex);
			SPDK_ERRLOG("nkvx/wasm: cold fill failed for object '%s'\n", obj_key);
			return SPDK_KVDEV_IO_STATUS_FAILED;
		}
		obj->obj_len = got;
		obj->filled = true;
		g_nkvx_wasm_stats.cold_fills++;
		nkvx_obj_list_add(obj);		/* MRU at tail + account bytes/count */
		/* Bound the object cache (spdk-wwy): evict LRU LIVE objects past the cap.
		 * The just-filled object is MRU (tail), so it is never the victim. Eviction
		 * tears down the victim's warm entries and defers any pinned free. Done
		 * before the run so this Exec's warm build observes the post-eviction set. */
		nkvx_obj_cache_enforce_caps(api);
	}

	status = nkvx_run_on_object_locked(api, name, mod, obj, out, out_len, result_len);

	pthread_mutex_unlock(&g_cache.mutex);
	return status;
}

/*
 * Warm the object cache for obj_key with NO module run (slice spdk-7sr.4 / S3). The
 * base-layer Retrieve verb shares this cache with Exec: a Retrieve miss cold-fills
 * here so a later Retrieve/Exec of the same oid hits. The allocate -> cold-fill ->
 * list-add path mirrors the MISS arm of run_cached exactly (same WASM_OBJ_OFF layout,
 * page rounding, cold-fill-once accounting, and cap enforcement), minus the run.
 */
int
kvdev_rados_nkvx_wasm_cache_fill(const char *obj_key, size_t object_len,
				 kvdev_rados_nkvx_fill_fn fill, void *fill_arg)
{
	const struct nkvx_wasm_api *api = nkvx_wasm_api();
	struct nkvx_obj_entry *obj;
	size_t need;
	size_t got = 0;

	if (api == NULL) {
		/* No runtime => no object cache backing; the caller falls back to a
		 * direct read (the cache is a wasm-runtime artifact). */
		return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
	}
	if (obj_key == NULL || obj_key[0] == '\0' || fill == NULL) {
		return SPDK_KVDEV_IO_STATUS_INVALID;
	}

	pthread_mutex_lock(&g_cache.mutex);
	nkvx_cache_init_once();

	obj = nkvx_obj_lookup(obj_key);
	if (obj != NULL && obj->filled) {
		/* HIT: already warm (perhaps filled by a prior Retrieve or Exec). LRU
		 * touch and leave it; fill is not called. */
		g_nkvx_wasm_stats.content_hits++;
		nkvx_obj_touch(obj);
		pthread_mutex_unlock(&g_cache.mutex);
		return SPDK_KVDEV_IO_STATUS_SUCCESS;
	}

	/* MISS: allocate the page-rounded backing and cold-fill ONCE, exactly as the
	 * run_cached miss arm does (object bytes at WASM_OBJ_OFF). */
	need = nkvx_round_up((size_t)WASM_OBJ_OFF + object_len, NKVX_WASM_PAGE);
	if (need == 0) {
		need = NKVX_WASM_PAGE;
	}
	obj = calloc(1, sizeof(*obj));
	if (obj == NULL) {
		pthread_mutex_unlock(&g_cache.mutex);
		return SPDK_KVDEV_IO_STATUS_NOMEM;
	}
	if (posix_memalign((void **)&obj->mem, NKVX_WASM_PAGE, need) != 0) {
		free(obj);
		pthread_mutex_unlock(&g_cache.mutex);
		return SPDK_KVDEV_IO_STATUS_NOMEM;
	}
	memset(obj->mem, 0, need);
	obj->mem_cap = need;
	obj->mem_size = need;
	snprintf(obj->obj_key, sizeof(obj->obj_key), "%s", obj_key);

	if (fill(obj->mem + WASM_OBJ_OFF, need - WASM_OBJ_OFF, &got, fill_arg) != 0) {
		free(obj->mem);
		free(obj);
		pthread_mutex_unlock(&g_cache.mutex);
		SPDK_ERRLOG("nkvx/wasm: cold fill failed for object '%s'\n", obj_key);
		return SPDK_KVDEV_IO_STATUS_FAILED;
	}
	obj->obj_len = got;
	obj->filled = true;
	g_nkvx_wasm_stats.cold_fills++;
	nkvx_obj_list_add(obj);
	nkvx_obj_cache_enforce_caps(api);

	pthread_mutex_unlock(&g_cache.mutex);
	return SPDK_KVDEV_IO_STATUS_SUCCESS;
}

/*
 * Run module `name` against an object the caller already PINNED via
 * kvdev_rados_nkvx_wasm_cache_pin (spdk-ii0 D2 probe-skip path). Operates on the
 * exact pinned version — no key lookup, no cold fill — so an invalidation that
 * unlinked the entry between probe and now cannot turn this into a stale/empty
 * read. The caller still owns the pin and must unpin afterwards. A pinned hit is
 * a content hit (no librados refetch happened on this Exec).
 */
int
kvdev_rados_nkvx_wasm_run_pinned(const char *name,
				 const struct kvdev_rados_nkvx_module *mod, void *pin,
				 void *out, uint32_t out_len, uint32_t *result_len)
{
	const struct nkvx_wasm_api *api = nkvx_wasm_api();
	struct nkvx_obj_entry *obj = pin;
	int status;

	*result_len = 0;

	if (api == NULL) {
		SPDK_WARNLOG("nkvx/wasm: runtime unavailable for pinned module '%s'\n", name);
		return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
	}
	if (obj == NULL) {
		return SPDK_KVDEV_IO_STATUS_INVALID;
	}

	pthread_mutex_lock(&g_cache.mutex);
	nkvx_cache_init_once();
	g_nkvx_wasm_stats.content_hits++;
	status = nkvx_run_on_object_locked(api, name, mod, obj, out, out_len, result_len);
	pthread_mutex_unlock(&g_cache.mutex);
	return status;
}

void
kvdev_rados_nkvx_wasm_cache_reset(void)
{
	const struct nkvx_wasm_api *api = nkvx_wasm_api();

	pthread_mutex_lock(&g_cache.mutex);
	nkvx_cache_init_once();

	{
		struct nkvx_warm_entry *w;

		/* Tear down every warm instance and drop the pin it held on its object
		 * (a dead object that hits refcount 0 here is removed + freed by the
		 * unref). nkvx_warm_destroy keeps warm_count consistent. */
		while ((w = STAILQ_FIRST(&g_cache.warm)) != NULL) {
			nkvx_warm_destroy(api, w);
		}
	}
	{
		struct nkvx_obj_entry *o;

		/* Free whatever objects remain on the list (live, or dead entries whose
		 * pins all dropped above). A dead entry still pinned by an in-flight Exec
		 * cannot exist at a clean reset point. nkvx_obj_list_remove keeps the
		 * count/bytes accounting consistent. */
		while ((o = STAILQ_FIRST(&g_cache.objects)) != NULL) {
			nkvx_obj_list_remove(o);
			nkvx_obj_free(o);
		}
	}
	memset(&g_nkvx_wasm_stats, 0, sizeof(g_nkvx_wasm_stats));
	pthread_mutex_unlock(&g_cache.mutex);
}

uint64_t
kvdev_rados_nkvx_wasm_obj_cache_count(void)
{
	uint64_t live = 0;
	struct nkvx_obj_entry *o;

	pthread_mutex_lock(&g_cache.mutex);
	nkvx_cache_init_once();
	/* Report LIVE entries (dead-pinned entries are transient and invisible to
	 * lookups), matching what the cap counts. */
	STAILQ_FOREACH(o, &g_cache.objects, link) {
		if (!o->dead) {
			live++;
		}
	}
	pthread_mutex_unlock(&g_cache.mutex);
	return live;
}

uint64_t
kvdev_rados_nkvx_wasm_obj_cache_bytes(void)
{
	uint64_t bytes;

	pthread_mutex_lock(&g_cache.mutex);
	nkvx_cache_init_once();
	bytes = g_cache.obj_bytes;
	pthread_mutex_unlock(&g_cache.mutex);
	return bytes;
}

uint64_t
kvdev_rados_nkvx_wasm_warm_cache_count(void)
{
	uint64_t n;

	pthread_mutex_lock(&g_cache.mutex);
	nkvx_cache_init_once();
	n = g_cache.warm_count;
	pthread_mutex_unlock(&g_cache.mutex);
	return n;
}

bool
kvdev_rados_nkvx_wasm_cache_has(const char *obj_key)
{
	struct nkvx_obj_entry *obj;
	bool present;

	if (obj_key == NULL || obj_key[0] == '\0') {
		return false;
	}
	pthread_mutex_lock(&g_cache.mutex);
	nkvx_cache_init_once();
	obj = nkvx_obj_lookup(obj_key);
	present = (obj != NULL && obj->filled);
	pthread_mutex_unlock(&g_cache.mutex);
	return present;
}

/*
 * Probe-and-PIN (spdk-ii0 D2 race fix, carry-ref). If obj_key is present and
 * filled, take a reference on its buffer and return an opaque handle; otherwise
 * return NULL (a miss — the caller must cold-fill via the read path). The pin
 * guarantees that a Store/Delete invalidation between this probe and the eventual
 * worker run cannot free the buffer (it is unlinked from new lookups and marked
 * dead, but the bytes survive until the matching unpin). The in-flight Exec then
 * serves exactly the version that existed at probe time — a correct linearization
 * (ordered before the concurrent Store) — while subsequent Execs miss the dead
 * entry and cold-fill fresh, so no stale value is ever served to a later Exec.
 *
 * EVICTION SAFETY (spdk-wwy): the same pin protects against LRU EVICTION. An
 * evictor that selects this entry as the LRU victim marks it dead and defers the
 * free to the last unpin (nkvx_obj_evict_locked, identical carry-ref to
 * invalidation), so a probe-hit that pinned here is never freed under the
 * dispatch->run_pinned path — no use-after-free on the probe-then-evicted race.
 */
void *
kvdev_rados_nkvx_wasm_cache_pin(const char *obj_key)
{
	struct nkvx_obj_entry *obj;

	if (obj_key == NULL || obj_key[0] == '\0') {
		return NULL;
	}
	pthread_mutex_lock(&g_cache.mutex);
	nkvx_cache_init_once();
	obj = nkvx_obj_lookup(obj_key);
	if (obj != NULL && obj->filled) {
		obj->refcount++;
	} else {
		obj = NULL;
	}
	pthread_mutex_unlock(&g_cache.mutex);
	return obj;
}

/* Drop a pin taken by kvdev_rados_nkvx_wasm_cache_pin (frees a dead entry once
 * its last pin is gone). Safe with a NULL handle. */
void
kvdev_rados_nkvx_wasm_cache_unpin(void *handle)
{
	struct nkvx_obj_entry *obj = handle;

	if (obj == NULL) {
		return;
	}
	pthread_mutex_lock(&g_cache.mutex);
	nkvx_obj_unref(obj);
	pthread_mutex_unlock(&g_cache.mutex);
}

/*
 * spdk-k3z: expose the OBJECT CONTENT bytes behind a pin (the handle from
 * kvdev_rados_nkvx_wasm_cache_pin) so the reactor-side result-cache probe can key
 * on the SAME content the worker would run on, without a librados refetch. The
 * caller holds the pin (so the buffer is alive), reads the bytes synchronously,
 * and must not retain the returned pointer past the unpin. Returns the object
 * bytes pointer + true length; *bytes may be NULL for a zero-length object.
 */
void
kvdev_rados_nkvx_wasm_pin_object(void *handle, const void **bytes, size_t *len)
{
	struct nkvx_obj_entry *obj = handle;

	if (bytes != NULL) {
		*bytes = NULL;
	}
	if (len != NULL) {
		*len = 0;
	}
	if (obj == NULL) {
		return;
	}
	pthread_mutex_lock(&g_cache.mutex);
	if (obj->mem != NULL && obj->filled) {
		if (bytes != NULL) {
			*bytes = obj->mem + WASM_OBJ_OFF;
		}
		if (len != NULL) {
			*len = obj->obj_len;
		}
	}
	pthread_mutex_unlock(&g_cache.mutex);
}

/*
 * Invalidate every cache entry (object + warm instances) for obj_key. Called from
 * EVERY value-mutating datapath op (Store, Delete) so a subsequent Exec cannot
 * serve stale cached bytes. Identity(oid)-addressed with invalidation-on-write —
 * NOT content-hashed.
 */
void
kvdev_rados_nkvx_wasm_cache_invalidate(const char *obj_key)
{
	const struct nkvx_wasm_api *api = nkvx_wasm_api();

	if (obj_key == NULL || obj_key[0] == '\0') {
		return;
	}
	pthread_mutex_lock(&g_cache.mutex);
	nkvx_cache_init_once();
	nkvx_cache_invalidate_locked(api, obj_key);
	pthread_mutex_unlock(&g_cache.mutex);
}

int
kvdev_rados_nkvx_wasm_run(const char *name,
			  const struct kvdev_rados_nkvx_module *mod,
			  const void *object, size_t object_len,
			  void *out, uint32_t out_len, uint32_t *result_len)
{
	const struct nkvx_wasm_api *api = nkvx_wasm_api();
	char path[1024];
	uint8_t *wasm = NULL;
	size_t wasm_len = 0;
	int status = SPDK_KVDEV_IO_STATUS_FAILED;

	wasm_config_t *config = NULL;
	wasm_engine_t *engine = NULL;
	bool epoch_armed = false;
	wasmtime_store_t *store = NULL;
	wasmtime_context_t *ctx = NULL;
	wasmtime_module_t *module = NULL;
	wasmtime_error_t *err = NULL;
	wasm_trap_t *trap = NULL;
	wasmtime_instance_t instance;
	wasmtime_extern_t mem_ext, fn_ext;
	uint8_t *mem_base;
	size_t mem_size;
	wasmtime_val_t args[2], results[1];
	struct nkvx_wasm_caps caps;

	*result_len = 0;

	/* Fail-soft when the runtime is unavailable. */
	if (api == NULL) {
		SPDK_WARNLOG("nkvx/wasm: runtime unavailable for module '%s'\n", name);
		return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
	}

	/*
	 * This plain-copy path (no object cache) is legacy/test-only: the live TB3+
	 * datapath always carries an object id and runs through run_cached/run_pinned
	 * (zero-copy + the verified module cache). A verified \c mod is therefore never
	 * routed here; reject it rather than re-derive a separate engine-config that
	 * matches the modcache engine for deserialize. The legacy path reads an
	 * unverified file by name (in-tree tests only).
	 */
	if (mod != NULL) {
		SPDK_ERRLOG("nkvx/wasm: verified module not supported on the plain-copy path; "
			    "use the cached/pinned path\n");
		return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
	}
	if (nkvx_wasm_module_path(name, path, sizeof(path)) != 0) {
		return SPDK_KVDEV_IO_STATUS_FAILED;
	}
	wasm = nkvx_wasm_read_file(path, &wasm_len);
	if (wasm == NULL) {
		return SPDK_KVDEV_IO_STATUS_FAILED;
	}

	/* Per-invocation caps (legacy defaults; env-overridable). */
	nkvx_wasm_caps_load(&caps, 0);
	SPDK_NOTICELOG("nkvx/wasm: module '%s' caps fuel=%" PRIu64 " epoch_ticks=%" PRIu64
		       " max_mem=%" PRIu64 "B\n", name,
		       caps.fuel_ceiling, caps.epoch_deadline_ticks, caps.max_memory_bytes);

	/*
	 * On-demand instance strategy (ADR-0013): a fresh engine/store/module/
	 * instance per run, NO warm reuse/pooling. TB2 adds per-invocation caps:
	 * the engine carries fuel + epoch-interruption feature toggles (set on the
	 * config BEFORE the engine is created); the store carries the fuel ceiling,
	 * epoch deadline, and linear-memory limiter.
	 */
	config = api->config_new();
	if (config == NULL) {
		SPDK_ERRLOG("nkvx/wasm: wasm_config_new failed\n");
		goto out;
	}
	/* Enable the features the caps need. Fuel and epoch are independent guards
	 * (fuel = deterministic compute budget; epoch = wall-clock backstop). */
	api->config_consume_fuel_set(config, caps.fuel_ceiling > 0);
	api->config_epoch_interruption_set(config, caps.epoch_deadline_ticks > 0);

	/* engine_new_with_config CONSUMES the config. */
	engine = api->engine_new_with_config(config);
	config = NULL;
	if (engine == NULL) {
		SPDK_ERRLOG("nkvx/wasm: wasm_engine_new_with_config failed\n");
		goto out;
	}

	/* Arm the wall-clock ticker for this engine BEFORE the module runs, so an
	 * epoch deadline can actually fire. Disarmed before engine teardown. */
	if (caps.epoch_deadline_ticks > 0) {
		nkvx_wasm_epoch_register(api, engine);
		epoch_armed = true;
	}

	store = api->store_new(engine, NULL, NULL);
	if (store == NULL) {
		SPDK_ERRLOG("nkvx/wasm: wasmtime_store_new failed\n");
		goto out;
	}
	ctx = api->store_context(store);

	/* MEMORY CAP: bound linear memory so a memory.grow past the cap fails and
	 * the module is contained, never OOMing the target. Other limits left at
	 * wasmtime defaults (negative => keep default). */
	if (caps.max_memory_bytes > 0) {
		api->store_limiter(store, (int64_t)caps.max_memory_bytes,
				   -1, -1, -1, -1);
	}

	err = api->module_new(engine, wasm, wasm_len, &module);
	if (err != NULL) {
		nkvx_wasm_log_error(api, "module_new", err);
		goto out;
	}

	/* No imports: imports=NULL, nimports=0 (no WASI). */
	err = api->instance_new(ctx, module, NULL, 0, &instance, &trap);
	if (err != NULL) {
		nkvx_wasm_log_error(api, "instance_new", err);
		goto out;
	}
	if (trap != NULL) {
		SPDK_ERRLOG("nkvx/wasm: instantiation trapped\n");
		api->trap_delete(trap);
		trap = NULL;
		goto out;
	}

	/* The module must export a linear memory named "memory" and a function
	 * named <name>. */
	if (!api->instance_export_get(ctx, &instance, "memory", strlen("memory"), &mem_ext) ||
	    mem_ext.kind != WASMTIME_EXTERN_MEMORY) {
		SPDK_ERRLOG("nkvx/wasm: module '%s' has no exported 'memory'\n", name);
		goto out;
	}
	if (!api->instance_export_get(ctx, &instance, name, strlen(name), &fn_ext) ||
	    fn_ext.kind != WASMTIME_EXTERN_FUNC) {
		SPDK_ERRLOG("nkvx/wasm: module '%s' has no exported function '%s'\n", name, name);
		goto out;
	}

	mem_base = api->memory_data(ctx, &mem_ext.of.memory);
	mem_size = api->memory_data_size(ctx, &mem_ext.of.memory);

	/* PLAIN COPY of the object bytes into wasm linear memory (this slice's
	 * memory strategy — not zero-copy, that is TB4). */
	if ((size_t)WASM_OBJ_OFF + object_len > mem_size) {
		SPDK_ERRLOG("nkvx/wasm: object (%zu B) does not fit linear memory (%zu B) "
			    "at off %u\n", object_len, mem_size, WASM_OBJ_OFF);
		goto out;
	}
	if (object_len > 0 && object != NULL) {
		memcpy(mem_base + WASM_OBJ_OFF, object, object_len);
	}

	/*
	 * Apply the per-invocation FUEL + EPOCH caps to the store context, right
	 * before the call (fuel must be (re)set per invocation; the store starts
	 * with 0 fuel => an immediate trap if we forgot). A fuel-runaway exhausts
	 * its budget and traps; an epoch-runaway (which fuel may under-count) trips
	 * the wall-clock deadline the ticker thread advances.
	 */
	if (caps.fuel_ceiling > 0) {
		err = api->context_set_fuel(ctx, caps.fuel_ceiling);
		if (err != NULL) {
			nkvx_wasm_log_error(api, "context_set_fuel", err);
			goto out;
		}
	}
	if (caps.epoch_deadline_ticks > 0) {
		api->context_set_epoch_deadline(ctx, caps.epoch_deadline_ticks);
	}

	/* Call <name>(obj_off, obj_len) -> result_len. */
	args[0].kind = WASMTIME_I32;
	args[0].of.i32 = (int32_t)WASM_OBJ_OFF;
	args[1].kind = WASMTIME_I32;
	args[1].of.i32 = (int32_t)object_len;
	err = nkvx_wasm_func_call(api, ctx, &fn_ext.of.func, args, 2, results, 1, &trap);
	/*
	 * spdk-5pq: the wasmtime setjmp/longjmp return leaves the benign "uninitialised"
	 * Memcheck shadow on the specific locals this frame reads on the way out. err
	 * and our frame pointer were already cleared inside nkvx_wasm_func_call; clear
	 * the remaining named outputs/handles here -- each holds a deterministic value
	 * by this point, so we drop only the shadow, never a byte. Naming each object
	 * (NOT a frame-window heuristic) keeps a genuine uninitialised read of any other
	 * local detectable. No-op outside the unit-test build (see header).
	 */
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(api);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(trap);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(results);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(mem_base);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(mem_size);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(status);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(module);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(store);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(epoch_armed);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(engine);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(config);
	NKVX_VALGRIND_MAKE_OBJ_DEFINED(wasm);
	NKVX_VALGRIND_SCRUB_FRAME_TOP();	/* canary + saved-reg band (see header) */
	if (err != NULL) {
		nkvx_wasm_log_error(api, "func_call", err);
		goto out;
	}
	if (trap != NULL) {
		/* A trap here is either a per-invocation cap kill (fuel/epoch) or an
		 * ordinary module fault. Classify it so a cap kill surfaces the
		 * distinct ABORTED ("resource exhausted") status; either way the
		 * worker returns cleanly and the SPDK-thread completion still fires. */
		bool cap = nkvx_wasm_trap_is_cap(api, trap);
		/* spdk-5pq: cap is derived from the wasmtime trap object, which carries
		 * the benign longjmp shadow; define it so the status/log selection below
		 * is not falsely flagged. No-op outside the UT. */
		NKVX_VALGRIND_MAKE_OBJ_DEFINED(cap);

		SPDK_ERRLOG("nkvx/wasm: module '%s' trapped during execution (%s)\n",
			    name, cap ? "RESOURCE CAP — aborted" : "fault");
		api->trap_delete(trap);
		trap = NULL;
		status = cap ? SPDK_KVDEV_IO_STATUS_ABORTED : SPDK_KVDEV_IO_STATUS_FAILED;
		goto out;
	}
	if (results[0].kind != WASMTIME_I32) {
		SPDK_ERRLOG("nkvx/wasm: module '%s' returned unexpected kind %d\n",
			    name, results[0].kind);
		goto out;
	}

	{
		uint32_t produced = (uint32_t)results[0].of.i32;

		/* Bound the produced length to what the module could have written. */
		if ((size_t)WASM_RES_OFF + produced > mem_size) {
			SPDK_ERRLOG("nkvx/wasm: module '%s' result (%u B at off %u) exceeds "
				    "linear memory\n", name, produced, WASM_RES_OFF);
			goto out;
		}

		*result_len = produced;

		/* Read the result out of linear memory into the host buffer. */
		uint32_t copy = (uint32_t)spdk_min(produced, out_len);
		if (copy > 0 && out != NULL) {
			memcpy(out, mem_base + WASM_RES_OFF, copy);
		}
		status = (produced > out_len) ?
			 SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL :
			 SPDK_KVDEV_IO_STATUS_SUCCESS;
	}

out:
	if (module != NULL) {
		api->module_delete(module);
	}
	if (store != NULL) {
		api->store_delete(store);
	}
	/* Disarm the ticker BEFORE deleting the engine it points at (avoids the
	 * ticker thread touching a freed engine). */
	if (epoch_armed) {
		nkvx_wasm_epoch_unregister(engine);
	}
	if (engine != NULL) {
		api->engine_delete(engine);
	}
	/* config is always NULL here: engine_new_with_config consumes it (even on
	 * failure) and we NULL it immediately, and no goto jumps in between. */
	(void)config;
	free(wasm);
	/* spdk-5pq: final clear of this frame's compiler-owned canary band, immediately
	 * before the stack-protector epilogue reads it (see header). No-op outside UT. */
	NKVX_VALGRIND_SCRUB_FRAME_TOP();
	return status;
}

#else /* !SPDK_CONFIG_WASM */

int
kvdev_rados_nkvx_wasm_run(const char *name,
			  const struct kvdev_rados_nkvx_module *mod,
			  const void *object, size_t object_len,
			  void *out, uint32_t out_len, uint32_t *result_len)
{
	(void)mod;
	(void)object;
	(void)object_len;
	(void)out;
	(void)out_len;

	/* Built --without-wasm: the wasm Exec path is compiled out. Fail-soft with a
	 * distinct "runtime unavailable" status; never a crash. */
	*result_len = 0;
	SPDK_WARNLOG("nkvx/wasm: built without --with-wasm; module '%s' unavailable\n", name);
	return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
}

int
kvdev_rados_nkvx_wasm_run_cached(const char *name,
				 const struct kvdev_rados_nkvx_module *mod,
				 const char *obj_key, size_t object_len,
				 kvdev_rados_nkvx_fill_fn fill, void *fill_arg,
				 void *out, uint32_t out_len, uint32_t *result_len)
{
	(void)mod;
	(void)obj_key;
	(void)object_len;
	(void)fill;
	(void)fill_arg;
	(void)out;
	(void)out_len;

	/* --without-wasm: no cache/zero-copy path. Fail-soft, never a crash. The
	 * fill callback is intentionally NOT invoked (nothing to fill into). */
	*result_len = 0;
	SPDK_WARNLOG("nkvx/wasm: built without --with-wasm; cached module '%s' unavailable\n", name);
	return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
}

int
kvdev_rados_nkvx_wasm_cache_fill(const char *obj_key, size_t object_len,
				 kvdev_rados_nkvx_fill_fn fill, void *fill_arg)
{
	(void)obj_key;
	(void)object_len;
	(void)fill;
	(void)fill_arg;
	/* --without-wasm: no object cache. fill is NOT invoked; the caller falls back
	 * to a plain read. */
	return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
}

void
kvdev_rados_nkvx_wasm_cache_reset(void)
{
	/* No cache exists in the stub build. */
}

bool
kvdev_rados_nkvx_wasm_module_cached(const uint8_t sha256[SPDK_KV_EXEC_SHA256_LEN])
{
	(void)sha256;
	/* No module cache in the stub build: always a miss. */
	return false;
}

int
kvdev_rados_nkvx_wasm_module_verify(const uint8_t sha256[SPDK_KV_EXEC_SHA256_LEN],
				    const void *bytes, size_t bytes_len)
{
	(void)sha256;
	(void)bytes;
	(void)bytes_len;
	/* --without-wasm: no runtime to compile for; fail-soft, never crash. */
	return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
}

int
kvdev_rados_nkvx_wasm_module_insert(const uint8_t sha256[SPDK_KV_EXEC_SHA256_LEN],
				    const void *bytes, size_t bytes_len)
{
	(void)sha256;
	(void)bytes;
	(void)bytes_len;
	/* --without-wasm: cannot compile/run; fail-soft, never crash. */
	return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
}

void
kvdev_rados_nkvx_wasm_module_cache_reset(void)
{
	/* No module cache in the stub build. */
}

uint64_t
kvdev_rados_nkvx_wasm_module_cache_count(void)
{
	return 0;
}

bool
kvdev_rados_nkvx_wasm_cache_has(const char *obj_key)
{
	(void)obj_key;
	/* No cache in the stub build: always a miss (callers take the cold path). */
	return false;
}

void *
kvdev_rados_nkvx_wasm_cache_pin(const char *obj_key)
{
	(void)obj_key;
	/* No cache in the stub build: always a miss. */
	return NULL;
}

void
kvdev_rados_nkvx_wasm_cache_unpin(void *handle)
{
	(void)handle;
}

void
kvdev_rados_nkvx_wasm_pin_object(void *handle, const void **bytes, size_t *len)
{
	(void)handle;
	/* No cache in the stub build: cache_pin never hands out a handle, but keep
	 * the symbol defined (and zero the outputs) so the unconditional reactor-side
	 * call site links and is safe in a --without-wasm target. */
	if (bytes != NULL) {
		*bytes = NULL;
	}
	if (len != NULL) {
		*len = 0;
	}
}

int
kvdev_rados_nkvx_wasm_run_pinned(const char *name,
				 const struct kvdev_rados_nkvx_module *mod, void *pin,
				 void *out, uint32_t out_len, uint32_t *result_len)
{
	(void)mod;
	(void)pin;
	(void)out;
	(void)out_len;

	*result_len = 0;
	SPDK_WARNLOG("nkvx/wasm: built without --with-wasm; pinned module '%s' unavailable\n", name);
	return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
}

void
kvdev_rados_nkvx_wasm_cache_invalidate(const char *obj_key)
{
	(void)obj_key;
	/* No cache in the stub build. */
}

void
kvdev_rados_nkvx_wasm_runtime_teardown(void)
{
	/* No epoch ticker / runtime in the stub build. */
}

uint64_t
kvdev_rados_nkvx_wasm_obj_cache_count(void)
{
	return 0;
}

uint64_t
kvdev_rados_nkvx_wasm_obj_cache_bytes(void)
{
	return 0;
}

uint64_t
kvdev_rados_nkvx_wasm_warm_cache_count(void)
{
	return 0;
}

#endif /* SPDK_CONFIG_WASM */
