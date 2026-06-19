/*
 * NKVX wasm test module: fuel_runaway (TB2 — per-invocation fuel cap).
 *
 * A tight, unbounded compute loop. With fuel-consumption enabled and a finite
 * per-invocation fuel ceiling, wasmtime debits fuel as instructions retire and
 * TRAPS ("all fuel consumed") once the budget is exhausted — BEFORE this loop
 * could ever return. The host then surfaces ABORTED (resource exhausted) and the
 * worker thread returns cleanly: the target never crashes or hangs.
 *
 * The loop is written so the optimizer cannot prove termination or elide it
 * (volatile accumulator, data-dependent condition that is always true).
 *
 * ABI matches bytecount.c: exported memory "memory" + exported function with the
 * module's name, signature (i32 obj_off, i32 obj_len) -> i32 result_len.
 */
typedef unsigned long long u64;

#define RES_OFF 8u

__attribute__((export_name("fuel_runaway")))
unsigned int
fuel_runaway(unsigned int obj_off, unsigned int obj_len)
{
    (void)obj_off;
    volatile u64 acc = 1;

    /* Burns fuel forever: each iteration retires instructions, so a finite fuel
     * ceiling is hit long before acc could wrap to 0 (it never does — acc|1 is
     * always nonzero). Fuel traps the module; this never returns. */
    while (acc | 1u) {
        acc = acc * 6364136223846793005ull + 1442695040888963407ull;
        acc ^= (u64)obj_len;
    }

    /* Unreachable: present only so the signature has a return path. */
    volatile u64 *res = (volatile u64 *)(unsigned long)RES_OFF;
    *res = acc;
    return 8u;
}
