/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Deterministic unit test for the rados-nkvx off-reactor Exec executor (ADR-0009
 * / TB1). It exercises the executor in isolation — no librados, no OSD, no Ceph —
 * because the executor itself has no RADOS dependency: it consumes already-filled
 * object bytes and runs a built-in module off the SPDK reactor.
 *
 * It proves the three TB1 acceptance criteria deterministically:
 *   1. A dispatched Exec returns the correct trivial result computed from the
 *      object bytes (bytecount -> object length as LE u64; identity -> bytes back).
 *   2. The module body ran OFF the polled SPDK thread: the test captures the OS
 *      thread id of the SPDK thread driving completions and asserts the worker's
 *      run_tid (captured inside the module run) differs from it. This is a
 *      thread-id check, NOT a timing race.
 *   3. The dispatch is selected by the static built-in module name (the same name
 *      the "nkvx:<module>" op-ID binding resolves to in the live path).
 *
 * The executor's worker is a real pthread; it hands completions back to the SPDK
 * thread via spdk_thread_send_msg(), which ut_multithread.c's poll_threads()
 * harvests on the main OS thread. So the worker run_tid is genuinely a different
 * OS thread from the one delivering the completion.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"

#include "spdk/thread.h"

/* Pull the executor in as source: it depends only on spdk/thread + pthreads. */
#include "kvdev/rados/kvdev_rados_nkvx.c"
/* And the dlopen-backed wasm runtime, so the real-wasm path is exercised in the
 * same translation unit. With --without-wasm this is the NOT_SUPPORTED stub. */
#include "kvdev/rados/kvdev_rados_nkvx_wasm.c"
/* The datapath's oid sizing (KVDEV_RADOS_NKVX_OID_BUFSZ, the actual size of
 * struct kvdev_rados_io::nkvx_oid) and its key->oid encoder live here, so the
 * long-key regression below tests the SAME buffer + encoder the datapath uses.
 * This header is librados-free, so it is safe in the unit build. */
#include "kvdev/rados/kvdev_rados.h"

#include "common/lib/ut_multithread.c"

struct nkvx_result {
	bool		completed;
	int		kvstatus;
	uint32_t	out_len;
	pthread_t	complete_tid;	/* OS thread the completion fired on */
};

static void
nkvx_done(void *arg, int kvstatus, uint32_t out_len)
{
	struct nkvx_result *r = arg;

	r->kvstatus = kvstatus;
	r->out_len = out_len;
	r->complete_tid = pthread_self();
	r->completed = true;
}

/* Dispatch a job from the (single) SPDK thread and pump until it completes. */
static void
dispatch_and_wait(const char *module, const void *obj, size_t obj_len,
		  void *out, uint32_t out_cap, struct nkvx_result *r)
{
	int rc;

	memset(r, 0, sizeof(*r));

	set_thread(0);
	/* NULL obj_key + NULL pin: these dispatch-level tests use the plain-copy path;
	 * the TB4 cache/zero-copy path is exercised directly via _wasm_run_cached. */
	rc = kvdev_rados_nkvx_dispatch(module, NULL, NULL, NULL, obj, obj_len, out, out_cap,
				       nkvx_done, r);
	CU_ASSERT(rc == 0);
	set_thread(INVALID_THREAD);

	/* Pump the SPDK thread until the worker hands the completion back. A
	 * bounded spin avoids hanging the suite if the worker were broken. */
	for (int i = 0; i < 100000 && !r->completed; i++) {
		poll_threads();
		if (!r->completed) {
			usleep(100);
		}
	}
	CU_ASSERT(r->completed);
}

static void
test_nkvx_bytecount_offreactor(void)
{
	static const char obj[] = "the quick brown fox";
	const uint32_t obj_len = (uint32_t)sizeof(obj); /* includes NUL, matches host */
	uint8_t out[64];
	uint64_t got = 0;
	struct nkvx_result r;
	pthread_t main_tid = pthread_self();

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

	memset(out, 0, sizeof(out));
	dispatch_and_wait(KVDEV_RADOS_NKVX_MODULE_BYTECOUNT, obj, obj_len,
			  out, sizeof(out), &r);

	/* Criterion 1+3: correct trivial result from object bytes, via static name. */
	CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(r.out_len == sizeof(uint64_t));
	memcpy(&got, out, sizeof(got));
	CU_ASSERT(got == obj_len);

	/* Criterion 2: completion fired on the SPDK (main) OS thread, but the module
	 * body ran on the executor worker — a DIFFERENT OS thread. Deterministic. */
	printf("\n    off-reactor proof: spdk/reactor_tid=0x%lx worker_tid=0x%lx differ=%s\n",
	       (unsigned long)main_tid, (unsigned long)g_nkvx.tid,
	       pthread_equal(g_nkvx.tid, main_tid) ? "NO" : "YES");
	CU_ASSERT(pthread_equal(r.complete_tid, main_tid));
	CU_ASSERT(g_nkvx.running);
	CU_ASSERT(!pthread_equal(g_nkvx.tid, main_tid));

	kvdev_rados_nkvx_stop();
}

static void
test_nkvx_identity(void)
{
	static const char obj[] = "near-data-compute payload";
	const uint32_t obj_len = (uint32_t)sizeof(obj);
	uint8_t out[64];
	struct nkvx_result r;

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

	memset(out, 0, sizeof(out));
	dispatch_and_wait(KVDEV_RADOS_NKVX_MODULE_IDENTITY, obj, obj_len,
			  out, sizeof(out), &r);

	CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(r.out_len == obj_len);
	CU_ASSERT(memcmp(out, obj, obj_len) == 0);

	kvdev_rados_nkvx_stop();
}

static void
test_nkvx_unknown_module(void)
{
	static const char obj[] = "x";
	uint8_t out[8];
	struct nkvx_result r;

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

	/* No static binding for this op-ID -> NOT_SUPPORTED, no result bytes. */
	dispatch_and_wait("does-not-exist", obj, sizeof(obj), out, sizeof(out), &r);
	CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED);
	CU_ASSERT(r.out_len == 0);

	kvdev_rados_nkvx_stop();
}

static void
test_nkvx_bytecount_buffer_too_small(void)
{
	static const char obj[] = "0123456789";
	const uint32_t obj_len = (uint32_t)sizeof(obj);
	uint8_t out[4]; /* < sizeof(uint64_t): forces a truncated copy */
	struct nkvx_result r;

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

	memset(out, 0, sizeof(out));
	dispatch_and_wait(KVDEV_RADOS_NKVX_MODULE_BYTECOUNT, obj, obj_len,
			  out, sizeof(out), &r);

	/* True length still reported (8), but the small buffer is flagged. */
	CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL);
	CU_ASSERT(r.out_len == sizeof(uint64_t));

	kvdev_rados_nkvx_stop();
}

/*
 * Real-wasm path (TB-WIMP / ADR-0013). Dispatch "wasm:bytecount" off the reactor;
 * the dlopen-backed runtime instantiates the checked-in bytecount.wasm and
 * runs it against a PLAIN COPY of the object bytes in linear memory.
 *
 * Graceful-degradation aware: if SPDK was built --without-wasm, or libwasmtime.so
 * is not installed on this host, the runtime is unavailable and the dispatch must
 * fail SOFT with NOT_SUPPORTED (never crash). When the runtime IS available the
 * result must equal the object length (mirroring the bytecount built-in). Either
 * way the off-reactor dispatch/threading is exercised.
 */
static void
test_nkvx_wasm_bytecount_offreactor(void)
{
	static const char obj[] = "the quick brown fox";
	const uint32_t obj_len = (uint32_t)sizeof(obj);
	uint8_t out[64];
	struct nkvx_result r;

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
#endif

	memset(out, 0, sizeof(out));
	dispatch_and_wait(KVDEV_RADOS_NKVX_MODULE_WASM_PREFIX
			  KVDEV_RADOS_NKVX_MODULE_BYTECOUNT,
			  obj, obj_len, out, sizeof(out), &r);

	/* The dispatch itself always completes (no crash) on the SPDK thread. */
	CU_ASSERT(r.completed);

	if (r.kvstatus == SPDK_KVDEV_IO_STATUS_SUCCESS) {
		uint64_t got = 0;

		/* Runtime available: a REAL .wasm ran off-reactor and returned the
		 * object length as an LE u64 read back from linear memory. */
		CU_ASSERT(r.out_len == sizeof(uint64_t));
		memcpy(&got, out, sizeof(got));
		CU_ASSERT(got == obj_len);
		printf("\n    real-wasm bytecount ran off-reactor: got=%llu expected=%u\n",
		       (unsigned long long)got, obj_len);
	} else {
		/* Runtime unavailable (built --without-wasm or libwasmtime.so absent):
		 * fail-soft with a DISTINCT status, no result bytes, no crash. */
		CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED);
		printf("\n    wasm runtime unavailable -> fail-soft NOT_SUPPORTED (ok)\n");
	}

	kvdev_rados_nkvx_stop();
}

/*
 * TB2 per-invocation caps. These only run a REAL module when the wasm runtime is
 * available (built --with-wasm AND libwasmtime.so present AND the .wasm dir is
 * known); otherwise they assert the fail-soft NOT_SUPPORTED path, like the
 * bytecount wasm test. Caps are sourced per-invocation from env overrides
 * (SPDK_NKVX_WASM_FUEL / _EPOCH_TICKS / _MAX_MEMORY), exactly the TB2 source.
 */
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
#define NKVX_WASM_RUNTIME_TESTS 1
#endif

/* Fuel cap: a tight compute-runaway module is killed by fuel exhaustion and the
 * dispatch returns the distinct ABORTED status — never a crash/hang. */
static void
test_nkvx_wasm_fuel_runaway_aborted(void)
{
	struct nkvx_result r;
	uint8_t out[64];

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

#ifdef NKVX_WASM_RUNTIME_TESTS
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	/* Small fuel ceiling so the tight loop exhausts it fast; epoch off so this
	 * test isolates the FUEL path; default memory. */
	setenv("SPDK_NKVX_WASM_FUEL", "1000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);
	unsetenv("SPDK_NKVX_WASM_MAX_MEMORY");

	memset(out, 0, sizeof(out));
	dispatch_and_wait("wasm:fuel_runaway", "x", 1, out, sizeof(out), &r);
	CU_ASSERT(r.completed);
	if (r.kvstatus != SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED) {
		/* Runtime available: fuel must have killed it -> ABORTED, no crash. */
		CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_ABORTED);
		printf("\n    fuel-runaway contained: status=%d (ABORTED expected)\n", r.kvstatus);
	} else {
		printf("\n    wasm runtime unavailable -> fail-soft (fuel test skipped)\n");
	}
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	(void)r;
	(void)out;
	printf("\n    built --without-wasm: fuel cap test is a no-op\n");
#endif
	kvdev_rados_nkvx_stop();
}

/* Epoch cap: a wall-clock-runaway module with FUEL DISABLED is stopped only by
 * the epoch deadline the background ticker advances -> ABORTED, never a hang. */
static void
test_nkvx_wasm_walltime_runaway_epoch_aborted(void)
{
	struct nkvx_result r;
	uint8_t out[64];

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

#ifdef NKVX_WASM_RUNTIME_TESTS
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	/* Fuel OFF: ONLY epoch can stop it. Few ticks so the wall-clock budget is
	 * short (a few * 10ms). */
	setenv("SPDK_NKVX_WASM_FUEL", "0", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "3", 1);
	unsetenv("SPDK_NKVX_WASM_MAX_MEMORY");

	memset(out, 0, sizeof(out));
	dispatch_and_wait("wasm:walltime_runaway", "x", 1, out, sizeof(out), &r);
	CU_ASSERT(r.completed);
	if (r.kvstatus != SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED) {
		CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_ABORTED);
		printf("\n    walltime-runaway contained by EPOCH: status=%d (ABORTED expected)\n",
		       r.kvstatus);
	} else {
		printf("\n    wasm runtime unavailable -> fail-soft (epoch test skipped)\n");
	}
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	(void)r;
	(void)out;
	printf("\n    built --without-wasm: epoch cap test is a no-op\n");
#endif
	kvdev_rados_nkvx_stop();
}

/* Memory cap: an over-allocating module (memory.grow past the cap) is CONTAINED
 * by the store limiter -> the module's grow fails and it traps; the dispatch
 * returns a contained failure and the target survives (never an OOM). */
static void
test_nkvx_wasm_overalloc_contained(void)
{
	struct nkvx_result r;
	uint8_t out[64];

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

#ifdef NKVX_WASM_RUNTIME_TESTS
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	/* Tight memory cap (1 MiB) so memory.grow fails quickly. Fuel/epoch high
	 * enough not to interfere — the grow loop is short. */
	setenv("SPDK_NKVX_WASM_MAX_MEMORY", "1048576", 1);
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");

	memset(out, 0, sizeof(out));
	dispatch_and_wait("wasm:overalloc", "x", 1, out, sizeof(out), &r);
	CU_ASSERT(r.completed);
	if (r.kvstatus != SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED) {
		/* Contained: a clean failure status (ABORTED if classified, else
		 * FAILED), NOT a crash. Either is acceptable containment. */
		CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_ABORTED ||
			  r.kvstatus == SPDK_KVDEV_IO_STATUS_FAILED);
		printf("\n    over-alloc contained by MEMORY cap: status=%d (target survived)\n",
		       r.kvstatus);
	} else {
		printf("\n    wasm runtime unavailable -> fail-soft (memory test skipped)\n");
	}
	unsetenv("SPDK_NKVX_WASM_MAX_MEMORY");
#else
	(void)r;
	(void)out;
	printf("\n    built --without-wasm: memory cap test is a no-op\n");
#endif
	kvdev_rados_nkvx_stop();
}

/* Caps configurable: the normal bytecount module STILL completes within generous
 * caps (proves caps don't break the happy path and are read per-invocation). */
static void
test_nkvx_wasm_normal_within_caps(void)
{
	static const char obj[] = "the quick brown fox";
	const uint32_t obj_len = (uint32_t)sizeof(obj);
	uint8_t out[64];
	struct nkvx_result r;

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

#ifdef NKVX_WASM_RUNTIME_TESTS
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	/* Explicit, modest caps -> bytecount still succeeds. */
	setenv("SPDK_NKVX_WASM_FUEL", "10000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "100", 1);
	setenv("SPDK_NKVX_WASM_MAX_MEMORY", "8388608", 1);

	memset(out, 0, sizeof(out));
	dispatch_and_wait("wasm:bytecount", obj, obj_len, out, sizeof(out), &r);
	CU_ASSERT(r.completed);
	if (r.kvstatus != SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED) {
		uint64_t got = 0;

		CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_SUCCESS);
		CU_ASSERT(r.out_len == sizeof(uint64_t));
		memcpy(&got, out, sizeof(got));
		CU_ASSERT(got == obj_len);
		printf("\n    normal bytecount within caps: got=%llu (SUCCESS)\n",
		       (unsigned long long)got);
	}
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
	unsetenv("SPDK_NKVX_WASM_MAX_MEMORY");
#else
	(void)obj;
	(void)obj_len;
	(void)out;
	(void)r;
	printf("\n    built --without-wasm: caps happy-path test is a no-op\n");
#endif
	kvdev_rados_nkvx_stop();
}

