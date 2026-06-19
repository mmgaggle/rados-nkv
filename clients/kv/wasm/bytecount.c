/*
 * Minimal NKVX wasm module: bytecount (TB-WIMP thinnest real-wasm slice).
 *
 * Contract:
 *   - Host copies the cold-filled object bytes into this module's exported
 *     linear memory at offset obj_off, then calls bytecount(obj_off, obj_len).
 *   - The module writes the object length as a little-endian u64 into linear
 *     memory at offset RES_OFF and returns the result length in bytes (8).
 *   - Host reads RES_OFF..RES_OFF+8 out of the exported linear memory.
 *
 * No WASI, no imports: a single exported memory + one exported function.
 * Result region is at a fixed NONZERO offset so the toolchain does not treat
 * the store as a null-pointer deref (which would compile to `unreachable`).
 */
typedef unsigned long long u64;

/* Result lives at offset 8 (nonzero); host knows this fixed offset. */
#define RES_OFF 8u

__attribute__((export_name("bytecount")))
unsigned int
bytecount(unsigned int obj_off, unsigned int obj_len)
{
    (void)obj_off;
    volatile u64 *res = (volatile u64 *)(unsigned long)RES_OFF;
    *res = (u64)obj_len;   /* wasm linear memory is little-endian */
    return 8u;             /* result length in bytes */
}
