/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */
/*
 * NKVX wasm test module: pageprobe (sandbox-semantics regression, spdk-ii0 D1).
 *
 * Companion to slackwrite.c. Declares a single 64 KiB page of linear memory and
 * does an IN-BOUNDS write at offset 1000 (well within its own declared page),
 * then writes a recognizable marker u64 at the result offset and returns 8.
 *
 * Run against a LARGER (2-page) object backing, this proves that capping the
 * reported memory size to the module's declared minimum (spdk-ii0 D1) does NOT
 * break legitimate in-bounds access and does NOT break zero-copy: the write
 * succeeds and the module's linear memory still ALIASES the cached object buffer
 * (mem_base == cache_base). It is the in-bounds half of the D1 CASE-A proof;
 * slackwrite.wasm is the out-of-bounds (must-trap) half.
 *
 * Build (requires clang with the wasm32 target) -- see README.md:
 *   clang --target=wasm32 -nostdlib -O2 \
 *     -Wl,--no-entry -Wl,--export=pageprobe \
 *     -Wl,--initial-memory=65536 -Wl,--export-memory \
 *     -o pageprobe.wasm pageprobe.c
 */

typedef unsigned long long u64;

#define RES_OFF 8u

__attribute__((export_name("pageprobe")))
unsigned int
pageprobe(unsigned int obj_off, unsigned int obj_len)
{
	(void)obj_off;
	(void)obj_len;

	/* In-bounds write (offset 1000 < 65536): must succeed. */
	volatile unsigned char *p = (volatile unsigned char *)1000ul;
	*p = 0xAA;

	/* Recognizable marker so the host can confirm the run completed normally. */
	volatile u64 *res = (volatile u64 *)(unsigned long)RES_OFF;
	*res = 0x50524F424551ull; /* "PROBEQ" */
	return 8u;
}