/* ==========================================================================
 * TB4 (spdk-ii0): content-addressed object cache + zero-copy DMA + warm instance.
 *
 * These exercise the executor's cache layer DIRECTLY (kvdev_rados_nkvx_wasm_run_cached),
 * with a FAKE cold-fill callback that counts how many times it actually ran. The
 * four acceptance criteria map to explicit asserts below. The cache code is real;
 * the fill callback stands in for the librados cold fill (the executor never calls
 * librados itself — it only invokes this callback on a content-cache miss).
 *
 * Graceful-degradation aware, exactly like the other wasm tests: when the runtime
 * is unavailable (built --without-wasm OR libwasmtime.so absent), run_cached returns
 * NOT_SUPPORTED and the fill callback is never reached; the test asserts that
 * fail-soft path instead and skips the cache-specific assertions.
 * ========================================================================== */

struct fake_fill {
	const void	*bytes;
	size_t		len;
	uint64_t	calls;		/* number of cold fills actually performed */
};

static int
fake_cold_fill(void *buf, size_t cap, size_t *out_len, void *arg)
{
	struct fake_fill *f = arg;

	f->calls++;			/* a cold fill (the only "librados" touch) happened */
	if (f->len > cap) {
		return -1;
	}
	memcpy(buf, f->bytes, f->len);
	*out_len = f->len;
	return 0;
}

/* Expected checksum.wasm result for a given object: must match checksum.c. */
static uint64_t
expected_checksum(const uint8_t *obj, size_t len)
{
	uint64_t sum = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		sum += (uint64_t)obj[i] * (uint64_t)(i + 1u);
	}
	sum ^= (uint64_t)len << 32;
	return sum;
}

static bool
nkvx_wasm_runtime_available(void)
{
	/* A cheap probe: run_cached returns NOT_SUPPORTED iff the runtime is absent. */
	struct kvdev_rados_nkvx_wasm_stats before, after;
	struct fake_fill f = { .bytes = "x", .len = 1 };
	uint8_t out[16];
	uint32_t rlen = 0;
	int rc;

	kvdev_rados_nkvx_wasm_get_stats(&before);
	rc = kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, "__probe__", 1,
					      fake_cold_fill, &f, out, sizeof(out), &rlen);
	(void)before;
	(void)after;
	if (rc == SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED) {
		return false;
	}
	kvdev_rados_nkvx_wasm_cache_reset();
	return true;
}

/*
 * Acceptance criteria 1+2+3+4 in one flow:
 *   #4 cold-fill correct: 1st Exec returns the right checksum (depends on the
 *      object bytes actually being in linear memory).
 *   #1 no refetch:        2nd Exec of the SAME (module,object) does NOT call the
 *      fill callback again (cold_fills stays at 1; content_hits increments).
 *   #2 zero-copy:         the wasm linear-memory base pointer ALIASES the cache
 *      buffer (last_mem_base == last_cache_base) — no gather copy-in.
 *   #3 warm instance:     the 2nd Exec reuses the instantiated instance
 *      (warm_hits increments).
 */
static void
test_nkvx_tb4_cache_zerocopy_warm(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	/* Generous caps so checksum completes; per-invocation source is env (TB2). */
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> TB4 cache test skipped (fail-soft)\n");
		return;
	}

	kvdev_rados_nkvx_wasm_cache_reset();

	static const uint8_t obj[] = "graph-partition-bytes-AABBCCDD";
	const size_t obj_len = sizeof(obj);	/* includes NUL */
	struct fake_fill fill = { .bytes = obj, .len = obj_len, .calls = 0 };
	uint8_t out[64];
	uint32_t rlen = 0;
	uint64_t got = 0;
	int rc;
	struct kvdev_rados_nkvx_wasm_stats st;

	/* ---- 1st Exec: COLD FILL ---- */
	memset(out, 0, sizeof(out));
	rc = kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, "objK", obj_len,
					      fake_cold_fill, &fill,
					      out, sizeof(out), &rlen);
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(rlen == sizeof(uint64_t));
	memcpy(&got, out, sizeof(got));
	/* #4 cold-fill correct: checksum reflects the actual object bytes. */
	CU_ASSERT(got == expected_checksum(obj, obj_len));
	CU_ASSERT(fill.calls == 1);

	kvdev_rados_nkvx_wasm_get_stats(&st);
	/* #2 zero-copy: linear memory aliases the cache buffer. */
	CU_ASSERT(st.cold_fills == 1);
	CU_ASSERT(st.last_mem_base != NULL);
	CU_ASSERT(st.last_mem_base == st.last_cache_base);
	printf("\n    TB4 1st Exec: checksum=0x%llx cold_fills=%llu mem_base=%p cache_base=%p (alias=%s)\n",
	       (unsigned long long)got, (unsigned long long)st.cold_fills,
	       st.last_mem_base, st.last_cache_base,
	       st.last_mem_base == st.last_cache_base ? "YES" : "NO");

	/* ---- 2nd Exec of the SAME (module,object) ---- */
	memset(out, 0, sizeof(out));
	got = 0;
	rc = kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, "objK", obj_len,
					      fake_cold_fill, &fill,
					      out, sizeof(out), &rlen);
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_SUCCESS);
	memcpy(&got, out, sizeof(got));
	CU_ASSERT(got == expected_checksum(obj, obj_len));	/* still correct */

	/* #1 no librados refetch: the fill callback did NOT run again. */
	CU_ASSERT(fill.calls == 1);
	kvdev_rados_nkvx_wasm_get_stats(&st);
	CU_ASSERT(st.cold_fills == 1);
	CU_ASSERT(st.content_hits == 1);
	/* #3 warm instance reused. */
	CU_ASSERT(st.warm_hits == 1);
	/* #2 still zero-copy on the warm path. */
	CU_ASSERT(st.last_mem_base == st.last_cache_base);
	printf("    TB4 2nd Exec: cold_fills=%llu content_hits=%llu warm_hits=%llu (no refetch, warm reuse)\n",
	       (unsigned long long)st.cold_fills, (unsigned long long)st.content_hits,
	       (unsigned long long)st.warm_hits);

	kvdev_rados_nkvx_wasm_cache_reset();
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: TB4 cache/zero-copy test is a no-op\n");
#endif
}

/*
 * A DIFFERENT object key is a genuine miss: it cold-fills again (a second fill)
 * and gets its OWN cache buffer (distinct zero-copy backing), proving the cache
 * is content-addressed (per-object), not a single global slot.
 */
static void
test_nkvx_tb4_distinct_object_is_miss(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> TB4 distinct-object test skipped\n");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();

	static const uint8_t a[] = "object-AAAA";
	static const uint8_t b[] = "object-BBBBBBBB";
	struct fake_fill fa = { .bytes = a, .len = sizeof(a), .calls = 0 };
	struct fake_fill fb = { .bytes = b, .len = sizeof(b), .calls = 0 };
	uint8_t out[64];
	uint32_t rlen = 0;
	uint64_t ga = 0, gb = 0;
	const void *base_a;
	struct kvdev_rados_nkvx_wasm_stats st;

	memset(out, 0, sizeof(out));
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, "A", sizeof(a),
			fake_cold_fill, &fa, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	memcpy(&ga, out, sizeof(ga));
	kvdev_rados_nkvx_wasm_get_stats(&st);
	base_a = st.last_cache_base;

	memset(out, 0, sizeof(out));
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, "B", sizeof(b),
			fake_cold_fill, &fb, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	memcpy(&gb, out, sizeof(gb));
	kvdev_rados_nkvx_wasm_get_stats(&st);

	/* Both cold-filled exactly once -> two distinct cold fills, zero hits. */
	CU_ASSERT(fa.calls == 1);
	CU_ASSERT(fb.calls == 1);
	CU_ASSERT(st.cold_fills == 2);
	CU_ASSERT(st.content_hits == 0);
	/* Distinct content-addressed buffers (different zero-copy backings). */
	CU_ASSERT(st.last_cache_base != base_a);
	CU_ASSERT(st.last_cache_base == st.last_mem_base);	/* B still zero-copy */
	/* Correct, distinct results. */
	CU_ASSERT(ga == expected_checksum(a, sizeof(a)));
	CU_ASSERT(gb == expected_checksum(b, sizeof(b)));
	CU_ASSERT(ga != gb);
	printf("\n    TB4 distinct objects: cold_fills=%llu hits=%llu base_a=%p base_b=%p (distinct=%s)\n",
	       (unsigned long long)st.cold_fills, (unsigned long long)st.content_hits,
	       base_a, st.last_cache_base, base_a != st.last_cache_base ? "YES" : "NO");

	kvdev_rados_nkvx_wasm_cache_reset();
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: TB4 distinct-object test is a no-op\n");
#endif
}

/*
 * LONG-KEY oid TRUNCATION regression (spdk-vim) -- the core, config-independent
 * guard. Runs in EVERY build (including the pipeline's --without-wasm config), and
 * is wired to the SAME source of truth as the datapath: KVDEV_RADOS_NKVX_OID_BUFSZ
 * is the literal size of struct kvdev_rados_io::nkvx_oid, and kvdev_rados_key_to_oid
 * is the literal encoder, both from kvdev_rados.h.
 *
 * The bug: kvdev_rados_nkvx_exec() builds the full hex oid then does
 *     snprintf(io->nkvx_oid, sizeof(io->nkvx_oid), "%s", oid);   // kvdev_rados.c
 * When nkvx_oid was sized KVDEV_RADOS_OID_MAX (33), any key >16 bytes was TRUNCATED
 * to 32 hex chars in io->nkvx_oid -- the value every data-phase reader (cache pin,
 * cold-fill librados read, dispatch) then uses -- so it addressed a WRONG/short
 * object, and two distinct long keys sharing a 16-byte prefix collapsed to one oid.
 *
 * This test reproduces that exact snprintf into a buffer of the REAL field size and
 * asserts no truncation + distinctness. It is engineered to FAIL if nkvx_oid is ever
 * shrunk back to KVDEV_RADOS_OID_MAX and PASS at KVDEV_RADOS_EXEC_OID_MAX.
 */
static void
ut_exec_oid_like_datapath(const void *key, uint8_t key_len, char *io_nkvx_oid)
{
	/* Mirror kvdev_rados_nkvx_exec(): hex-encode into a full-size scratch, then
	 * the lossy snprintf copy into the (possibly-too-small) per-io field. */
	char oid[KVDEV_RADOS_EXEC_OID_MAX];

	kvdev_rados_key_to_oid(key, key_len, oid);
	snprintf(io_nkvx_oid, KVDEV_RADOS_NKVX_OID_BUFSZ, "%s", oid);
}

static void
test_nkvx_long_key_oid(void)
{
	/* A 255-byte key is the Exec ABI maximum (SPDK_KVDEV_EXEC_KEY_MAX_LEN). Its
	 * full oid is 510 hex chars; nkvx_oid MUST hold it (511 incl. NUL). */
	CU_ASSERT(KVDEV_RADOS_NKVX_OID_BUFSZ >= SPDK_KVDEV_EXEC_KEY_MAX_LEN * 2 + 1);

	/* Buffers sized exactly like the production io->nkvx_oid field. */
	char io_oid_a[KVDEV_RADOS_NKVX_OID_BUFSZ];
	char io_oid_b[KVDEV_RADOS_NKVX_OID_BUFSZ];

	/* Two 32-byte keys (>16 B) sharing a 16-byte prefix; they differ only in the
	 * second half -- only past hex char 32, exactly where a 33-byte oid buffer
	 * truncates and loses the distinction. */
	uint8_t key_a[32], key_b[32];
	memset(key_a, 0xA5, sizeof(key_a));
	memcpy(key_b, key_a, 16);		/* identical 16-byte prefix */
	memset(key_b + 16, 0x5A, 16);		/* diverge only past byte 16 */

	ut_exec_oid_like_datapath(key_a, sizeof(key_a), io_oid_a);
	ut_exec_oid_like_datapath(key_b, sizeof(key_b), io_oid_b);

	/* NOT truncated: a 32-byte key yields 64 hex chars in io->nkvx_oid. With the
	 * buggy 33-byte buffer this strlen would be 32 -> assertion fails. */
	CU_ASSERT(strlen(io_oid_a) == sizeof(key_a) * 2);
	CU_ASSERT(strlen(io_oid_b) == sizeof(key_b) * 2);

	/* Distinct in full, despite a shared 32-char prefix. With truncation both
	 * collapse to the same 32-char oid -> this inequality fails. */
	CU_ASSERT(strncmp(io_oid_a, io_oid_b, 32) == 0);	/* shared prefix */
	CU_ASSERT(strcmp(io_oid_a, io_oid_b) != 0);		/* but distinct */

	/* Round-trip identity: re-deriving key A's oid yields the identical string the
	 * data phase used the first time (store and retrieve address the same object). */
	char io_oid_a2[KVDEV_RADOS_NKVX_OID_BUFSZ];
	ut_exec_oid_like_datapath(key_a, sizeof(key_a), io_oid_a2);
	CU_ASSERT(strcmp(io_oid_a, io_oid_a2) == 0);

	/* The full 255-byte Exec maximum must survive end to end (no truncation). */
	uint8_t key_max[SPDK_KVDEV_EXEC_KEY_MAX_LEN];
	char io_oid_max[KVDEV_RADOS_NKVX_OID_BUFSZ];
	memset(key_max, 0xC3, sizeof(key_max));
	ut_exec_oid_like_datapath(key_max, sizeof(key_max), io_oid_max);
	CU_ASSERT(strlen(io_oid_max) == (size_t)SPDK_KVDEV_EXEC_KEY_MAX_LEN * 2);

	printf("\n    long-key oid: nkvx_oid buf=%zu, 32B key -> |oid|=%zu (not 32), "
	       "255B key -> |oid|=%zu, A==A round-trip, A!=B\n",
	       (size_t)KVDEV_RADOS_NKVX_OID_BUFSZ, strlen(io_oid_a), strlen(io_oid_max));
}

#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
/* Mirror of kvdev_rados_key_to_oid for a size_t key_len (the deep wasm test below
 * uses oversized keys); identical hex encoding. */
static void
ut_key_to_oid(const void *key, size_t key_len, char *oid)
{
	static const char hex[] = "0123456789abcdef";
	const uint8_t *k = key;
	size_t i;

	for (i = 0; i < key_len; i++) {
		oid[i * 2]     = hex[k[i] >> 4];
		oid[i * 2 + 1] = hex[k[i] & 0xf];
	}
	oid[key_len * 2] = '\0';
}
#endif

/*
 * LONG-KEY oid cache-distinctness (spdk-vim), wasm-deep variant: with a real wasm
 * runtime, drive the executor cache with two full-length long-key oids sharing a
 * 16-byte prefix and prove they back DISTINCT cached objects (no false content hit)
 * and that re-running the same long oid hits the same slot. Complements
 * test_nkvx_long_key_oid (which is the config-independent truncation guard).
 */
