/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/*
 * Dependency-free unit tests for the pure executor contract helpers in
 * nkvx_oid.h (ADR-0014: oid hex-encoding + result truncation). These compile and
 * run WITHOUT Mercury/librados/wasm — `make -f Makefile test` builds this with a
 * plain host compiler, so it runs anywhere (CI, a laptop) and is the fast inner
 * loop for the contract logic the e2e shell suite (`make check-integration`)
 * exercises end-to-end.
 *
 * Intentionally framework-free (bare assert + a tiny EXPECT helper), matching the
 * minimalist standalone Makefile. Add cases by appending a test_* function and a
 * call in main().
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nkvx_oid.h"

static int failures;

#define EXPECT(cond)							\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "  FAIL %s:%d: %s\n",		\
				__FILE__, __LINE__, #cond);		\
			failures++;					\
		}							\
	} while (0)

/* oid = lowercase hex of the key, NUL-terminated. The front
 * (kvdev_rados_key_to_oid) MUST produce the identical string. */
static void
test_key_to_oid_basic(void)
{
	char oid[2 * 16 + 1];

	/* Single byte, both nibbles. */
	uint8_t k1[] = { 0xAB };
	nkvx_key_to_oid(k1, sizeof(k1), oid);
	EXPECT(strcmp(oid, "ab") == 0);

	/* Low + high nibble ordering (high nibble first). */
	uint8_t k2[] = { 0x0F, 0xF0 };
	nkvx_key_to_oid(k2, sizeof(k2), oid);
	EXPECT(strcmp(oid, "0ff0") == 0);

	/* Every hex digit appears, ascending bytes. */
	uint8_t k3[] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
	nkvx_key_to_oid(k3, sizeof(k3), oid);
	EXPECT(strcmp(oid, "0123456789abcdef") == 0);
}

static void
test_key_to_oid_edges(void)
{
	char oid[2 * 255 + 1];

	/* Zero-length key -> empty, still NUL-terminated. */
	nkvx_key_to_oid("", 0, oid);
	EXPECT(oid[0] == '\0');

	/* All-zero bytes encode as "00..", not an early NUL. */
	uint8_t z[3] = { 0, 0, 0 };
	nkvx_key_to_oid(z, sizeof(z), oid);
	EXPECT(strcmp(oid, "000000") == 0);

	/* Full 16-byte (SPDK_KVDEV_KEY_MAX_LEN) key -> 32 hex chars + NUL. */
	uint8_t k16[16];
	memset(k16, 0xFF, sizeof(k16));
	nkvx_key_to_oid(k16, sizeof(k16), oid);
	EXPECT(strlen(oid) == 32);
	EXPECT(strspn(oid, "f") == 32);

	/* Max exec key (SPDK_KVDEV_EXEC_KEY_MAX_LEN = 255, the uint8_t ceiling)
	 * must terminate the loop, not wrap. */
	uint8_t k255[255];
	memset(k255, 0x5A, sizeof(k255));
	nkvx_key_to_oid(k255, sizeof(k255), oid);
	EXPECT(strlen(oid) == 510);
	EXPECT(oid[0] == '5' && oid[1] == 'a');
	EXPECT(oid[508] == '5' && oid[509] == 'a' && oid[510] == '\0');
}

/* min(result_len, osize) — the delivered byte count under the tenant cap. */
static void
test_deliver_len(void)
{
	EXPECT(nkvx_deliver_len(0, 0) == 0);
	EXPECT(nkvx_deliver_len(10, 100) == 10);	/* result fits */
	EXPECT(nkvx_deliver_len(100, 10) == 10);	/* truncated to cap */
	EXPECT(nkvx_deliver_len(50, 50) == 50);		/* exact fit, no truncation */
	EXPECT(nkvx_deliver_len(0, 50) == 0);		/* empty result */
	EXPECT(nkvx_deliver_len(50, 0) == 0);		/* zero cap delivers nothing */
	EXPECT(nkvx_deliver_len(UINT32_MAX, UINT32_MAX) == UINT32_MAX);
}

/* result_len > osize -> BUFFER_TOO_SMALL. Predicate takes uint64_t so the
 * identity built-in can pass an unclamped object size. */
static void
test_result_too_small(void)
{
	EXPECT(!nkvx_result_too_small(0, 0));
	EXPECT(!nkvx_result_too_small(10, 100));
	EXPECT(!nkvx_result_too_small(50, 50));		/* boundary: equal is NOT too small */
	EXPECT(nkvx_result_too_small(51, 50));
	EXPECT(nkvx_result_too_small(100, 0));

	/* Object larger than UINT32_MAX still overflows a uint32 cap even though the
	 * delivered length saturates at UINT32_MAX (identity-path semantics). */
	EXPECT(nkvx_result_too_small((uint64_t)UINT32_MAX + 1, UINT32_MAX));
	EXPECT(!nkvx_result_too_small(UINT32_MAX, UINT32_MAX));
}

int
main(void)
{
	test_key_to_oid_basic();
	test_key_to_oid_edges();
	test_deliver_len();
	test_result_too_small();

	if (failures) {
		fprintf(stderr, "nkvx_unit_test: %d assertion(s) FAILED\n", failures);
		return 1;
	}
	printf("nkvx_unit_test: all assertions passed\n");
	return 0;
}
