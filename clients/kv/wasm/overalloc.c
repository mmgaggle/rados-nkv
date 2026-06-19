/*
 * NKVX wasm test module: overalloc (TB2 — per-invocation linear-memory cap).
 *
 * Grows linear memory one 64 KiB page at a time, without bound. The host caps the
 * store's linear memory via the wasmtime store limiter, so memory.grow FAILS
 * (returns -1) once the module tries to grow past the cap — instead of the target
 * actually allocating gigabytes and OOMing. The module detects the failed grow
 * and traps (unreachable); the host surfaces a contained failure and the target
 * stays up.
 *
 * Without the cap this loop would grow until the host OOMs; WITH the cap it is
 * contained to at most the configured ceiling.
 *
 * ABI matches bytecount.c. memory.grow / memory.size are wasm built-ins.
 */
typedef unsigned long long u64;

#define RES_OFF 8u

__attribute__((export_name("overalloc")))
unsigned int
overalloc(unsigned int obj_off, unsigned int obj_len)
{
    (void)obj_off;
    (void)obj_len;

    /* Grow forever, one page per step. The store limiter makes memory_grow
     * return (unsigned)-1 once the cap is hit; we then deliberately trap so the
     * over-alloc is a clean, contained kill rather than silent truncation. */
    for (;;) {
        unsigned long prev = __builtin_wasm_memory_grow(0, 1);

        if (prev == (unsigned long)-1) {
            /* Cap reached: contained. Trap (unreachable). */
            __builtin_trap();
        }
        /* Touch the newly-grown page so a lazy backing store is forced to
         * commit, proving the cap is real and not just bookkeeping. */
        volatile unsigned char *p =
            (volatile unsigned char *)(prev * 65536ul);
        *p = (unsigned char)prev;
    }

    /* Unreachable. */
    return 0u;
}