static void
test_nkvx_long_key_oid_distinct(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> long-key oid test skipped\n");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();

	/* Two 32-byte keys (>16 B) sharing a 16-byte prefix; they differ only in the
	 * second half, i.e. only past hex char 32 -- exactly where the old truncated
	 * oid buffer would have lost the distinction. */
	uint8_t key_a[32], key_b[32];
	char oid_a[KVDEV_RADOS_EXEC_OID_MAX];
	char oid_b[KVDEV_RADOS_EXEC_OID_MAX];

	memset(key_a, 0xA5, sizeof(key_a));
	memcpy(key_b, key_a, 16);		/* identical 16-byte prefix */
	memset(key_b + 16, 0x5A, 16);		/* diverge only past byte 16 */

	ut_key_to_oid(key_a, sizeof(key_a), oid_a);
	ut_key_to_oid(key_b, sizeof(key_b), oid_b);
	CU_ASSERT(strlen(oid_a) == sizeof(key_a) * 2);		/* 64 hex chars */
	CU_ASSERT(strncmp(oid_a, oid_b, 32) == 0);		/* same 32-char prefix */
	CU_ASSERT(strcmp(oid_a, oid_b) != 0);			/* but distinct in full */

	static const uint8_t obj_a[] = "object-bytes-for-long-key-A";
	static const uint8_t obj_b[] = "object-bytes-for-long-key-B-differs";
	struct fake_fill fa = { .bytes = obj_a, .len = sizeof(obj_a), .calls = 0 };
	struct fake_fill fb = { .bytes = obj_b, .len = sizeof(obj_b), .calls = 0 };
	uint8_t out[64];
	uint32_t rlen = 0;
	uint64_t ga = 0, gb = 0, ga2 = 0;
	const void *base_a;
	struct kvdev_rados_nkvx_wasm_stats st;

	/* Long key A -> cold fill, distinct cache slot. */
	memset(out, 0, sizeof(out));
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, oid_a, sizeof(obj_a),
			fake_cold_fill, &fa, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	memcpy(&ga, out, sizeof(ga));
	kvdev_rados_nkvx_wasm_get_stats(&st);
	base_a = st.last_cache_base;

	/* Long key B (shares A's 32-char oid prefix) -> MUST be a genuine MISS, not a
	 * collision with A's slot. A truncated oid would make this a false hit on A. */
	memset(out, 0, sizeof(out));
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, oid_b, sizeof(obj_b),
			fake_cold_fill, &fb, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	memcpy(&gb, out, sizeof(gb));
	kvdev_rados_nkvx_wasm_get_stats(&st);

	CU_ASSERT(fa.calls == 1);
	CU_ASSERT(fb.calls == 1);			/* B cold-filled on its own */
	CU_ASSERT(st.cold_fills == 2);
	CU_ASSERT(st.content_hits == 0);		/* no false hit on A */
	CU_ASSERT(st.last_cache_base != base_a);	/* distinct backing buffers */
	CU_ASSERT(ga == expected_checksum(obj_a, sizeof(obj_a)));
	CU_ASSERT(gb == expected_checksum(obj_b, sizeof(obj_b)));
	CU_ASSERT(ga != gb);				/* the correct, distinct objects */

	/* Re-run long key A -> same full oid must HIT A's slot (round-trip identity),
	 * with no extra cold fill. */
	memset(out, 0, sizeof(out));
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, oid_a, sizeof(obj_a),
			fake_cold_fill, &fa, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	memcpy(&ga2, out, sizeof(ga2));
	kvdev_rados_nkvx_wasm_get_stats(&st);
	CU_ASSERT(fa.calls == 1);			/* no refetch of A */
	CU_ASSERT(st.cold_fills == 2);
	CU_ASSERT(st.content_hits == 1);		/* A served from cache */
	CU_ASSERT(ga2 == ga);				/* same object as the first A run */

	printf("\n    long-key oid: |oid|=%zu shared-prefix=32 distinct -> cold_fills=%llu "
	       "content_hits=%llu (A!=B, A round-trips)\n",
	       strlen(oid_a), (unsigned long long)st.cold_fills,
	       (unsigned long long)st.content_hits);

	kvdev_rados_nkvx_wasm_cache_reset();
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: long-key oid test is a no-op\n");
#endif
}

/*
 * BACKING-SIZE regression (spdk-ii0): a module that declares MORE linear memory
 * than the object-sized zero-copy backing must still run -- served from a private
 * buffer (object bytes copied in), NOT zero-copy. bytecount.wasm declares 2 pages;
 * a sub-page object yields a 1-page backing, so this hits the private fallback.
 * Pre-fix, new_memory rejected this ("module min exceeds zero-copy backing") and
 * the Exec failed.
 */
static void
test_nkvx_tb4_multipage_module_private(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> TB4 multipage test skipped\n");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();

	/* Sub-page object -> 1-page backing; bytecount.wasm wants 2 pages. */
	static const uint8_t obj[] = "tiny-object";
	struct fake_fill fill = { .bytes = obj, .len = sizeof(obj), .calls = 0 };
	uint8_t out[64];
	uint32_t rlen = 0;
	uint64_t got = 0;
	int rc;
	struct kvdev_rados_nkvx_wasm_stats st;

	rc = kvdev_rados_nkvx_wasm_run_cached("bytecount", NULL, "mpK", sizeof(obj),
					      fake_cold_fill, &fill, out, sizeof(out), &rlen);
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_SUCCESS);   /* runs despite 2-page need */
	CU_ASSERT(rlen == sizeof(uint64_t));
	memcpy(&got, out, sizeof(got));
	CU_ASSERT(got == sizeof(obj));                   /* bytecount = object length */
	CU_ASSERT(fill.calls == 1);
	kvdev_rados_nkvx_wasm_get_stats(&st);
	/* Private-buffer fallback: linear memory is NOT the cache buffer (not aliased). */
	CU_ASSERT(st.last_mem_base != NULL);
	CU_ASSERT(st.last_mem_base != st.last_cache_base);
	printf("\n    TB4 multipage module: ran (len=%llu), private mem_base=%p != cache_base=%p\n",
	       (unsigned long long)got, st.last_mem_base, st.last_cache_base);

	kvdev_rados_nkvx_wasm_cache_reset();
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: TB4 multipage test is a no-op\n");
#endif
}

/*
 * SANDBOX ESCAPE regression (spdk-ii0 B1, ADR-0013): oob.wasm writes one byte
 * PAST its declared single-page linear memory. The zero-copy host buffer has no
 * guard region, so this is only caught because the engine config forces DYNAMIC
 * bounds checks (memory_reservation=0 / memory_guard_size=0). The access MUST
 * trap -> the run is CONTAINED (FAILED/ABORTED), never a SUCCESS that silently
 * clobbered host memory. Pre-fix (static elision, no guard) this returned SUCCESS
 * with the host heap corrupted.
 */
static void
test_nkvx_tb4_oob_traps(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> TB4 oob-trap test skipped\n");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();

	/* A small object keeps the cold-filled backing at exactly one 64 KiB page,
	 * so the module's offset-65536 store is exactly one byte past the memory. */
	static const uint8_t obj[] = "x";
	struct fake_fill fill = { .bytes = obj, .len = sizeof(obj), .calls = 0 };
	uint8_t out[64];
	uint32_t rlen = 0xdead;
	int rc;

	memset(out, 0xAB, sizeof(out));
	rc = kvdev_rados_nkvx_wasm_run_cached("oob", NULL, "oobK", sizeof(obj),
					      fake_cold_fill, &fill,
					      out, sizeof(out), &rlen);
	/* The OOB store MUST be caught: a contained failure, never SUCCESS. */
	CU_ASSERT(rc != SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_FAILED ||
		  rc == SPDK_KVDEV_IO_STATUS_ABORTED);
	printf("\n    TB4 oob.wasm OOB store at 65536: status=%d (trapped/contained, no host clobber)\n",
	       rc);

	kvdev_rados_nkvx_wasm_cache_reset();
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: TB4 oob-trap test is a no-op\n");
#endif
}

/*
 * SANDBOX-SEMANTICS regression (spdk-ii0 D1): a module declaring ONE page run
 * against a LARGER (2-page) object backing must NOT be able to reach the slack
 * between its declared size and the backing. The reviewer's CASE-A:
 *
 *   - slackwrite.wasm declares 1 page and writes at offset 70000 (past 65536 but
 *     inside the 2-page backing). Before the fix the custom memory reported the
 *     full backing as its size, so this slack write SUCCEEDED (escape of the
 *     module's declared bounds). After the fix the reported size is capped to the
 *     module's page-rounded minimum (1 page), so the access is OOB and MUST trap
 *     -> contained (FAILED/ABORTED), never SUCCESS.
 *   - pageprobe.wasm declares 1 page and writes IN-BOUNDS (offset 1000) against
 *     the SAME 2-page backing: it must still SUCCEED and still be ZERO-COPY
 *     (mem_base == cache_base) -- proving the cap does not break legitimate
 *     in-bounds access or the zero-copy alias.
 *
 * A ~70000-byte object forces the 2-page page-rounded backing for both modules.
 */
static void
test_nkvx_d1_declared_size_caps_slack(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> TB4 D1 slack test skipped\n");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();

	/* A ~70 KiB object -> page-rounded backing of 2 pages (131072), bigger than
	 * the single page these modules declare. */
	static uint8_t bigobj[70000];
	struct fake_fill fill_oob = { .bytes = bigobj, .len = sizeof(bigobj), .calls = 0 };
	struct fake_fill fill_ok  = { .bytes = bigobj, .len = sizeof(bigobj), .calls = 0 };
	uint8_t out[64];
	uint32_t rlen = 0;
	int rc;
	struct kvdev_rados_nkvx_wasm_stats st;

	memset(bigobj, 0x5A, sizeof(bigobj));

	/* CASE-A out-of-bounds half: write into the slack (offset 70000) MUST trap. */
	memset(out, 0xAB, sizeof(out));
	rc = kvdev_rados_nkvx_wasm_run_cached("slackwrite", NULL, "d1_oob", sizeof(bigobj),
					      fake_cold_fill, &fill_oob,
					      out, sizeof(out), &rlen);
	CU_ASSERT(rc != SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_FAILED ||
		  rc == SPDK_KVDEV_IO_STATUS_ABORTED);
	printf("\n    D1 slack write @70000 (1-page module, 2-page backing): status=%d (trapped)\n", rc);

	/* CASE-A in-bounds half: write at offset 1000 (in-bounds) MUST succeed AND
	 * still be zero-copy (linear memory aliases the cached object buffer). */
	memset(out, 0, sizeof(out));
	rlen = 0;
	rc = kvdev_rados_nkvx_wasm_run_cached("pageprobe", NULL, "d1_ok", sizeof(bigobj),
					      fake_cold_fill, &fill_ok,
					      out, sizeof(out), &rlen);
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(rlen == sizeof(uint64_t));
	kvdev_rados_nkvx_wasm_get_stats(&st);
	CU_ASSERT(st.last_mem_base != NULL);
	CU_ASSERT(st.last_mem_base == st.last_cache_base);	/* still zero-copy */
	printf("    D1 in-bounds write @1000 (1-page module, 2-page backing): SUCCESS, "
	       "zero-copy mem_base=%p cache_base=%p (alias=%s)\n",
	       st.last_mem_base, st.last_cache_base,
	       st.last_mem_base == st.last_cache_base ? "YES" : "NO");

	kvdev_rados_nkvx_wasm_cache_reset();
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: TB4 D1 slack test is a no-op\n");
#endif
}

/*
 * INTEGRITY / STALENESS regression (spdk-ii0 D2): the identity cache must be
 * INVALIDATED on a value mutation, or an Exec after a Store/Delete serves stale
 * cached bytes. This drives the invalidation API the datapath (kvdev_rados.c)
 * calls from Store/Delete:
 *
 *   Exec(k)         -> cold-fill v0 (cold_fills==1), correct checksum over v0.
 *   invalidate(k)   -> stands in for Store(k, v1) / the datapath cache_invalidate.
 *   Exec(k, v1)     -> MUST cold-fill AGAIN (cold_fills==2) and return a checksum
 *                      over the NEW bytes v1 -- NOT a stale hit on v0.
 *   invalidate(k)   -> stands in for Delete(k).
 *   <lookup misses> -> a subsequent Exec is a fresh cold-fill, never a stale hit.
 *
 * Fail-before: without the invalidate call the 2nd Exec is a content HIT
 * (cold_fills stays 1) and returns the v0 checksum -- demonstrably stale.
 */
static void
test_nkvx_d2_invalidate_on_mutation(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> TB4 D2 invalidate test skipped\n");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();

	static const uint8_t v0[] = "value-VERSION-0";
	static const uint8_t v1[] = "value-VERSION-ONE-different-length";
	struct fake_fill f0 = { .bytes = v0, .len = sizeof(v0), .calls = 0 };
	struct fake_fill f1 = { .bytes = v1, .len = sizeof(v1), .calls = 0 };
	uint8_t out[64];
	uint32_t rlen = 0;
	uint64_t got = 0;
	struct kvdev_rados_nkvx_wasm_stats st;

	/* Exec(k): cold-fill v0. */
	memset(out, 0, sizeof(out));
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, "k", sizeof(v0),
			fake_cold_fill, &f0, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	memcpy(&got, out, sizeof(got));
	CU_ASSERT(got == expected_checksum(v0, sizeof(v0)));
	kvdev_rados_nkvx_wasm_get_stats(&st);
	CU_ASSERT(st.cold_fills == 1);
	CU_ASSERT(f0.calls == 1);

	/* Store(k, v1): the datapath invalidates the cache here. */
	kvdev_rados_nkvx_wasm_cache_invalidate("k");

	/* Exec(k) again with the NEW bytes: must cold-fill AGAIN and reflect v1. */
	memset(out, 0, sizeof(out));
	got = 0;
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, "k", sizeof(v1),
			fake_cold_fill, &f1, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	memcpy(&got, out, sizeof(got));
	kvdev_rados_nkvx_wasm_get_stats(&st);
	/* The load-bearing assertions: a FRESH cold-fill and the NEW checksum. */
	CU_ASSERT(st.cold_fills == 2);
	CU_ASSERT(f1.calls == 1);
	CU_ASSERT(got == expected_checksum(v1, sizeof(v1)));
	CU_ASSERT(got != expected_checksum(v0, sizeof(v0)));	/* not stale */
	printf("\n    D2 Store-then-Exec: cold_fills=%llu new_checksum=0x%llx (fresh, not stale)\n",
	       (unsigned long long)st.cold_fills, (unsigned long long)got);

	/* Delete(k): invalidate again. A subsequent Exec must be a fresh cold-fill
	 * (a miss), proving the entry is truly gone -- never a stale hit. */
	kvdev_rados_nkvx_wasm_cache_invalidate("k");
	struct fake_fill f2 = { .bytes = v1, .len = sizeof(v1), .calls = 0 };
	memset(out, 0, sizeof(out));
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, "k", sizeof(v1),
			fake_cold_fill, &f2, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(f2.calls == 1);		/* fresh cold-fill, not a hit */
	kvdev_rados_nkvx_wasm_get_stats(&st);
	CU_ASSERT(st.cold_fills == 3);
	printf("    D2 Delete-then-Exec: cold_fills=%llu (miss -> fresh cold-fill, not stale)\n",
	       (unsigned long long)st.cold_fills);

	kvdev_rados_nkvx_wasm_cache_reset();
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: TB4 D2 invalidate test is a no-op\n");
#endif
}

/*
 * D2 race-safety (carry-ref): a probe-hit that PINS an object stays valid through
 * a concurrent invalidation -- the pinned version is served (no use-after-free),
 * while a NEW Exec after the invalidation misses and cold-fills fresh. This
 * mirrors the datapath cache_pin -> dispatch -> (Store invalidates) -> worker
 * run_pinned sequence, exercised here without threads (run_cached/run_pinned hold
 * the cache mutex, so the only real race window is probe->run, modelled below).
 */
