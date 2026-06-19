/*
 * NKVX wasm test module: walltime_runaway (TB2 — per-invocation EPOCH cap).
 *
 * A long-running compute loop used to exercise the WALL-CLOCK (epoch) cap
 * specifically — the backstop for what fuel under-counts. The test runs this
 * module with the fuel cap DISABLED (SPDK_NKVX_WASM_FUEL=0) so the ONLY thing
 * that can stop it is the epoch deadline that the background ticker thread
 * advances. With epoch interruption enabled and a finite epoch deadline, the
 * module is interrupted/trapped after its wall-clock budget elapses; the host
 * surfaces ABORTED and the worker returns cleanly. The target never hangs.
 *
 * The bound is astronomically large (effectively non-terminating within any
 * reasonable test window) so the epoch deadline — not loop completion — is what
 * stops it.
 *
 * ABI matches bytecount.c.
 */
typedef unsigned long long u64;

#define RES_OFF 8u

__attribute__((export_name("walltime_runaway")))
unsigned int
walltime_runaway(unsigned int obj_off, unsigned int obj_len)
{
    (void)obj_off;
    volatile u64 acc = 1;
    volatile u64 i = 0;

    /* ~1.8e19 iterations of cheap arithmetic: at any real CPU rate this runs for
     * many seconds, far beyond the ~100ms default epoch budget, so epoch
     * interruption fires first. */
    while (i < 0xFFFFFFFFFFFFFFFFull) {
        acc = acc * 2862933555777941757ull + 3037000493ull;
        acc ^= (u64)obj_len;
        i = i + 1;
    }

    volatile u64 *res = (volatile u64 *)(unsigned long)RES_OFF;
    *res = acc;
    return 8u;
}
