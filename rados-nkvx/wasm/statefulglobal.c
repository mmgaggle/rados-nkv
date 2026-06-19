/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */
/*
 * NKVX wasm test module: statefulglobal (spdk-yc1 — warm-instance state isolation).
 *
 * Deliberately STATEFUL: it keeps a mutable module-level counter and, each run,
 * reports the value it observed BEFORE incrementing it. The counter lives in a
 * place that persists across runs ONLY if the warm-instance state is reused:
 *
 *   - `g_counter` is a mutable global with a declared init of 0. Re-instantiating
 *     the module (the spdk-yc1 reset) resets it to 0 every run; reusing the same
 *     instance across runs would let run 2 observe run 1's increment.
 *
 * Contract (same ABI as the other modules):
 *   - statefulglobal(i32 obj_off, i32 obj_len) -> i32 result_len (== 8)
 *   - writes the OBSERVED counter value (before increment) as a little-endian u64
 *     at RES_OFF, then increments the counter, and returns 8.
 *
 * Proof: run twice warm against the same (module,object).
 *   - WITH per-Exec re-instantiation (isolation): both runs observe 0.
 *   - WITHOUT (instance reused): run 1 observes 0, run 2 observes 1 (leak).
 */
typedef unsigned long long u64;

#define RES_OFF 8u

/* Mutable global, declared init 0. Re-instantiation resets it to 0. */
static unsigned long long g_counter = 0;

__attribute__((export_name("statefulglobal")))
unsigned int
statefulglobal(unsigned int obj_off, unsigned int obj_len)
{
    (void)obj_off;
    (void)obj_len;

    volatile u64 *res = (volatile u64 *)(unsigned long)RES_OFF;
    *res = (u64)g_counter;	/* report what we observed this run */
    g_counter++;		/* mutate state (would leak if the instance is reused) */
    return 8u;
}