static void
test_nkvx_d2_pin_survives_invalidate(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> TB4 D2 pin test skipped\n");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();

	static const uint8_t v0[] = "pinned-version-0";
	static const uint8_t v1[] = "fresh-version-1-after-store";
	struct fake_fill f0 = { .bytes = v0, .len = sizeof(v0), .calls = 0 };
	struct fake_fill f1 = { .bytes = v1, .len = sizeof(v1), .calls = 0 };
	uint8_t out[64];
	uint32_t rlen = 0;
	uint64_t got = 0;
	void *pin;
	struct kvdev_rados_nkvx_wasm_stats st;

	/* Exec(k): cold-fill v0 so the entry exists to pin. */
	memset(out, 0, sizeof(out));
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, "pk", sizeof(v0),
			fake_cold_fill, &f0, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);

	/* Datapath probe-hit: PIN the v0 entry (carry-ref through dispatch). */
	pin = kvdev_rados_nkvx_wasm_cache_pin("pk");
	CU_ASSERT(pin != NULL);

	/* Concurrent Store(k, v1): invalidate while the pin is outstanding. The
	 * pinned buffer must survive (deferred free); new lookups miss it. */
	kvdev_rados_nkvx_wasm_cache_invalidate("pk");

	/* The worker runs the PINNED version: still v0, no use-after-free. */
	memset(out, 0, sizeof(out));
	got = 0;
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_pinned("checksum", NULL, pin, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	memcpy(&got, out, sizeof(got));
	CU_ASSERT(got == expected_checksum(v0, sizeof(v0)));	/* the pinned version */
	kvdev_rados_nkvx_wasm_cache_unpin(pin);

	/* A NEW Exec(k) after the invalidation MISSES and cold-fills v1 -- the dead
	 * pinned entry is invisible to new lookups, so no stale hit. */
	memset(out, 0, sizeof(out));
	got = 0;
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, "pk", sizeof(v1),
			fake_cold_fill, &f1, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	memcpy(&got, out, sizeof(got));
	CU_ASSERT(f1.calls == 1);				/* fresh cold-fill */
	CU_ASSERT(got == expected_checksum(v1, sizeof(v1)));	/* new version */
	kvdev_rados_nkvx_wasm_get_stats(&st);
	printf("\n    D2 pin-survives-invalidate: pinned served v0, new Exec cold-filled v1 "
	       "(cold_fills=%llu, no UAF, no stale)\n", (unsigned long long)st.cold_fills);

	kvdev_rados_nkvx_wasm_cache_reset();
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: TB4 D2 pin test is a no-op\n");
#endif
}

/*
 * CONTAINMENT regression on the WARM/cached path (spdk-ii0 D3, closes spdk-9jl):
 * the cached path must arm the SAME caps as the cold run() — memory limiter AND
 * the epoch wall-clock backstop — not just fuel.
 *
 *   (a) MEMORY: a module that grows past the warm-path memory bound is contained
 *       (the grow fails, the module traps) — never a host OOM.
 *   (b) EPOCH:  a wall-clock-runaway module with FUEL DISABLED must be stopped by
 *       the epoch deadline the warm engine now arms (before the fix the warm path
 *       armed only fuel, so a fuel=0 runaway ran unbounded). Run on the warm path
 *       with SPDK_NKVX_WASM_FUEL=0 and a short epoch budget -> ABORTED, no hang.
 *
 * MECHANISM NOTE (spdk-90x). The warm/cached path is ALWAYS backed by our custom
 * MemoryCreator (zero-copy alias, or the private fallback). For a custom host
 * memory the REAL linear-memory bound is nkvx_zc_grow, which refuses any growth
 * past the object backing (m->cap) and returns a wasmtime error -> the grow traps.
 * The wasmtime store memory LIMITER (store_limiter) is belt-and-suspenders on this
 * path: it never gets the chance to be the deciding bound because nkvx_zc_grow
 * refuses growth first. So this warm test asserts the ACTUAL mechanism — that the
 * grow is refused (contained) — and is INDEPENDENT of the store_limiter. The
 * store_limiter is proven load-bearing separately, on the NON-custom-memory plain
 * run() path, by test_nkvx_store_limiter_bounds_plain_path below (where disabling
 * the limiter genuinely flips the result). This test used to (mis)attribute the
 * containment to the store_limiter even though disabling it did not change the
 * outcome (the overalloc grow was already refused by nkvx_zc_grow).
 */
static void
test_nkvx_d3_warm_memory_cap_contained(void)
{
#ifdef NKVX_WASM_RUNTIME_TESTS
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	/* A memory cap is set, but the warm-path bound is nkvx_zc_grow (see note). */
	setenv("SPDK_NKVX_WASM_MAX_MEMORY", "1048576", 1);	/* 1 MiB cap */
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> TB4 D3 memory test skipped\n");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();

	static const uint8_t obj[] = "x";
	struct fake_fill fill = { .bytes = obj, .len = sizeof(obj), .calls = 0 };
	uint8_t out[64];
	uint32_t rlen = 0;
	int rc;

	memset(out, 0, sizeof(out));
	rc = kvdev_rados_nkvx_wasm_run_cached("overalloc", NULL, "d3memK", sizeof(obj),
					      fake_cold_fill, &fill, out, sizeof(out), &rlen);
	/* Contained by the WARM-path bound (nkvx_zc_grow refuses the grow past the
	 * backing): a clean failure, not an OOM. Independent of the store_limiter. */
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_ABORTED ||
		  rc == SPDK_KVDEV_IO_STATUS_FAILED);
	printf("\n    D3 warm memory cap: status=%d (contained on cached path by nkvx_zc_grow)\n",
	       rc);

	kvdev_rados_nkvx_wasm_cache_reset();
	unsetenv("SPDK_NKVX_WASM_MAX_MEMORY");
#else
	printf("\n    built --without-wasm: TB4 D3 memory test is a no-op\n");
#endif
}

/*
 * STORE-LIMITER load-bearing proof on the NON-custom-memory plain run() path
 * (spdk-90x). The plain run() path uses wasmtime's DEFAULT (non-custom) linear
 * memory, whose growth is bounded by the wasmtime store memory LIMITER
 * (store_limiter) — NOT by nkvx_zc_grow (which only governs the custom warm-path
 * memory). This is therefore the path where the store_limiter is the genuine,
 * deciding bound, so it is the right place to prove it is load-bearing.
 *
 * growcap.wasm grows a BOUNDED 1024 pages (64 MiB) then returns SUCCESS:
 *   - WITH a tight 1 MiB cap the limiter refuses a grow well before 64 MiB; the
 *     module traps -> the dispatch is contained (ABORTED/FAILED).
 *   - The growth is BOUNDED, so even if the cap were removed the module would
 *     allocate at most 64 MiB and return SUCCESS — it would NOT OOM the target.
 *     That bound is what makes the fail-before (disable the limiter -> SUCCESS)
 *     safe to run; see the manual fail-before note in the commit.
 *
 * Run via the dispatch path (NULL obj_key) so it goes through run() (plain copy,
 * no MemoryCreator), exercising exactly the store_limiter-bounded path.
 *
 * Fail-before/after (manual, documented): with the plain-path store_limiter call
 * commented out, growcap grows all 1024 bounded pages and returns SUCCESS, so the
 * "contained" assert below FAILS; with the limiter in place the grow is refused at
 * the cap and the dispatch is contained. The result flips solely on the limiter,
 * proving it load-bearing on this path.
 */
static void
test_nkvx_store_limiter_bounds_plain_path(void)
{
	struct nkvx_result r;
	uint8_t out[64];

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

#ifdef NKVX_WASM_RUNTIME_TESTS
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	/* Tight 1 MiB cap, far below growcap's bounded 64 MiB target. Fuel/epoch high
	 * enough not to interfere (the bounded grow loop is short). */
	setenv("SPDK_NKVX_WASM_MAX_MEMORY", "1048576", 1);
	setenv("SPDK_NKVX_WASM_FUEL", "1000000000", 1);
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");

	memset(out, 0, sizeof(out));
	/* NULL obj_key -> plain run() path (default wasmtime memory, store_limiter). */
	dispatch_and_wait("wasm:growcap", "x", 1, out, sizeof(out), &r);
	CU_ASSERT(r.completed);
	if (r.kvstatus != SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED) {
		/* The store limiter refused the grow at the cap -> the module trapped ->
		 * contained. With the limiter the result is NOT SUCCESS (the bounded grow
		 * could not finish under the 1 MiB cap). */
		CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_ABORTED ||
			  r.kvstatus == SPDK_KVDEV_IO_STATUS_FAILED);
		CU_ASSERT(r.kvstatus != SPDK_KVDEV_IO_STATUS_SUCCESS);
		printf("\n    store-limiter (plain path): growcap contained at 1MiB cap, status=%d "
		       "(limiter load-bearing; disable it -> bounded grow SUCCEEDS)\n", r.kvstatus);
	} else {
		printf("\n    wasm runtime unavailable -> store-limiter plain-path test skipped\n");
	}
	unsetenv("SPDK_NKVX_WASM_MAX_MEMORY");
	unsetenv("SPDK_NKVX_WASM_FUEL");
#else
	(void)r;
	(void)out;
	printf("\n    built --without-wasm: store-limiter plain-path test is a no-op\n");
#endif
	kvdev_rados_nkvx_stop();
}

static void
test_nkvx_d3_warm_epoch_cap_contained(void)
{
#ifdef NKVX_WASM_RUNTIME_TESTS
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	/* Fuel OFF so ONLY the warm-path epoch deadline can stop it; short budget. */
	setenv("SPDK_NKVX_WASM_FUEL", "0", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "3", 1);
	unsetenv("SPDK_NKVX_WASM_MAX_MEMORY");

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> TB4 D3 epoch test skipped\n");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();

	static const uint8_t obj[] = "x";
	struct fake_fill fill = { .bytes = obj, .len = sizeof(obj), .calls = 0 };
	uint8_t out[64];
	uint32_t rlen = 0;
	int rc;

	memset(out, 0, sizeof(out));
	rc = kvdev_rados_nkvx_wasm_run_cached("walltime_runaway", NULL, "d3epK", sizeof(obj),
					      fake_cold_fill, &fill, out, sizeof(out), &rlen);
	/* Stopped by the warm-path EPOCH deadline (fuel was disabled) -> ABORTED. */
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_ABORTED);
	printf("\n    D3 warm epoch cap: status=%d (ABORTED by wall-clock on cached path)\n", rc);

	kvdev_rados_nkvx_wasm_cache_reset();
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: TB4 D3 epoch test is a no-op\n");
#endif
}

/*
 * Fail-soft: --without-wasm or libwasmtime.so absent -> run_cached returns
 * NOT_SUPPORTED and NEVER invokes the fill callback (the executor cannot run the
 * module, so it must not cold-fill). Deterministic in both build modes.
 */
static void
test_nkvx_tb4_cached_failsoft(void)
{
	struct fake_fill f = { .bytes = "x", .len = 1, .calls = 0 };
	uint8_t out[16];
	uint32_t rlen = 0;
	int rc;

#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	if (nkvx_wasm_runtime_available()) {
		/* Runtime present: this test's fail-soft assertion does not apply. */
		printf("\n    wasm runtime available -> fail-soft test not applicable (ok)\n");
		kvdev_rados_nkvx_wasm_cache_reset();
		return;
	}
#endif
	rc = kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, "objK", 1,
					      fake_cold_fill, &f, out, sizeof(out), &rlen);
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED);
	CU_ASSERT(f.calls == 0);	/* no cold fill when the runtime can't run it */
	CU_ASSERT(rlen == 0);
	printf("\n    TB4 cached fail-soft: NOT_SUPPORTED, fill not called (ok)\n");
}

/*
 * Live executor wiring (TB4): a wasm: dispatch carrying an obj_key flows through
 * the off-reactor worker into the content-addressed cache. Two dispatches of the
 * SAME (module, oid) cold-fill ONCE (the worker's fill shim runs once); the second
 * is a content hit + warm reuse — proving the executor, not just the cache unit,
 * is wired to TB4.
 */
static void
test_nkvx_tb4_dispatch_wires_cache(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	static const char obj[] = "dispatch-cached-object";
	const uint32_t obj_len = (uint32_t)sizeof(obj);
	uint8_t out[64];
	struct nkvx_result r;
	struct kvdev_rados_nkvx_wasm_stats st;

	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> TB4 dispatch-wiring test skipped\n");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

	/* 1st dispatch of (checksum, oidZ): cold fill via the worker shim. */
	memset(&r, 0, sizeof(r));
	memset(out, 0, sizeof(out));
	set_thread(0);
	CU_ASSERT(kvdev_rados_nkvx_dispatch("wasm:checksum", NULL, "oidZ", NULL, obj, obj_len,
					    out, sizeof(out), nkvx_done, &r) == 0);
	set_thread(INVALID_THREAD);
	for (int i = 0; i < 100000 && !r.completed; i++) {
		poll_threads();
		if (!r.completed) {
			usleep(100);
		}
	}
	CU_ASSERT(r.completed);
	CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_SUCCESS);
	kvdev_rados_nkvx_wasm_get_stats(&st);
	CU_ASSERT(st.cold_fills == 1);
	CU_ASSERT(st.last_mem_base == st.last_cache_base);	/* zero-copy */

	/* 2nd dispatch of the SAME (module, oid): content hit + warm reuse. */
	memset(&r, 0, sizeof(r));
	memset(out, 0, sizeof(out));
	set_thread(0);
	CU_ASSERT(kvdev_rados_nkvx_dispatch("wasm:checksum", NULL, "oidZ", NULL, obj, obj_len,
					    out, sizeof(out), nkvx_done, &r) == 0);
	set_thread(INVALID_THREAD);
	for (int i = 0; i < 100000 && !r.completed; i++) {
		poll_threads();
		if (!r.completed) {
			usleep(100);
		}
	}
	CU_ASSERT(r.completed);
	CU_ASSERT(r.kvstatus == SPDK_KVDEV_IO_STATUS_SUCCESS);
	kvdev_rados_nkvx_wasm_get_stats(&st);
	CU_ASSERT(st.cold_fills == 1);		/* NO refetch */
	CU_ASSERT(st.content_hits == 1);
	CU_ASSERT(st.warm_hits == 1);		/* warm reuse */
	printf("\n    TB4 dispatch wiring: cold_fills=%llu content_hits=%llu warm_hits=%llu (executor wired)\n",
	       (unsigned long long)st.cold_fills, (unsigned long long)st.content_hits,
	       (unsigned long long)st.warm_hits);

	kvdev_rados_nkvx_stop();
	kvdev_rados_nkvx_wasm_cache_reset();
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: TB4 dispatch-wiring test is a no-op\n");
#endif
}

/* ==========================================================================
 * TB3 (spdk-fbm / ADR-0010): the content-addressed module cache + the SHA-256
 * integrity GATE. These exercise the executor's verified-module path directly
 * (the sha256 cache + kvdev_rados_nkvx_wasm_module_insert + the verified run_cached
 * path), with a FAKE cold-fill standing in for the librados DATA read. The MODULE
 * bytes are the real checked-in .wasm files, hashed at runtime so the tests are
 * self-contained. The reactor-side deny-by-default gate (a wasm op with no valid
 * sha256 rejected before any fetch) lives in kvdev_rados.c:kvdev_rados_exec and is
 * covered by the live e2e + code review; here we prove the executor's hash gate,
 * which is the load-bearing "never run unverified bytes" enforcement point.
 *
 * Graceful-degradation aware: when the runtime is unavailable these skip, like the
 * other wasm tests.
 * ========================================================================== */
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)

/* Read a checked-in <name>.wasm into a malloc'd buffer; CU_FAIL + return NULL on
 * error. Caller frees. */
