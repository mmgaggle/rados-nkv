/*
 * oob.wasm -- adversarial out-of-bounds module for the WASM sandbox-escape
 * regression test (spdk-ii0 B1, ADR-0013). It declares a single 64 KiB page of
 * linear memory and then writes one byte at offset 65536 -- exactly one byte
 * PAST its own memory. A correctly sandboxed runtime MUST trap on this access.
 *
 * Before the fix, the zero-copy host linear memory was handed to wasmtime with
 * no guard region and the default static bounds-check elision, so this write
 * SILENTLY clobbered host heap (the escape). With reservation=0 / guard_size=0
 * forcing dynamic bounds checks, the access is caught and the run is contained
 * (trap -> FAILED/ABORTED), never a host-memory clobber.
 *
 * Build (requires clang with the wasm32 target) -- see README.md:
 *   clang --target=wasm32 -nostdlib -O2 \
 *     -Wl,--no-entry -Wl,--export=oob \
 *     -Wl,--initial-memory=65536 -Wl,--export-memory \
 *     -o oob.wasm oob.c
 */

__attribute__((export_name("oob")))
int
oob(int obj_off, int obj_len)
{
	(void)obj_off;
	(void)obj_len;

	/* One byte past the declared single 64 KiB page (valid offsets 0..65535). */
	volatile unsigned char *p = (volatile unsigned char *)65536u;
	*p = 0xCC;

	return 1; /* unreachable: the store above must trap in a sound sandbox */
}
