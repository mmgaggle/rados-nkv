/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * Pure, dependency-free executor contract helpers — the ADR-0014 logic (oid
 * encoding + result truncation) lifted out of nkvx_executor.c so it can be
 * unit-tested (nkvx_unit_test.c) WITHOUT linking librados/wasm/Mercury. The
 * executor and the test BOTH include this header, so the tested logic is the
 * production logic — there is no copy to drift (cf. the front's
 * kvdev_rados_key_to_oid, which this MUST match byte-for-byte).
 *
 * Header-only by design: every helper is a self-contained static inline over
 * <stdint.h>/<stdbool.h> only, so the unit test needs no SPDK/RADOS toolchain.
 */

#ifndef NKVX_OID_H
#define NKVX_OID_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Hex-encode a binary key into a NUL-terminated oid (mirrors
 * kvdev_rados_key_to_oid in kvdev_rados.h — the front and executor MUST encode
 * the oid identically, ADR-0014). \p oid must hold key_len*2 + 1 bytes.
 */
static inline void
nkvx_key_to_oid(const void *key, uint8_t key_len, char *oid)
{
	static const char hex[] = "0123456789abcdef";
	const uint8_t *k = (const uint8_t *)key;

	for (uint8_t i = 0; i < key_len; i++) {
		oid[i * 2]     = hex[k[i] >> 4];
		oid[i * 2 + 1] = hex[k[i] & 0xf];
	}
	oid[key_len * 2] = '\0';
}

/**
 * Bytes actually delivered for a result of true length \p result_len under the
 * tenant output cap \p osize: min(result_len, osize). The FULL result_len is
 * always reported separately (ADR-0014 truncation semantics).
 */
static inline uint32_t
nkvx_deliver_len(uint32_t result_len, uint32_t osize)
{
	return result_len < osize ? result_len : osize;
}

/**
 * Whether a result of true length \p result_len overflows the tenant cap
 * \p osize and must report BUFFER_TOO_SMALL (ADR-0014). Takes a uint64_t length
 * so callers can pass an unclamped object size (the identity built-in checks the
 * true object size, which may exceed UINT32_MAX, against osize).
 */
static inline bool
nkvx_result_too_small(uint64_t result_len, uint32_t osize)
{
	return result_len > osize;
}

#endif /* NKVX_OID_H */