static uint8_t *
tb3_read_wasm(const char *name, size_t *out_len)
{
	char path[1024];
	uint8_t *buf;

	snprintf(path, sizeof(path), "%s/%s.wasm", NKVX_UT_WASM_DIR, name);
	buf = nkvx_wasm_read_file(path, out_len);
	if (buf == NULL) {
		CU_FAIL("could not read test .wasm module");
	}
	return buf;
}

/* SHA-256 of (buf,len) into out[32] using the same primitive as the executor. */
static void
tb3_sha256(const void *buf, size_t len, uint8_t out[SPDK_KV_EXEC_SHA256_LEN])
{
	CU_ASSERT(nkvx_sha256(buf, len, out));
}
#endif /* SPDK_CONFIG_WASM && NKVX_UT_WASM_DIR */

/*
 * TB3 (a) deny-by-default / hash GATE — sha256 MISMATCH is rejected and the module
 * is NEVER compiled, cached, or run; the FAIL-BEFORE is shown by flipping one byte
 * of the bound hash to flip the verdict from SUCCESS to INVALID.
 */
static void
test_nkvx_tb3_hash_mismatch_rejected(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> TB3 hash-mismatch test skipped\n");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();
	kvdev_rados_nkvx_wasm_module_cache_reset();

	size_t wlen = 0;
	uint8_t *wasm = tb3_read_wasm("checksum", &wlen);
	uint8_t good[SPDK_KV_EXEC_SHA256_LEN], bad[SPDK_KV_EXEC_SHA256_LEN];
	int rc;

	if (wasm == NULL) {
		return;
	}
	tb3_sha256(wasm, wlen, good);

	/* The CORRECT hash verifies + compiles + caches: the baseline that the
	 * mismatch must differ from (fail-before / fail-after pair). */
	rc = kvdev_rados_nkvx_wasm_module_insert(good, wasm, wlen);
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_cached(good));
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_cache_count() == 1);

	/* Now TAMPER: flip one byte of the bound hash. The SAME bytes must now be
	 * REJECTED (INVALID), never compiled/cached. The verdict flipped solely on the
	 * hash -> the gate is load-bearing. */
	memcpy(bad, good, sizeof(bad));
	bad[0] ^= 0x01;
	rc = kvdev_rados_nkvx_wasm_module_insert(bad, wasm, wlen);
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_INVALID);
	CU_ASSERT(!kvdev_rados_nkvx_wasm_module_cached(bad));	/* never cached */
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_cache_count() == 1);	/* still just the good one */

	/* Equivalently: tampering the BYTES (not the hash) is also rejected — the
	 * module's content no longer matches its bound hash. */
	wasm[wlen / 2] ^= 0xFF;
	rc = kvdev_rados_nkvx_wasm_module_insert(good, wasm, wlen);
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_INVALID);
	printf("\n    TB3 hash gate: correct hash -> SUCCESS+cached; flipped hash/bytes -> INVALID, "
	       "never compiled (cache_count stays 1)\n");

	free(wasm);
	kvdev_rados_nkvx_wasm_cache_reset();
	kvdev_rados_nkvx_wasm_module_cache_reset();
#else
	printf("\n    built --without-wasm: TB3 hash-mismatch test is a no-op\n");
#endif
}

/*
 * TB3 (c) successful fetch+verify+compile+run, and (d) module-cache HIT on the 2nd
 * Exec of the SAME hash (no recompile). The DATA object is served via the fake
 * cold-fill; the MODULE comes via the verified \c mod (bytes on the 1st Exec,
 * NULL/cached on the 2nd). The module cache count proves exactly one compile.
 */
static void
test_nkvx_tb3_verify_run_and_module_cache_hit(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);
	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> TB3 verify+run test skipped\n");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();
	kvdev_rados_nkvx_wasm_module_cache_reset();

	size_t wlen = 0;
	uint8_t *wasm = tb3_read_wasm("checksum", &wlen);
	struct kvdev_rados_nkvx_module mod;
	static const uint8_t obj[] = "tb3-verified-run-object";
	struct fake_fill fill = { .bytes = obj, .len = sizeof(obj), .calls = 0 };
	uint8_t out[64];
	uint32_t rlen = 0;
	uint64_t got = 0;
	int rc;

	if (wasm == NULL) {
		return;
	}
	memset(&mod, 0, sizeof(mod));
	tb3_sha256(wasm, wlen, mod.sha256);
	mod.bytes = wasm;		/* 1st Exec: fetched bytes supplied (cache miss) */
	mod.bytes_len = wlen;
	mod.caps = 0;			/* defaults tier */

	/* 1st Exec: module verified + compiled + cached, then run over the data obj. */
	memset(out, 0, sizeof(out));
	rc = kvdev_rados_nkvx_wasm_run_cached("checksum", &mod, "tb3objA", sizeof(obj),
					      fake_cold_fill, &fill, out, sizeof(out), &rlen);
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(rlen == sizeof(uint64_t));
	memcpy(&got, out, sizeof(got));
	CU_ASSERT(got == expected_checksum(obj, sizeof(obj)));	/* correct verified run */
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_cache_count() == 1);	/* compiled once */

	/* 2nd Exec of the SAME hash on a DIFFERENT data object: the module is a CACHE
	 * HIT — mod.bytes is NULL (no refetch needed) and no recompile happens. */
	static const uint8_t obj2[] = "tb3-second-object-different";
	struct fake_fill fill2 = { .bytes = obj2, .len = sizeof(obj2), .calls = 0 };
	struct kvdev_rados_nkvx_module mod_hit;

	memset(&mod_hit, 0, sizeof(mod_hit));
	memcpy(mod_hit.sha256, mod.sha256, sizeof(mod_hit.sha256));
	mod_hit.bytes = NULL;		/* prove the cached compiled module is used */
	mod_hit.bytes_len = 0;

	CU_ASSERT(kvdev_rados_nkvx_wasm_module_cached(mod_hit.sha256));	/* reactor probe would HIT */
	memset(out, 0, sizeof(out));
	got = 0;
	rc = kvdev_rados_nkvx_wasm_run_cached("checksum", &mod_hit, "tb3objB", sizeof(obj2),
					      fake_cold_fill, &fill2, out, sizeof(out), &rlen);
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_SUCCESS);
	memcpy(&got, out, sizeof(got));
	CU_ASSERT(got == expected_checksum(obj2, sizeof(obj2)));
	/* HIT proof: still exactly one compiled module — no recompile on the 2nd Exec
	 * even though it carried NO module bytes. */
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_cache_count() == 1);
	printf("\n    TB3 verify+run: 1st Exec compiled+ran (checksum ok); 2nd Exec of same hash "
	       "served from module cache (count=1, bytes=NULL, no recompile)\n");

	free(wasm);
	kvdev_rados_nkvx_wasm_cache_reset();
	kvdev_rados_nkvx_wasm_module_cache_reset();
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: TB3 verify+run test is a no-op\n");
#endif
}

/*
 * READ-ONLY-EXECUTOR GUARANTEE (S5b / ADR-0008 D3): a WRITE-CAPABLE wasm module —
 * one that exports the reserved write-intent symbol
 * KVDEV_RADOS_NKVX_WASM_WRITE_EXPORT ("kv_write") — is rejected with READ_ONLY on
 * a read-only namespace BEFORE its entry function runs, so it can never mutate
 * read-only data; the SAME module on a writable namespace runs normally.
 * writer.wasm exports both its `writer` entry and `kv_write`. The gate keys on
 * mod->read_only (the inter-tier read_only bit the executor threads in), proven
 * here on the verified-module run_cached path the executor uses.
 */
static void
test_nkvx_wasm_write_capable_read_only_rejected(void)
{
#ifdef NKVX_WASM_RUNTIME_TESTS
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);
	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> RO-executor test skipped (fail-soft)\n");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();
	kvdev_rados_nkvx_wasm_module_cache_reset();

	size_t wlen = 0;
	uint8_t *wasm = tb3_read_wasm("writer", &wlen);
	static const uint8_t obj[] = "ro-guarantee-object";
	struct fake_fill fill = { .bytes = obj, .len = sizeof(obj), .calls = 0 };
	struct kvdev_rados_nkvx_module mod;
	uint8_t out[64];
	uint32_t rlen = 0;
	uint64_t got = 0;
	int rc;

	if (wasm == NULL) {
		return;
	}
	memset(&mod, 0, sizeof(mod));
	tb3_sha256(wasm, wlen, mod.sha256);
	mod.bytes = wasm;
	mod.bytes_len = wlen;
	mod.caps = 0;

	/* READ-ONLY ns: the write-capable module is rejected with READ_ONLY before its
	 * entry function runs (no result produced). */
	mod.read_only = true;
	memset(out, 0, sizeof(out));
	rc = kvdev_rados_nkvx_wasm_run_cached("writer", &mod, "roK", sizeof(obj),
					      fake_cold_fill, &fill, out, sizeof(out), &rlen);
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_READ_ONLY);
	CU_ASSERT(rlen == 0);
	printf("\n    RO-executor: write-capable 'writer' on read-only ns -> status=%d "
	       "(READ_ONLY expected), entry never ran\n", rc);

	/* WRITABLE ns: the SAME module runs normally and returns the 8-byte length. */
	mod.read_only = false;
	mod.bytes = wasm;		/* re-supply bytes (cache was reset) */
	mod.bytes_len = wlen;
	memset(out, 0, sizeof(out));
	rlen = 0;
	rc = kvdev_rados_nkvx_wasm_run_cached("writer", &mod, "rwK", sizeof(obj),
					      fake_cold_fill, &fill, out, sizeof(out), &rlen);
	CU_ASSERT(rc == SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(rlen == sizeof(uint64_t));
	memcpy(&got, out, sizeof(got));
	CU_ASSERT(got == (uint64_t)sizeof(obj));	/* writer returns the object length */
	printf("    RO-executor: same module on a writable ns -> status=%d len=%llu (SUCCESS)\n",
	       rc, (unsigned long long)got);

	free(wasm);
	kvdev_rados_nkvx_wasm_cache_reset();
	kvdev_rados_nkvx_wasm_module_cache_reset();
#else
	printf("\n    built --without-wasm: RO-executor guarantee test is a no-op\n");
#endif
}

/*
 * TB3 (b) a module-cache MISS with NO fetched bytes is a clean failure, never a run
 * of unverified/absent bytes. This is the executor-side guard for the reactor's
 * "fetch the module on a miss" contract: if a run reaches the executor on a miss
 * without the bytes, it fails closed.
 */
static void
test_nkvx_tb3_miss_without_bytes_fails_closed(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> TB3 fail-closed test skipped\n");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();
	kvdev_rados_nkvx_wasm_module_cache_reset();

	struct kvdev_rados_nkvx_module mod;
	static const uint8_t obj[] = "x";
	struct fake_fill fill = { .bytes = obj, .len = sizeof(obj), .calls = 0 };
	uint8_t out[16];
	uint32_t rlen = 0;
	int rc;

	memset(&mod, 0, sizeof(mod));
	/* A plausible-but-uncached hash with NO bytes: nothing to verify/compile. */
	memset(mod.sha256, 0xAB, sizeof(mod.sha256));
	mod.bytes = NULL;
	mod.bytes_len = 0;

	CU_ASSERT(!kvdev_rados_nkvx_wasm_module_cached(mod.sha256));
	rc = kvdev_rados_nkvx_wasm_run_cached("checksum", &mod, "tb3missK", sizeof(obj),
					      fake_cold_fill, &fill, out, sizeof(out), &rlen);
	CU_ASSERT(rc != SPDK_KVDEV_IO_STATUS_SUCCESS);	/* failed closed, nothing run */
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_cache_count() == 0);
	printf("\n    TB3 fail-closed: module-cache miss with no bytes -> status=%d (no run, count=0)\n",
	       rc);

	kvdev_rados_nkvx_wasm_cache_reset();
	kvdev_rados_nkvx_wasm_module_cache_reset();
#else
	printf("\n    built --without-wasm: TB3 fail-closed test is a no-op\n");
#endif
}

/*
 * spdk-wwy: the object + warm + module caches are BOUNDED (LRU eviction). Fill
 * each past its cap and assert it stays bounded (count/bytes) and that eviction
 * happened (the eviction counter advanced). The default caps are large, so the
 * tests set tight env caps. Helper to run a checksum Exec on a unique key.
 */
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
static int
wwy_exec_key(const char *key, const uint8_t *obj, size_t obj_len, uint64_t *out_sum)
{
	struct fake_fill fill = { .bytes = obj, .len = obj_len, .calls = 0 };
	uint8_t out[64];
	uint32_t rlen = 0;
	int rc;

	memset(out, 0, sizeof(out));
	rc = kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, key, obj_len,
					      fake_cold_fill, &fill, out, sizeof(out), &rlen);
	if (rc == SPDK_KVDEV_IO_STATUS_SUCCESS && out_sum != NULL) {
		memcpy(out_sum, out, sizeof(*out_sum));
	}
	return rc;
}
#endif

/*
 * spdk-wwy (1): the OBJECT cache is bounded by an LRU count cap. Fill past the cap
 * with distinct keys and assert the live count stays <= cap and that evictions
 * occurred. Fail-before: an UNBOUNDED cache would hold all N entries (count == N,
 * obj_evictions == 0).
 */
static void
test_nkvx_wwy_object_cache_bounded(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);
	setenv("SPDK_NKVX_OBJ_CACHE_MAX_COUNT", "4", 1);	/* tight count cap */
	unsetenv("SPDK_NKVX_OBJ_CACHE_MAX_BYTES");

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> wwy object-cache test skipped\n");
		goto done;
	}
	kvdev_rados_nkvx_wasm_cache_reset();

	static const uint8_t obj[] = "evict-me-object-bytes";
	struct kvdev_rados_nkvx_wasm_stats st;
	char key[32];
	int i;
	const int N = 12;	/* well past the cap of 4 */

	for (i = 0; i < N; i++) {
		snprintf(key, sizeof(key), "k%d", i);
		CU_ASSERT(wwy_exec_key(key, obj, sizeof(obj), NULL) ==
			  SPDK_KVDEV_IO_STATUS_SUCCESS);
	}

	/* Bounded: live count never exceeded the cap. */
	CU_ASSERT(kvdev_rados_nkvx_wasm_obj_cache_count() <= 4);
	kvdev_rados_nkvx_wasm_get_stats(&st);
	CU_ASSERT(st.obj_evictions >= (uint64_t)(N - 4));	/* evicted the overflow */
	CU_ASSERT(st.obj_evictions > 0);
	printf("\n    wwy object cache: inserted %d, live=%llu (cap 4), obj_evictions=%llu (bounded)\n",
	       N, (unsigned long long)kvdev_rados_nkvx_wasm_obj_cache_count(),
	       (unsigned long long)st.obj_evictions);

	kvdev_rados_nkvx_wasm_cache_reset();
done:
	unsetenv("SPDK_NKVX_OBJ_CACHE_MAX_COUNT");
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: wwy object-cache test is a no-op\n");
#endif
}

/*
 * spdk-wwy (2): a PINNED object is NOT freed by eviction (carry-ref, no UAF). Pin
 * an object, then drive enough cold-fills to exceed the cap so the pinned object is
 * the LRU victim. The pinned bytes MUST stay valid (run_pinned returns the correct
 * checksum over the pinned version); after unpin the entry is freed. valgrind
 * confirms no use-after-free / no leak.
 *
 * Fail-before: an evictor that did a bare free of the LRU entry (instead of the
 * dead+defer-to-last-unpin path) would free the pinned buffer and run_pinned would
 * read freed memory (UAF -> wrong checksum / valgrind error).
 */
