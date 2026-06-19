/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */
/*
 * NKVX wasm test module: growcap (spdk-90x — store-limiter cap proof).
 *
 * Unlike overalloc (which grows UNBOUNDED until something stops it), growcap
 * grows a FIXED, BOUNDED number of pages and then returns SUCCESS. This makes it
 * a clean fail-before/after probe for the wasmtime store memory LIMITER on the
 * NON-custom-memory plain run() path:
 *
 *   - WITH a tight store limiter (cap < the bounded target), one of the
 *     memory.grow calls FAILS (returns (unsigned)-1) before the target is
 *     reached; the module then deliberately traps (unreachable). The host
 *     surfaces a contained failure — the limiter is load-bearing.
 *   - WITHOUT the limiter the bounded grows all SUCCEED and the module returns
 *     SUCCESS (8-byte result). Because the growth is BOUNDED (GROW_PAGES), this
 *     never OOMs the target even with the cap disabled — so the fail-before is
 *     safe to run.
 *
 * GROW_PAGES * 64 KiB = the bounded ceiling (here 1024 pages = 64 MiB). A test
 * cap of ~1 MiB (16 pages) is far below it, so the grow fails fast under the cap.
 *
 * ABI matches the other modules: growcap(i32 obj_off, i32 obj_len) -> i32 res_len.
 */
typedef unsigned long long u64;

#define RES_OFF 8u
#define GROW_PAGES 1024u   /* bounded: 1024 * 64 KiB = 64 MiB ceiling */

__attribute__((export_name("growcap")))
unsigned int
growcap(unsigned int obj_off, unsigned int obj_len)
{
    (void)obj_off;
    (void)obj_len;

    for (unsigned int i = 0; i < GROW_PAGES; i++) {
        unsigned long prev = __builtin_wasm_memory_grow(0, 1);

        if (prev == (unsigned long)-1) {
            /* The store limiter refused this grow: contained. Trap so the host
             * surfaces a clean ABORTED/FAILED rather than a silent partial grow. */
            __builtin_trap();
        }
    }

    /* All bounded grows succeeded (no/looser cap): report success. */
    volatile u64 *res = (volatile u64 *)(unsigned long)RES_OFF;
    *res = (u64)GROW_PAGES;
    return 8u;
}
