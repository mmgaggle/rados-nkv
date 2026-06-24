/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */
/*
 * NKVX wasm module: writer (read-only-executor guarantee regression, ADR-0008 D3).
 *
 * A WRITE-CAPABLE module: in addition to its entry function it exports the
 * reserved write-intent symbol `kv_write` (KVDEV_RADOS_NKVX_WASM_WRITE_EXPORT),
 * declaring that it intends to MUTATE the stored object. The executor's
 * read-only-executor gate (nkvx_run_on_object_locked) probes for this export and
 * rejects the run with READ_ONLY on a read-only namespace *before* the entry
 * function runs — so a write module can never mutate a read-only namespace.
 *
 * The v1 imports-free ABI cannot actually write back to RADOS, so the entry
 * function just computes the object length like bytecount; the LOAD-BEARING part
 * for the regression is the presence of the `kv_write` export, which the gate
 * keys on. On a writable namespace (read_only == false) the module runs normally
 * and returns the 8-byte length.
 *
 * No WASI, no imports: an exported memory + the `writer` entry + the `kv_write`
 * write-intent marker.
 */
typedef unsigned long long u64;

/* Result lives at offset 8 (nonzero); host knows this fixed offset. */
#define RES_OFF 8u

__attribute__((export_name("writer")))
unsigned int
writer(unsigned int obj_off, unsigned int obj_len)
{
	(void)obj_off;
	volatile u64 *res = (volatile u64 *)(unsigned long)RES_OFF;
	*res = (u64)obj_len;   /* wasm linear memory is little-endian */
	return 8u;             /* result length in bytes */
}

/*
 * The reserved write-intent marker. Its mere PRESENCE as an export declares the
 * module write-capable; the host never calls it (the v1 ABI has no write path).
 * The body is trivial and exists only so the symbol is a real exported function.
 */
__attribute__((export_name("kv_write")))
unsigned int
kv_write(void)
{
	return 0u;
}