static void
test_nkvx_wwy_pinned_survives_eviction(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);
	setenv("SPDK_NKVX_OBJ_CACHE_MAX_COUNT", "3", 1);
	unsetenv("SPDK_NKVX_OBJ_CACHE_MAX_BYTES");

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> wwy pinned-eviction test skipped\n");
		goto done;
	}
	kvdev_rados_nkvx_wasm_cache_reset();

	static const uint8_t pv[] = "pinned-victim-object-bytes";
	static const uint8_t filler[] = "filler-object";
	struct fake_fill pf = { .bytes = pv, .len = sizeof(pv), .calls = 0 };
	uint8_t out[64];
	uint32_t rlen = 0;
	uint64_t got = 0;
	void *pin;
	char key[32];
	int i;
	struct kvdev_rados_nkvx_wasm_stats st;

	/* Cold-fill the victim object so it exists to pin (it becomes the LRU head). */
	memset(out, 0, sizeof(out));
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, "pinvic", sizeof(pv),
			fake_cold_fill, &pf, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);

	/* PIN it (datapath probe-hit carry-ref). */
	pin = kvdev_rados_nkvx_wasm_cache_pin("pinvic");
	CU_ASSERT(pin != NULL);

	/* Drive enough DISTINCT cold-fills to exceed the cap -> the pinned object (LRU)
	 * is selected for eviction, marked dead, but its bytes deferred (still pinned). */
	for (i = 0; i < 8; i++) {
		snprintf(key, sizeof(key), "f%d", i);
		CU_ASSERT(wwy_exec_key(key, filler, sizeof(filler), NULL) ==
			  SPDK_KVDEV_IO_STATUS_SUCCESS);
	}
	kvdev_rados_nkvx_wasm_get_stats(&st);
	CU_ASSERT(st.obj_evictions > 0);
	/* The pinned object is gone from new lookups (evicted/dead) -> a cache_has miss. */
	CU_ASSERT(kvdev_rados_nkvx_wasm_cache_has("pinvic") == false);

	/* The PINNED bytes are still valid: run the pinned version, correct checksum
	 * (no use-after-free even though the entry was eviction-selected). */
	memset(out, 0, sizeof(out));
	got = 0;
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_pinned("checksum", NULL, pin, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	memcpy(&got, out, sizeof(got));
	CU_ASSERT(got == expected_checksum(pv, sizeof(pv)));	/* pinned bytes intact */
	printf("\n    wwy pinned survives eviction: obj_evictions=%llu, pinned checksum still "
	       "correct (no UAF)\n", (unsigned long long)st.obj_evictions);

	/* Drop the pin -> the deferred free happens now (last unpin). valgrind clean. */
	kvdev_rados_nkvx_wasm_cache_unpin(pin);

	kvdev_rados_nkvx_wasm_cache_reset();
done:
	unsetenv("SPDK_NKVX_OBJ_CACHE_MAX_COUNT");
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: wwy pinned-eviction test is a no-op\n");
#endif
}

/*
 * spdk-wwy (3): the WARM-instance cache is bounded by an LRU count cap. With a tight
 * warm cap and a LARGER object cap, running many distinct (module,object) pairs
 * evicts warm entries. Fail-before: unbounded warm cache holds all of them.
 */
static void
test_nkvx_wwy_warm_cache_bounded(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);
	setenv("SPDK_NKVX_WARM_CACHE_MAX_COUNT", "3", 1);	/* tight warm cap */
	setenv("SPDK_NKVX_OBJ_CACHE_MAX_COUNT", "64", 1);	/* generous object cap */
	unsetenv("SPDK_NKVX_OBJ_CACHE_MAX_BYTES");

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> wwy warm-cache test skipped\n");
		goto done;
	}
	kvdev_rados_nkvx_wasm_cache_reset();

	static const uint8_t obj[] = "warm-evict-object";
	struct kvdev_rados_nkvx_wasm_stats st;
	char key[32];
	int i;
	const int N = 10;

	for (i = 0; i < N; i++) {
		snprintf(key, sizeof(key), "wk%d", i);	/* distinct objects -> distinct warm */
		CU_ASSERT(wwy_exec_key(key, obj, sizeof(obj), NULL) ==
			  SPDK_KVDEV_IO_STATUS_SUCCESS);
	}

	CU_ASSERT(kvdev_rados_nkvx_wasm_warm_cache_count() <= 3);
	kvdev_rados_nkvx_wasm_get_stats(&st);
	CU_ASSERT(st.warm_evictions > 0);
	printf("\n    wwy warm cache: ran %d distinct pairs, warm live=%llu (cap 3), "
	       "warm_evictions=%llu (bounded)\n", N,
	       (unsigned long long)kvdev_rados_nkvx_wasm_warm_cache_count(),
	       (unsigned long long)st.warm_evictions);

	kvdev_rados_nkvx_wasm_cache_reset();
done:
	unsetenv("SPDK_NKVX_WARM_CACHE_MAX_COUNT");
	unsetenv("SPDK_NKVX_OBJ_CACHE_MAX_COUNT");
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: wwy warm-cache test is a no-op\n");
#endif
}

/*
 * spdk-wwy (4): the sha256 MODULE cache is bounded by an LRU count cap. Insert
 * distinct verified modules past the cap (hash-perturbed copies of a real .wasm)
 * and assert the count stays <= cap with evictions. Fail-before: unbounded module
 * cache holds every inserted hash.
 */
static void
test_nkvx_wwy_module_cache_bounded(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	setenv("SPDK_NKVX_MOD_CACHE_MAX_COUNT", "3", 1);
	unsetenv("SPDK_NKVX_MOD_CACHE_MAX_BYTES");

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> wwy module-cache test skipped\n");
		goto done;
	}
	kvdev_rados_nkvx_wasm_cache_reset();
	kvdev_rados_nkvx_wasm_module_cache_reset();

	size_t wlen = 0;
	uint8_t *wasm = tb3_read_wasm("checksum", &wlen);
	struct kvdev_rados_nkvx_wasm_stats st;
	int i;
	const int N = 9;

	if (wasm == NULL) {
		goto done;
	}

	/* Insert N distinct modules. Perturb a copy of the bytes each time and bind the
	 * matching (correct) hash, so each is a genuine verified insert with a unique
	 * key — exercising the cap with real compiles. */
	for (i = 0; i < N; i++) {
		uint8_t *copy = malloc(wlen);
		uint8_t h[SPDK_KV_EXEC_SHA256_LEN];
		int rc;

		CU_ASSERT_FATAL(copy != NULL);
		memcpy(copy, wasm, wlen);
		/* Flip a byte in a benign location (a data byte near the end) so the module
		 * still compiles but hashes differently. */
		copy[wlen - 1 - i] ^= (uint8_t)(0x11 + i);
		CU_ASSERT(nkvx_sha256(copy, wlen, h));
		rc = kvdev_rados_nkvx_wasm_module_insert(h, copy, wlen);
		/* A perturbed copy may or may not compile; only count successful inserts. */
		(void)rc;
		free(copy);
	}

	/* Bounded regardless of how many compiled: never more than the cap. */
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_cache_count() <= 3);
	kvdev_rados_nkvx_wasm_get_stats(&st);
	/* If at least cap+1 inserts succeeded we must have evicted; assert bounded and
	 * that eviction is wired (mod_evictions advanced when overflow occurred). */
	if (kvdev_rados_nkvx_wasm_module_cache_count() == 3) {
		CU_ASSERT(st.mod_evictions > 0);
	}
	printf("\n    wwy module cache: attempted %d inserts, count=%llu (cap 3), "
	       "mod_evictions=%llu (bounded)\n", N,
	       (unsigned long long)kvdev_rados_nkvx_wasm_module_cache_count(),
	       (unsigned long long)st.mod_evictions);

	free(wasm);
	kvdev_rados_nkvx_wasm_module_cache_reset();
	kvdev_rados_nkvx_wasm_cache_reset();
done:
	unsetenv("SPDK_NKVX_MOD_CACHE_MAX_COUNT");
#else
	printf("\n    built --without-wasm: wwy module-cache test is a no-op\n");
#endif
}

/*
 * spdk-yc1: warm-instance STATE ISOLATION between Execs. The warm cache reuses the
 * expensive engine + compiled module across Execs of the same (module,object), but
 * each Exec now gets a FRESH store+instance, so wasm globals / declared data reset
 * between runs and module state does not leak.
 *
 * statefulglobal.wasm keeps a mutable counter (declared init 0): each run reports
 * the value it OBSERVED (before incrementing). Run it twice warm against the SAME
 * (module,object):
 *   - WITH per-Exec re-instantiation (the fix): run 2 observes 0 (clean instance) —
 *     even though it is a warm HIT (warm_hits increments, engine+module reused).
 *   - WITHOUT (instance reused, the old behaviour): run 2 would observe 1 (leak).
 *
 * Fail-before/after: revert nkvx_run_on_object_locked to caching+reusing the store/
 * instance and run 2 observes 1 -> the run2==0 assert fails. With the fix run 2
 * observes 0. The warm_hits assert proves the engine/module are still reused (this
 * is genuine warm reuse with a reset instance, not a cold rebuild).
 */
static void
test_nkvx_yc1_warm_state_isolation(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "0", 1);

	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> yc1 state-isolation test skipped\n");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();

	static const uint8_t obj[] = "isolation-object";
	struct fake_fill fill = { .bytes = obj, .len = sizeof(obj), .calls = 0 };
	uint8_t out[64];
	uint32_t rlen = 0;
	uint64_t run1 = 0xdead, run2 = 0xdead;
	struct kvdev_rados_nkvx_wasm_stats st;

	/* ---- Run 1 (cold build): observes the declared-init counter (0). ---- */
	memset(out, 0, sizeof(out));
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_cached("statefulglobal", NULL, "isoK", sizeof(obj),
			fake_cold_fill, &fill, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(rlen == sizeof(uint64_t));
	memcpy(&run1, out, sizeof(run1));
	CU_ASSERT(run1 == 0);				/* fresh instance starts at 0 */

	/* ---- Run 2 (WARM hit, fresh instance): must ALSO observe 0. ---- */
	memset(out, 0, sizeof(out));
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_cached("statefulglobal", NULL, "isoK", sizeof(obj),
			fake_cold_fill, &fill, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	memcpy(&run2, out, sizeof(run2));

	kvdev_rados_nkvx_wasm_get_stats(&st);
	/* Load-bearing: run 2 did NOT see run 1's mutation -> state was reset. */
	CU_ASSERT(run2 == 0);
	CU_ASSERT(run2 == run1);
	/* Still a genuine WARM reuse of the engine+compiled module (not a cold rebuild):
	 * the 2nd Exec re-instantiated from the cached engine+module, not recompiled.
	 * (statefulglobal declares 2 pages, so on this small object it uses the private
	 * fallback rather than the zero-copy alias — the zero-copy path is proven
	 * elsewhere; here the load-bearing property is the per-Exec state RESET.) */
	CU_ASSERT(st.warm_hits == 1);
	CU_ASSERT(fill.calls == 1);			/* object cold-filled once */
	printf("\n    yc1 state isolation: run1=%llu run2=%llu (reset, no leak); "
	       "warm_hits=%llu (engine+module reused)\n",
	       (unsigned long long)run1, (unsigned long long)run2,
	       (unsigned long long)st.warm_hits);

	kvdev_rados_nkvx_wasm_cache_reset();
	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: yc1 state-isolation test is a no-op\n");
#endif
}

/*
 * spdk-0k1: the epoch ticker thread has a teardown hook and is JOINED on executor
 * stop -- no 1-thread leak across a stop/restart, no double-start, and the warm
 * epoch arming still works after a restart.
 *
 * The ticker is created lazily on the first epoch-armed run. We drive a real wasm
 * run with the epoch cap ENABLED (a short tick budget) so the ticker starts, then
 * tear the executor down and assert the ticker has been joined (g_epoch.started is
 * cleared and the thread exited). We then RESTART and arm again to prove the lazy
 * restart works (the warm-path epoch arming survives a stop/restart). When the wasm
 * runtime is unavailable the run can't arm the ticker, so the test asserts the
 * benign "never started -> teardown is a no-op" invariant instead.
 *
 * Fail-before: without nkvx_wasm_epoch_stop the ticker thread is never joined, so
 * g_epoch.started stays true after teardown (and the thread leaks); this test's
 * post-teardown assert fails.
 */
static void
test_nkvx_epoch_ticker_teardown(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	struct fake_fill fill;
	static const uint8_t obj[] = "x";
	uint8_t out[16];
	uint32_t rlen = 0;

	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	/* Epoch ENABLED with a generous budget so a normal module completes but the
	 * ticker is armed (and thus lazily started). Fuel generous too. */
	setenv("SPDK_NKVX_WASM_FUEL", "100000000", 1);
	setenv("SPDK_NKVX_WASM_EPOCH_TICKS", "100", 1);
	unsetenv("SPDK_NKVX_WASM_MAX_MEMORY");

	if (!nkvx_wasm_runtime_available()) {
		/* No runtime -> the ticker is never armed/started. Teardown must still be
		 * a safe no-op and leave started==false. */
		CU_ASSERT(kvdev_rados_nkvx_start() == 0);
		kvdev_rados_nkvx_stop();
		CU_ASSERT(g_epoch.started == false);
		printf("\n    epoch-ticker teardown: runtime unavailable -> ticker never started "
		       "(teardown no-op, ok)\n");
		unsetenv("SPDK_NKVX_WASM_FUEL");
		unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
		return;
	}
	kvdev_rados_nkvx_wasm_cache_reset();

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

	/* An epoch-armed run starts the ticker lazily. */
	fill = (struct fake_fill){ .bytes = obj, .len = sizeof(obj), .calls = 0 };
	memset(out, 0, sizeof(out));
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, "tickK", sizeof(obj),
			fake_cold_fill, &fill, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_epoch.started == true);	/* ticker is up */

	/* Teardown JOINS the ticker (no leak) and clears started. */
	kvdev_rados_nkvx_wasm_cache_reset();
	kvdev_rados_nkvx_stop();
	CU_ASSERT(g_epoch.started == false);	/* joined -> no leaked ticker thread */
	CU_ASSERT(g_epoch.engine == NULL);
	printf("\n    epoch-ticker teardown: started after armed run, JOINED on stop "
	       "(started=%d, no leak)\n", g_epoch.started);

	/* RESTART: a fresh executor + a fresh armed run re-creates the ticker (no
	 * double-start crash; warm-path epoch arming still works post-restart). */
	CU_ASSERT(kvdev_rados_nkvx_start() == 0);
	fill = (struct fake_fill){ .bytes = obj, .len = sizeof(obj), .calls = 0 };
	memset(out, 0, sizeof(out));
	CU_ASSERT(kvdev_rados_nkvx_wasm_run_cached("checksum", NULL, "tickK2", sizeof(obj),
			fake_cold_fill, &fill, out, sizeof(out), &rlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(g_epoch.started == true);	/* lazily restarted */
	kvdev_rados_nkvx_wasm_cache_reset();
	kvdev_rados_nkvx_stop();
	CU_ASSERT(g_epoch.started == false);	/* joined again */
	printf("    epoch-ticker teardown: restart re-armed the ticker and re-joined it (ok)\n");

	unsetenv("SPDK_NKVX_WASM_FUEL");
	unsetenv("SPDK_NKVX_WASM_EPOCH_TICKS");
#else
	printf("\n    built --without-wasm: epoch-ticker teardown test is a no-op\n");
#endif
}

/* ==========================================================================
 * spdk-5wi: the off-reactor module COMPILE. These drive the NEW path the P2
 * change introduced — the reactor-side hash GATE (kvdev_rados_nkvx_wasm_module_verify),
 * the off-reactor compile dispatch (kvdev_rados_nkvx_dispatch_compile ->
 * NKVX_JOB_COMPILE -> kvdev_rados_nkvx_wasm_module_insert on the worker), and the
 * origin-thread hand-back. Without these, the green UT proved nothing about the
 * new concurrency/ownership path (adversarial review finding #2).
 *
 * NOTE ON THE CHANNEL-TEARDOWN UAF (finding #1): that race lives in kvdev_rados.c
 * (the librados channel/io path), which is NOT compiled into this UT (it pulls in
 * librados). The fix there is a held spdk_io_channel reference taken at
 * dispatch_compile time and released in module_compiled, so an in-flight compile
 * cannot outlive its channel. The directly unit-testable surface of the change —
 * exercised below — is the executor: the gate, the off-reactor compile, and the
 * worker-join-before-teardown ordering invariant that the kvdev_rados.c teardown
 * reasoning depends on.
 * ========================================================================== */

/* Compile-completion sink for kvdev_rados_nkvx_dispatch_compile. */
struct compile_result {
	bool		completed;
	int		kvstatus;
	pthread_t	complete_tid;	/* OS thread the completion fired on */
};

static void
compile_done(void *arg, int kvstatus)
{
	struct compile_result *cr = arg;

	cr->kvstatus = kvstatus;
	cr->complete_tid = pthread_self();
	cr->completed = true;
}

/*
 * spdk-5wi (gate): kvdev_rados_nkvx_wasm_module_verify is the reactor-callable,
 * COMPILE-FREE hash gate. A correct hash is blessed (SUCCESS) without caching
 * anything; a flipped hash/byte is rejected INVALID; with the runtime unavailable
 * (incl. --without-wasm) it is NOT_SUPPORTED. The gate must NOT populate the module
 * cache — only the off-reactor insert does.
 */
static void
test_nkvx_5wi_verify_gate_is_compile_free(void)
{
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	if (!nkvx_wasm_runtime_available()) {
		/* Runtime genuinely unavailable: the gate fails soft, never crashes. */
		uint8_t any[SPDK_KV_EXEC_SHA256_LEN] = {0};
		CU_ASSERT(kvdev_rados_nkvx_wasm_module_verify(any, "x", 1) ==
			  SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED);
		printf("\n    wasm runtime unavailable -> verify gate NOT_SUPPORTED (fail-soft, ok)\n");
		return;
	}
	kvdev_rados_nkvx_wasm_module_cache_reset();

	size_t wlen = 0;
	uint8_t *wasm = tb3_read_wasm("checksum", &wlen);
	uint8_t good[SPDK_KV_EXEC_SHA256_LEN], bad[SPDK_KV_EXEC_SHA256_LEN];

	if (wasm == NULL) {
		return;
	}
	tb3_sha256(wasm, wlen, good);

	/* Correct hash -> blessed, but the gate does NOT compile/cache. */
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_verify(good, wasm, wlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(!kvdev_rados_nkvx_wasm_module_cached(good));
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_cache_count() == 0);

	/* Flipped hash and flipped bytes are both rejected INVALID, never blessed. */
	memcpy(bad, good, sizeof(bad));
	bad[0] ^= 0x01;
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_verify(bad, wasm, wlen) ==
		  SPDK_KVDEV_IO_STATUS_INVALID);
	wasm[wlen / 2] ^= 0xFF;
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_verify(good, wasm, wlen) ==
		  SPDK_KVDEV_IO_STATUS_INVALID);
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_cache_count() == 0);	/* still nothing cached */
	printf("\n    5wi verify gate: correct hash -> SUCCESS (no cache); flipped -> INVALID; "
	       "cache stays empty (compile-free)\n");

	free(wasm);
	kvdev_rados_nkvx_wasm_module_cache_reset();
