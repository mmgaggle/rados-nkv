/*
 * NKVX wasm module: checksum (TB4 zero-copy proof, spdk-ii0).
 *
 * Unlike bytecount (which only echoes the obj_len ARGUMENT), checksum READS the
 * object bytes out of linear memory at obj_off and folds them into a result. Its
 * answer is therefore wrong unless the object bytes are actually present in the
 * module's linear memory — which is exactly what the zero-copy MemoryCreator
 * backing must guarantee. A broken/empty backing yields the wrong checksum, so
 * this module makes the zero-copy path load-bearing for correctness.
 *
 * Contract (same ABI as the other modules):
 *   - checksum(i32 obj_off, i32 obj_len) -> i32 result_len (== 8)
 *   - reads obj_len bytes at obj_off from linear memory,
 *   - writes a little-endian u64 (sum of the bytes, + a length mix) at RES_OFF,
 *   - returns 8.
 */
typedef unsigned long long u64;
typedef unsigned char u8;

#define RES_OFF 8u

__attribute__((export_name("checksum")))
unsigned int
checksum(unsigned int obj_off, unsigned int obj_len)
{
    const volatile u8 *p = (const volatile u8 *)(unsigned long)obj_off;
    u64 sum = 0;
    unsigned int i;

    for (i = 0; i < obj_len; i++) {
        /* Position-weighted so byte order matters, not just the multiset. */
        sum += (u64)p[i] * (u64)(i + 1u);
    }
    /* Mix in the length so an empty backing (sum==0) is distinguishable. */
    sum ^= (u64)obj_len << 32;

    volatile u64 *res = (volatile u64 *)(unsigned long)RES_OFF;
    *res = sum;
    return 8u;
}
