/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */
/*
 * NKVX wasm test module: slackwrite (sandbox-semantics regression, spdk-ii0 D1).
 *
 * Declares a single 64 KiB page of linear memory (--initial-memory=65536) and
 * writes one byte at offset 70000 -- PAST its own declared page (valid offsets
 * 0..65535), but WITHIN a larger (2-page) object backing the zero-copy
 * MemoryCreator may have allocated for a big object.
 *
 * This is the D1 defect: before the fix the custom linear memory reported the
 * FULL object backing as its size, so this write into the "slack" between the
 * module's declared minimum and the larger backing did NOT trap (an escape of
 * the module's declared bounds, contained to the object buffer but a violation
 * of normal-linear-memory semantics). After the fix the reported size is capped
 * to the module's declared, page-rounded minimum (one page), so this access is
 * out of bounds and MUST trap -> contained (FAILED/ABORTED).
 *
 * The companion in-bounds proof (a write/read below 65536 still succeeds and is
 * still zero-copy) is covered by checksum.wasm in the same test.
 *
 * Build (requires clang with the wasm32 target) -- see README.md:
 *   clang --target=wasm32 -nostdlib -O2 \
 *     -Wl,--no-entry -Wl,--export=slackwrite \
 *     -Wl,--initial-memory=65536 -Wl,--export-memory \
 *     -o slackwrite.wasm slackwrite.c
 */

__attribute__((export_name("slackwrite")))
int
slackwrite(int obj_off, int obj_len)
{
	(void)obj_off;
	(void)obj_len;

	/* Offset 70000 is past the single declared 64 KiB page (valid 0..65535)
	 * but inside a 2-page object backing. Must trap once the reported size is
	 * the module's declared minimum (spdk-ii0 D1). */
	volatile unsigned char *p = (volatile unsigned char *)70000u;
	*p = 0xCC;

	return 1; /* unreachable in a sound sandbox: the store above must trap */
}