#else
	uint8_t any[SPDK_KV_EXEC_SHA256_LEN] = {0};
	/* --without-wasm: the gate stub must report NOT_SUPPORTED, never crash. */
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_verify(any, "x", 1) ==
		  SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED);
	printf("\n    built --without-wasm: 5wi verify gate is NOT_SUPPORTED stub (ok)\n");
#endif
}

/*
 * spdk-5wi (off-reactor compile): kvdev_rados_nkvx_dispatch_compile hands a VERIFIED
 * module to the executor worker (NKVX_JOB_COMPILE), which compiles + caches it, and
 * the completion fires back ON the originating SPDK thread — exactly like the run
 * dispatch. Proves: (a) the compile actually runs and caches the artifact, (b) the
 * completion lands on the SPDK thread (not the worker), and (c) a second dispatch of
 * the same hash is idempotent (the insert is self-synchronized under the modcache
 * mutex). Bad-args and no-thread rejections are checked too.
 */
static void
test_nkvx_5wi_dispatch_compile_offreactor(void)
{
	uint8_t junk[SPDK_KV_EXEC_SHA256_LEN] = {0};
	struct compile_result cr;
	pthread_t main_tid = pthread_self();
	int rc;

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

	/* Argument validation (no thread / NULLs) — same contract as the run dispatch. */
	set_thread(INVALID_THREAD);
	rc = kvdev_rados_nkvx_dispatch_compile(junk, "x", 1, compile_done, &cr);
	CU_ASSERT(rc == -EINVAL);	/* off any SPDK thread: no origin to complete on */
	set_thread(0);
	rc = kvdev_rados_nkvx_dispatch_compile(junk, NULL, 1, compile_done, &cr);
	CU_ASSERT(rc == -EINVAL);
	rc = kvdev_rados_nkvx_dispatch_compile(junk, "x", 0, compile_done, &cr);
	CU_ASSERT(rc == -EINVAL);
	set_thread(INVALID_THREAD);

#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	if (!nkvx_wasm_runtime_available()) {
		printf("\n    wasm runtime unavailable -> 5wi dispatch_compile run skipped "
		       "(arg-validation still covered)\n");
		kvdev_rados_nkvx_stop();
		return;
	}
	kvdev_rados_nkvx_wasm_module_cache_reset();

	size_t wlen = 0;
	uint8_t *wasm = tb3_read_wasm("checksum", &wlen);
	uint8_t good[SPDK_KV_EXEC_SHA256_LEN];

	if (wasm == NULL) {
		kvdev_rados_nkvx_stop();
		return;
	}
	tb3_sha256(wasm, wlen, good);

	/* The reactor-side gate blesses the bytes, THEN we hand the compile off. */
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_verify(good, wasm, wlen) ==
		  SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(!kvdev_rados_nkvx_wasm_module_cached(good));	/* gate cached nothing */

	memset(&cr, 0, sizeof(cr));
	set_thread(0);
	rc = kvdev_rados_nkvx_dispatch_compile(good, wasm, wlen, compile_done, &cr);
	CU_ASSERT(rc == 0);
	set_thread(INVALID_THREAD);

	/* Pump the SPDK thread until the worker hands the compile completion back. */
	for (int i = 0; i < 100000 && !cr.completed; i++) {
		poll_threads();
		if (!cr.completed) {
			usleep(100);
		}
	}
	CU_ASSERT(cr.completed);
	CU_ASSERT(cr.kvstatus == SPDK_KVDEV_IO_STATUS_SUCCESS);
	/* The artifact is now cached -> the compile really ran on the worker. */
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_cached(good));
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_cache_count() == 1);
	/* OFF-REACTOR proof: completion fired on the SPDK thread, compile body on the
	 * worker (a different OS thread). */
	CU_ASSERT(pthread_equal(cr.complete_tid, main_tid));
	CU_ASSERT(!pthread_equal(g_nkvx.tid, main_tid));
	printf("\n    5wi off-reactor compile: dispatched, worker compiled+cached, completion on "
	       "SPDK thread (worker_tid=0x%lx != spdk_tid=0x%lx)\n",
	       (unsigned long)g_nkvx.tid, (unsigned long)main_tid);

	/* Idempotent: a SECOND compile of the same hash is safe (modcache mutex), still
	 * exactly one cache entry. */
	memset(&cr, 0, sizeof(cr));
	set_thread(0);
	rc = kvdev_rados_nkvx_dispatch_compile(good, wasm, wlen, compile_done, &cr);
	CU_ASSERT(rc == 0);
	set_thread(INVALID_THREAD);
	for (int i = 0; i < 100000 && !cr.completed; i++) {
		poll_threads();
		if (!cr.completed) {
			usleep(100);
		}
	}
	CU_ASSERT(cr.completed);
	CU_ASSERT(cr.kvstatus == SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(kvdev_rados_nkvx_wasm_module_cache_count() == 1);

	free(wasm);
	kvdev_rados_nkvx_wasm_module_cache_reset();
#else
	printf("\n    built --without-wasm: 5wi compile-run is a no-op (arg-validation covered)\n");
#endif
	kvdev_rados_nkvx_stop();
}

/*
 * spdk-5wi (module_fini ordering invariant). The kvdev_rados.c teardown reasoning
 * for the GLOBAL-shutdown path depends on kvdev_rados_nkvx_stop() JOINING the worker
 * before any kvdev/channel is destroyed: every dispatched compile has run AND queued
 * its completion message to the origin thread before teardown proceeds. This drives
 * that invariant directly: dispatch compiles, then stop WITHOUT first pumping the
 * completions, and assert that after stop() returns the worker is joined (drained)
 * and the queued completions are still deliverable to the SPDK thread (no compile is
 * lost, none runs after the join). With the runtime unavailable the same ordering is
 * exercised with NOT_SUPPORTED-returning inserts.
 */
static void
test_nkvx_5wi_stop_drains_inflight_compiles(void)
{
	struct compile_result cr[4];
	uint8_t hashes[4][SPDK_KV_EXEC_SHA256_LEN];
	size_t wlen = 0;
	uint8_t *wasm = NULL;
	int i;

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	setenv(KVDEV_RADOS_NKVX_WASM_DIR_ENV, NKVX_UT_WASM_DIR, 1);
	if (nkvx_wasm_runtime_available()) {
		kvdev_rados_nkvx_wasm_module_cache_reset();
		wasm = tb3_read_wasm("checksum", &wlen);
	}
#endif

	/* Dispatch several compiles. Use the real verified hash when we have the wasm,
	 * else a junk hash (insert returns NOT_SUPPORTED but the JOB still flows through
	 * the worker and back — which is what the ordering test cares about). */
	for (i = 0; i < 4; i++) {
		memset(&cr[i], 0, sizeof(cr[i]));
#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
		if (wasm != NULL) {
			tb3_sha256(wasm, wlen, hashes[i]);
		} else
#endif
		{
			memset(hashes[i], (uint8_t)(i + 1), sizeof(hashes[i]));
		}
		set_thread(0);
		CU_ASSERT(kvdev_rados_nkvx_dispatch_compile(hashes[i],
				wasm != NULL ? (const void *)wasm : (const void *)"x",
				wasm != NULL ? wlen : 1,
				compile_done, &cr[i]) == 0);
		set_thread(INVALID_THREAD);
	}

	/* STOP without draining first. stop() must JOIN the worker, which only exits
	 * once its queue is EMPTY (kvdev_rados_nkvx_worker_main loops until stop && empty),
	 * so every compile has been processed and its completion message enqueued to the
	 * origin SPDK thread BEFORE stop() returns. This is the exact ordering
	 * module_fini relies on: no compile can still be running once teardown proceeds. */
	kvdev_rados_nkvx_stop();
	CU_ASSERT(!g_nkvx.running);

	/* The completions were queued to the SPDK thread before the join; they are still
	 * pending and deliver safely now (no compile ran after the join, none was lost). */
	for (i = 0; i < 100000; i++) {
		bool all = true;
		int j;

		poll_threads();
		for (j = 0; j < 4; j++) {
			if (!cr[j].completed) {
				all = false;
			}
		}
		if (all) {
			break;
		}
		usleep(100);
	}
	for (i = 0; i < 4; i++) {
		CU_ASSERT(cr[i].completed);
		/* Completion fired on the SPDK (main) thread, never the (now-joined) worker. */
		CU_ASSERT(pthread_equal(cr[i].complete_tid, pthread_self()));
	}
	printf("\n    5wi module_fini ordering: stop() joined the worker AFTER draining %d queued "
	       "compiles; all completions delivered on the SPDK thread post-join\n", 4);

#if defined(SPDK_CONFIG_WASM) && defined(NKVX_UT_WASM_DIR)
	free(wasm);
	kvdev_rados_nkvx_wasm_module_cache_reset();
#endif
}

static void
test_nkvx_dispatch_without_thread_fails(void)
{
	uint8_t out[8];
	int rc;

	CU_ASSERT(kvdev_rados_nkvx_start() == 0);

	/* Off any SPDK thread there is no origin to complete on -> -EINVAL. */
	set_thread(INVALID_THREAD);
	rc = kvdev_rados_nkvx_dispatch(KVDEV_RADOS_NKVX_MODULE_IDENTITY, NULL, NULL, NULL, "x", 1,
				       out, sizeof(out), nkvx_done, NULL);
	CU_ASSERT(rc == -EINVAL);

	kvdev_rados_nkvx_stop();
}

/* ==========================================================================
 * spdk-k3z: cross-consumer RESULT cache.
 *
 * These tests exercise the executor's result-cache API directly (the same
 * key/lookup/insert the reactor-side datapath calls in kvdev_rados.c, just driven
 * without librados). They prove the acceptance criteria DETERMINISTICALLY:
 *   - cross-consumer hit: an IDENTICAL (module, object content, input) request
 *     misses once (compute) then hits (no recompute); a measured hit rate is
 *     reported;
 *   - a DIFFERENT input  -> MISS (no stale hit);
 *   - a CHANGED OBJECT (different content under the SAME oid) -> MISS / no stale;
 *   - the DISABLED knob path -> lookup always misses (recompute), insert no-ops,
 *     and the recomputed answer is identical.
 *
 * "Compute" here is the bytecount built-in (object length as LE u64): a pure,
 * deterministic function of the object bytes, so a cache hit MUST reproduce the
 * recompute byte-for-byte.
 * ========================================================================== */

/* Recompute the bytecount built-in's answer for (object) into out[8]; returns the
 * true result length and status, exactly as a real off-reactor run would. */
static int
nkvx_recompute_bytecount(const void *object, size_t object_len,
			 void *out, uint32_t out_len, uint32_t *result_len)
{
	return kvdev_rados_nkvx_run_module(KVDEV_RADOS_NKVX_MODULE_BYTECOUNT, NULL,
					   NULL, NULL, object, object_len,
					   out, out_len, result_len);
}

/*
 * The cross-consumer flow the datapath runs: build the key from the object
 * CONTENT + input; on a lookup MISS recompute and insert; on a HIT serve the
 * memoized bytes. Returns the served (status,len,bytes) and whether it was a hit.
 */
static bool
nkvx_result_consume(const char *module, const uint8_t *mod_sha256,
		    const void *object, size_t object_len,
		    const void *input, size_t input_len,
		    uint8_t out[8], uint32_t *out_len, int *status)
{
	struct kvdev_rados_nkvx_result_key key;
	bool hit;

	CU_ASSERT(kvdev_rados_nkvx_result_key(module, mod_sha256, object, object_len,
					      input, input_len, &key) == 0);

	hit = kvdev_rados_nkvx_result_lookup(&key, out, 8, out_len, status);
	if (!hit) {
		*status = nkvx_recompute_bytecount(object, object_len, out, 8, out_len);
		kvdev_rados_nkvx_result_insert(&key, *status, *out_len, out, 8);
	}
	return hit;
}

/* Cross-consumer HIT + correctness: same (module,object,input) from two distinct
 * "consumers" -> 1st miss/compute, 2nd hit/no-recompute, identical answer. */
static void
test_nkvx_result_cache_cross_consumer_hit(void)
{
	static const uint8_t obj[] = "the-object-bytes-1234567890";
	static const uint8_t input[] = "req-input-A";
	uint8_t a[8] = {0}, b[8] = {0};
	uint32_t alen = 0, blen = 0;
	int astatus = 0, bstatus = 0;
	struct kvdev_rados_nkvx_result_stats st;
	bool hit_a, hit_b;

	setenv(KVDEV_RADOS_NKVX_RESULT_CACHE_ENV, "1", 1);
	kvdev_rados_nkvx_result_cache_reset();
	CU_ASSERT(kvdev_rados_nkvx_result_cache_enabled());

	/* Consumer #1: MISS -> compute + insert. */
	hit_a = nkvx_result_consume(KVDEV_RADOS_NKVX_MODULE_BYTECOUNT, NULL,
				    obj, sizeof(obj), input, sizeof(input),
				    a, &alen, &astatus);
	CU_ASSERT(!hit_a);
	CU_ASSERT(astatus == SPDK_KVDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(alen == 8);
	CU_ASSERT(*(uint64_t *)a == (uint64_t)sizeof(obj));	/* bytecount correctness */

	/* Consumer #2 (DISTINCT consumer, same request): HIT -> served WITHOUT
	 * recompute. The memoized answer is byte-identical to consumer #1's. */
	hit_b = nkvx_result_consume(KVDEV_RADOS_NKVX_MODULE_BYTECOUNT, NULL,
				    obj, sizeof(obj), input, sizeof(input),
				    b, &blen, &bstatus);
	CU_ASSERT(hit_b);
	CU_ASSERT(bstatus == astatus);
	CU_ASSERT(blen == alen);
	CU_ASSERT(memcmp(a, b, 8) == 0);

	kvdev_rados_nkvx_result_get_stats(&st);
	CU_ASSERT(st.hits == 1);
	CU_ASSERT(st.misses == 1);
	CU_ASSERT(st.inserts == 1);
	printf("\n    k3z cross-consumer: 2 identical requests from distinct consumers -> "
	       "1 miss/compute + 1 HIT/no-recompute; hit-rate=%.0f%% (hits=%" PRIu64
	       " misses=%" PRIu64 ")\n",
	       100.0 * (double)st.hits / (double)(st.hits + st.misses), st.hits, st.misses);

	unsetenv(KVDEV_RADOS_NKVX_RESULT_CACHE_ENV);
	kvdev_rados_nkvx_result_cache_reset();
}

/* A DIFFERENT input is a genuine MISS (no stale hit), and yields its own entry. */
static void
test_nkvx_result_cache_different_input_misses(void)
{
	static const uint8_t obj[] = "shared-object-bytes";
	uint8_t out[8] = {0};
	uint32_t len = 0;
	int status = 0;
	struct kvdev_rados_nkvx_result_stats st;

	setenv(KVDEV_RADOS_NKVX_RESULT_CACHE_ENV, "1", 1);
	kvdev_rados_nkvx_result_cache_reset();

	/* Prime with input A. */
	CU_ASSERT(!nkvx_result_consume(KVDEV_RADOS_NKVX_MODULE_BYTECOUNT, NULL,
				       obj, sizeof(obj), "AAAA", 4, out, &len, &status));
	/* Same object, DIFFERENT input -> MISS (logically different request). */
	CU_ASSERT(!nkvx_result_consume(KVDEV_RADOS_NKVX_MODULE_BYTECOUNT, NULL,
				       obj, sizeof(obj), "BBBB", 4, out, &len, &status));
	/* Re-issue input A -> now a HIT (proves A was cached and B did not clobber it). */
	CU_ASSERT(nkvx_result_consume(KVDEV_RADOS_NKVX_MODULE_BYTECOUNT, NULL,
				      obj, sizeof(obj), "AAAA", 4, out, &len, &status));

	kvdev_rados_nkvx_result_get_stats(&st);
	CU_ASSERT(st.misses == 2);	/* A, then B */
	CU_ASSERT(st.hits == 1);	/* A re-issue */
	CU_ASSERT(st.inserts == 2);
	printf("\n    k3z different-input: input A primed, input B MISSED (not a stale A hit), "
	       "A re-issue HIT\n");

	unsetenv(KVDEV_RADOS_NKVX_RESULT_CACHE_ENV);
	kvdev_rados_nkvx_result_cache_reset();
}

/*
 * THE adversarial-reviewer case: the SAME oid is overwritten with DIFFERENT
 * CONTENT. Because the key uses object CONTENT identity (not the bare oid), the
 * second request MISSES and recomputes — it never returns the stale first result.
 * This is what makes the result cache coherent with an in-place Store.
 */
static void
test_nkvx_result_cache_changed_object_no_stale(void)
{
	static const uint8_t v1[] = "version-one";			/* len 12 incl NUL */
	static const uint8_t v2[] = "version-two-is-longer-here";	/* different content+len */
	static const uint8_t input[] = "x";
	uint8_t out1[8] = {0}, out2[8] = {0};
	uint32_t len1 = 0, len2 = 0;
	int s1 = 0, s2 = 0;
	struct kvdev_rados_nkvx_result_stats st;

	setenv(KVDEV_RADOS_NKVX_RESULT_CACHE_ENV, "1", 1);
	kvdev_rados_nkvx_result_cache_reset();

	/* Compute over v1, cache it. */
	CU_ASSERT(!nkvx_result_consume(KVDEV_RADOS_NKVX_MODULE_BYTECOUNT, NULL,
				       v1, sizeof(v1), input, sizeof(input),
				       out1, &len1, &s1));
	CU_ASSERT(*(uint64_t *)out1 == (uint64_t)sizeof(v1));

	/* Same oid in the datapath, but the CONTENT changed (v2). The key is over the
	 * content, so this MISSES and recomputes the v2 answer — NOT the stale v1. */
	CU_ASSERT(!nkvx_result_consume(KVDEV_RADOS_NKVX_MODULE_BYTECOUNT, NULL,
				       v2, sizeof(v2), input, sizeof(input),
				       out2, &len2, &s2));
	CU_ASSERT(*(uint64_t *)out2 == (uint64_t)sizeof(v2));
	CU_ASSERT(memcmp(out1, out2, 8) != 0);	/* distinct answers, no stale hit */

	kvdev_rados_nkvx_result_get_stats(&st);
	CU_ASSERT(st.misses == 2);
	CU_ASSERT(st.hits == 0);
	printf("\n    k3z changed-object: same oid, content v1->v2 -> 2nd request MISSED and "
	       "recomputed (no stale v1 result)\n");

	unsetenv(KVDEV_RADOS_NKVX_RESULT_CACHE_ENV);
	kvdev_rados_nkvx_result_cache_reset();
}

/* The DISABLED knob: lookup always misses, insert is a no-op, and the recomputed
 * answer is identical to the enabled path (correctness preserved, just no memo). */
static void
test_nkvx_result_cache_disabled_recomputes(void)
{
	static const uint8_t obj[] = "disabled-knob-object";
	static const uint8_t input[] = "in";
	uint8_t en[8] = {0}, dis1[8] = {0}, dis2[8] = {0};
	uint32_t lenE = 0, lenD1 = 0, lenD2 = 0;
	int sE = 0, sD1 = 0, sD2 = 0;
	struct kvdev_rados_nkvx_result_stats st;

	/* Reference answer with the cache ENABLED. */
	setenv(KVDEV_RADOS_NKVX_RESULT_CACHE_ENV, "1", 1);
	kvdev_rados_nkvx_result_cache_reset();
	nkvx_result_consume(KVDEV_RADOS_NKVX_MODULE_BYTECOUNT, NULL, obj, sizeof(obj),
			    input, sizeof(input), en, &lenE, &sE);

	/* Now DISABLE and issue the SAME request twice. */
	setenv(KVDEV_RADOS_NKVX_RESULT_CACHE_ENV, "0", 1);
	kvdev_rados_nkvx_result_cache_reset();
	CU_ASSERT(!kvdev_rados_nkvx_result_cache_enabled());

	CU_ASSERT(!nkvx_result_consume(KVDEV_RADOS_NKVX_MODULE_BYTECOUNT, NULL,
				       obj, sizeof(obj), input, sizeof(input),
				       dis1, &lenD1, &sD1));
	/* Even a SECOND identical request misses (disabled => nothing memoized). */
	CU_ASSERT(!nkvx_result_consume(KVDEV_RADOS_NKVX_MODULE_BYTECOUNT, NULL,
				       obj, sizeof(obj), input, sizeof(input),
				       dis2, &lenD2, &sD2));

	/* Recomputed answers identical to the enabled path. */
	CU_ASSERT(sD1 == sE && sD2 == sE);
	CU_ASSERT(lenD1 == lenE && lenD2 == lenE);
	CU_ASSERT(memcmp(dis1, en, 8) == 0 && memcmp(dis2, en, 8) == 0);

	kvdev_rados_nkvx_result_get_stats(&st);
	CU_ASSERT(st.inserts == 0);	/* disabled => never memoized */
	CU_ASSERT(st.hits == 0);
	printf("\n    k3z disabled-knob: SPDK_NKVX_RESULT_CACHE=0 -> 2 identical requests both "
	       "recomputed (inserts=0, hits=0), answers identical to enabled path\n");

	unsetenv(KVDEV_RADOS_NKVX_RESULT_CACHE_ENV);
	kvdev_rados_nkvx_result_cache_reset();
}

/* Module IDENTITY is part of the key: a verified-wasm sha256 vs a built-in
 * (NULL sha256) of the same NAME must NOT alias, and two different sha256 must
 * not alias. Pure key-level proof (no run needed). */
static void
test_nkvx_result_cache_module_identity_in_key(void)
{
	static const uint8_t obj[] = "obj";
	uint8_t shaA[SPDK_KV_EXEC_SHA256_LEN];
	uint8_t shaB[SPDK_KV_EXEC_SHA256_LEN];
	struct kvdev_rados_nkvx_result_key k_builtin, k_wasmA, k_wasmB;

	memset(shaA, 0xAA, sizeof(shaA));
	memset(shaB, 0xBB, sizeof(shaB));

	CU_ASSERT(kvdev_rados_nkvx_result_key("wasm:bytecount", NULL, obj, sizeof(obj),
					      NULL, 0, &k_builtin) == 0);
	CU_ASSERT(kvdev_rados_nkvx_result_key("wasm:bytecount", shaA, obj, sizeof(obj),
					      NULL, 0, &k_wasmA) == 0);
	CU_ASSERT(kvdev_rados_nkvx_result_key("wasm:bytecount", shaB, obj, sizeof(obj),
					      NULL, 0, &k_wasmB) == 0);

	/* built-in (no sha) != verified-wasm (sha), and different sha differ. */
	CU_ASSERT(memcmp(k_builtin.digest, k_wasmA.digest, SPDK_KV_EXEC_SHA256_LEN) != 0);
	CU_ASSERT(memcmp(k_wasmA.digest, k_wasmB.digest, SPDK_KV_EXEC_SHA256_LEN) != 0);
	printf("\n    k3z module-identity: built-in, wasm/shaA, wasm/shaB all key distinctly\n");
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("kvdev_rados_nkvx", NULL, NULL);

	CU_ADD_TEST(suite, test_nkvx_bytecount_offreactor);
	CU_ADD_TEST(suite, test_nkvx_identity);
	CU_ADD_TEST(suite, test_nkvx_unknown_module);
	CU_ADD_TEST(suite, test_nkvx_bytecount_buffer_too_small);
	CU_ADD_TEST(suite, test_nkvx_wasm_bytecount_offreactor);
	CU_ADD_TEST(suite, test_nkvx_wasm_fuel_runaway_aborted);
	CU_ADD_TEST(suite, test_nkvx_wasm_walltime_runaway_epoch_aborted);
	CU_ADD_TEST(suite, test_nkvx_wasm_overalloc_contained);
	CU_ADD_TEST(suite, test_nkvx_wasm_normal_within_caps);
	CU_ADD_TEST(suite, test_nkvx_tb4_cache_zerocopy_warm);
	CU_ADD_TEST(suite, test_nkvx_tb4_distinct_object_is_miss);
	CU_ADD_TEST(suite, test_nkvx_long_key_oid);
	CU_ADD_TEST(suite, test_nkvx_long_key_oid_distinct);
	CU_ADD_TEST(suite, test_nkvx_tb4_multipage_module_private);
	CU_ADD_TEST(suite, test_nkvx_tb4_oob_traps);
	CU_ADD_TEST(suite, test_nkvx_d1_declared_size_caps_slack);
	CU_ADD_TEST(suite, test_nkvx_d2_invalidate_on_mutation);
	CU_ADD_TEST(suite, test_nkvx_d2_pin_survives_invalidate);
	CU_ADD_TEST(suite, test_nkvx_wasm_write_capable_read_only_rejected);
	CU_ADD_TEST(suite, test_nkvx_d3_warm_memory_cap_contained);
	CU_ADD_TEST(suite, test_nkvx_store_limiter_bounds_plain_path);
	CU_ADD_TEST(suite, test_nkvx_d3_warm_epoch_cap_contained);
	CU_ADD_TEST(suite, test_nkvx_tb4_cached_failsoft);
	CU_ADD_TEST(suite, test_nkvx_tb4_dispatch_wires_cache);
	CU_ADD_TEST(suite, test_nkvx_tb3_hash_mismatch_rejected);
	CU_ADD_TEST(suite, test_nkvx_tb3_verify_run_and_module_cache_hit);
	CU_ADD_TEST(suite, test_nkvx_tb3_miss_without_bytes_fails_closed);
	CU_ADD_TEST(suite, test_nkvx_yc1_warm_state_isolation);
	CU_ADD_TEST(suite, test_nkvx_wwy_object_cache_bounded);
	CU_ADD_TEST(suite, test_nkvx_wwy_pinned_survives_eviction);
	CU_ADD_TEST(suite, test_nkvx_wwy_warm_cache_bounded);
	CU_ADD_TEST(suite, test_nkvx_wwy_module_cache_bounded);
	CU_ADD_TEST(suite, test_nkvx_epoch_ticker_teardown);
	CU_ADD_TEST(suite, test_nkvx_5wi_verify_gate_is_compile_free);
	CU_ADD_TEST(suite, test_nkvx_5wi_dispatch_compile_offreactor);
	CU_ADD_TEST(suite, test_nkvx_5wi_stop_drains_inflight_compiles);
	CU_ADD_TEST(suite, test_nkvx_dispatch_without_thread_fails);
	CU_ADD_TEST(suite, test_nkvx_result_cache_cross_consumer_hit);
	CU_ADD_TEST(suite, test_nkvx_result_cache_different_input_misses);
	CU_ADD_TEST(suite, test_nkvx_result_cache_changed_object_no_stale);
	CU_ADD_TEST(suite, test_nkvx_result_cache_disabled_recomputes);
	CU_ADD_TEST(suite, test_nkvx_result_cache_module_identity_in_key);

	/* One SPDK thread stands in for the reactor; the executor worker is a real
	 * separate pthread, so the off-reactor thread-id check is meaningful. */
	allocate_threads(1);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	free_threads();
	CU_cleanup_registry();
	return num_failures;
}
