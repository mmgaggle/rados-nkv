/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/*
 * Inter-tier Exec RPC contract serializers + wire-status mapping (Slice C3).
 * See nkvx_exec_rpc.h and docs/design/slice-c-exec-rpc-mercury.md §1/§3.
 */

#include "nkvx_exec_rpc.h"

#include <stdlib.h>
#include <string.h>

#include "spdk/assert.h"

/*
 * ====================================================================
 * Wire-status mapping (design §3) — the review-mandated fix.
 *
 * The wire status is a fixed int32. We map the 10 known
 * `enum spdk_kvdev_io_status` codes <-> their int32 wire value, and pin EVERY
 * value with SPDK_STATIC_ASSERT (mirroring the size-assert at kvdev.h:246).
 * An enum-value bump in a future kvdev.h then breaks the BUILD here instead of
 * silently corrupting a tenant CQE. An unknown wire value -> FAILED.
 *
 * BUILD INVARIANT: the executor must be built against the identical kvdev.h as
 * the front (see the header comment). These asserts are the enforcement.
 *
 * NOTE (load-bearing): SPDK_STATIC_ASSERT (spdk/assert.h) expands to a real
 * static_assert only when <assert.h> defines `static_assert` as a macro — true
 * under C11/gnu11 (SPDK's -std=gnu11) but NOT under the compiler default C23,
 * where static_assert is a keyword and the macro is absent, so the gate silently
 * becomes a no-op. Build this contract (front AND the C2 executor) under gnu11
 * so these asserts actually fire.
 * ====================================================================
 */

SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_SUCCESS          ==  0, "wire status drift");
SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_FAILED           == -1, "wire status drift");
SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST    == -2, "wire status drift");
SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL == -3, "wire status drift");
SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_INVALID          == -4, "wire status drift");
SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_NOMEM            == -5, "wire status drift");
SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_KEY_EXIST        == -6, "wire status drift");
SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED    == -7, "wire status drift");
SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_ABORTED          == -8, "wire status drift");
SPDK_STATIC_ASSERT(SPDK_KVDEV_IO_STATUS_READ_ONLY        == -9, "wire status drift");

/* The wire status is exactly a 32-bit signed integer on the wire. */
SPDK_STATIC_ASSERT(sizeof(int32_t) == 4, "wire status must be a fixed int32");

/*
 * The public KV Exec dma-buf sink minimum (bead spdk-avu, R2) MUST match this
 * module-internal inline cap. lib/nvmf rejects a dma-buf sink with declared
 * length <= SPDK_KVDEV_DMABUF_SINK_MIN_LEN, and the front drops a sub-inline
 * result (registers no bulk for it). If the two ever diverge, a sink in the gap
 * would pass the upstream check yet be silently dropped here -- pin them
 * together at compile time.
 */
SPDK_STATIC_ASSERT(NKVX_INLINE_MAX == SPDK_KVDEV_DMABUF_SINK_MIN_LEN,
		   "dma-buf sink inline cap must match the public minimum");

/*
 * Cancel-ack enum (Slice C6b). The value is telemetry-only (the DELIVERED ack is
 * the safety proof), but pin it anyway for cross-build hygiene — mirroring the
 * wire-status block so a future reorder breaks the BUILD instead of silently
 * shuffling the executor's logged cancel-case. Fires only under gnu11 (see above).
 */
SPDK_STATIC_ASSERT(NKVX_CANCEL_ALREADY_DONE  == 0, "cancel-ack drift");
SPDK_STATIC_ASSERT(NKVX_CANCEL_ABORTED       == 1, "cancel-ack drift");
SPDK_STATIC_ASSERT(NKVX_CANCEL_PUSH_CANCELED == 2, "cancel-ack drift");

/*
 * KV-verb discriminant (slice N1, ADR-0008 D1/D4). Pin every value: a verb is
 * carried on the wire as a uint8 and routed by the executor to a mutating or
 * non-mutating path, so a silent renumber would mis-gate a security-relevant
 * decision (deny-by-default: anything not classified is treated as mutating).
 * A reorder must break the BUILD, not silently shift a verb's class. Fires only
 * under gnu11 (see the wire-status note above).
 */
SPDK_STATIC_ASSERT(NKVX_KV_VERB_INVALID  == 0, "kv-verb drift");
SPDK_STATIC_ASSERT(NKVX_KV_VERB_STORE    == 1, "kv-verb drift");
SPDK_STATIC_ASSERT(NKVX_KV_VERB_RETRIEVE == 2, "kv-verb drift");
SPDK_STATIC_ASSERT(NKVX_KV_VERB_DELETE   == 3, "kv-verb drift");
SPDK_STATIC_ASSERT(NKVX_KV_VERB_EXIST    == 4, "kv-verb drift");
SPDK_STATIC_ASSERT(NKVX_KV_VERB_LIST     == 5, "kv-verb drift");

int32_t
nkvx_status_to_wire(enum spdk_kvdev_io_status status)
{
	switch (status) {
	case SPDK_KVDEV_IO_STATUS_SUCCESS:
	case SPDK_KVDEV_IO_STATUS_FAILED:
	case SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST:
	case SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL:
	case SPDK_KVDEV_IO_STATUS_INVALID:
	case SPDK_KVDEV_IO_STATUS_NOMEM:
	case SPDK_KVDEV_IO_STATUS_KEY_EXIST:
	case SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED:
	case SPDK_KVDEV_IO_STATUS_ABORTED:
	case SPDK_KVDEV_IO_STATUS_READ_ONLY:
		return (int32_t)status;
	}
	/* An out-of-set enum value is itself a programming error; map to FAILED. */
	return (int32_t)SPDK_KVDEV_IO_STATUS_FAILED;
}

enum spdk_kvdev_io_status
nkvx_status_from_wire(int32_t wire)
{
	switch (wire) {
	case SPDK_KVDEV_IO_STATUS_SUCCESS:
		return SPDK_KVDEV_IO_STATUS_SUCCESS;
	case SPDK_KVDEV_IO_STATUS_FAILED:
		return SPDK_KVDEV_IO_STATUS_FAILED;
	case SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST:
		return SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST;
	case SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL:
		return SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL;
	case SPDK_KVDEV_IO_STATUS_INVALID:
		return SPDK_KVDEV_IO_STATUS_INVALID;
	case SPDK_KVDEV_IO_STATUS_NOMEM:
		return SPDK_KVDEV_IO_STATUS_NOMEM;
	case SPDK_KVDEV_IO_STATUS_KEY_EXIST:
		return SPDK_KVDEV_IO_STATUS_KEY_EXIST;
	case SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED:
		return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
	case SPDK_KVDEV_IO_STATUS_ABORTED:
		return SPDK_KVDEV_IO_STATUS_ABORTED;
	case SPDK_KVDEV_IO_STATUS_READ_ONLY:
		return SPDK_KVDEV_IO_STATUS_READ_ONLY;
	default:
		/*
		 * Unknown wire value: a transport-level fault, never cast through
		 * (design §3). The front synthesizes FAILED -> INTERNAL_DEVICE_ERROR.
		 */
		return SPDK_KVDEV_IO_STATUS_FAILED;
	}
}

/*
 * ====================================================================
 * hg_proc serializers (design §1, C3).
 *
 * Each field is processed explicitly (NOT a struct memcpy). On HG_DECODE the
 * string and opaque-byte fields are allocated; HG_FREE / the _free() helpers
 * release them. hg_proc_get_op(proc) tells us which phase we are in.
 * ====================================================================
 */

/* Encode/decode a NUL-terminated, possibly-NULL string. Mercury's
 * hg_proc_hg_string_t handles NULL strings and (de)allocation; on decode it
 * mallocs the string, which the HG_FREE path releases. */
static hg_return_t
nkvx_proc_string(hg_proc_t proc, char **sp)
{
	return hg_proc_hg_string_t(proc, sp);
}

/*
 * Encode/decode a length-prefixed opaque byte blob.
 *   wire: uint32 length, then `length` raw bytes.
 * On HG_DECODE *bufp is malloc'd (NULL when length == 0); HG_FREE frees it.
 */
static hg_return_t
nkvx_proc_opaque(hg_proc_t proc, void **bufp, uint32_t *lenp)
{
	hg_return_t ret;
	hg_proc_op_t op = hg_proc_get_op(proc);
	uint32_t len;

	if (op == HG_ENCODE) {
		len = *lenp;
	}
	ret = hg_proc_uint32_t(proc, &len);
	if (ret != HG_SUCCESS) {
		return ret;
	}

	switch (op) {
	case HG_ENCODE:
		if (len > 0) {
			ret = hg_proc_bytes(proc, *bufp, len);
		}
		break;
	case HG_DECODE:
		*lenp = len;
		if (len > 0) {
			*bufp = malloc(len);
			if (*bufp == NULL) {
				return HG_NOMEM;
			}
			ret = hg_proc_bytes(proc, *bufp, len);
			if (ret != HG_SUCCESS) {
				free(*bufp);
				*bufp = NULL;
			}
		} else {
			*bufp = NULL;
		}
		break;
	case HG_FREE:
		free(*bufp);
		*bufp = NULL;
		break;
	default:
		break;
	}
	return ret;
}

hg_return_t
hg_proc_nkvx_exec_in_t(hg_proc_t proc, void *data)
{
	nkvx_exec_in_t *in = data;
	hg_return_t ret;

	ret = hg_proc_uint32_t(proc, &in->op_id);
	if (ret != HG_SUCCESS) { return ret; }
	ret = hg_proc_uint64_t(proc, &in->client_call_id);
	if (ret != HG_SUCCESS) { return ret; }
	ret = hg_proc_uint8_t(proc, &in->read_only);
	if (ret != HG_SUCCESS) { return ret; }
	ret = hg_proc_uint8_t(proc, &in->runtime);
	if (ret != HG_SUCCESS) { return ret; }
	ret = hg_proc_uint64_t(proc, &in->caps);
	if (ret != HG_SUCCESS) { return ret; }

	/* key: uint8 length then exactly key_len raw bytes (fixed-cap struct field). */
	ret = hg_proc_uint8_t(proc, &in->key_len);
	if (ret != HG_SUCCESS) { return ret; }
	if (in->key_len > 0) {
		/* in->key is a fixed buffer in the struct; no alloc needed on decode. */
		ret = hg_proc_bytes(proc, in->key, in->key_len);
		if (ret != HG_SUCCESS) { return ret; }
	}

	/* sha256[32] fixed-width opaque + validity flag. */
	ret = hg_proc_bytes(proc, in->sha256, SPDK_KV_EXEC_SHA256_LEN);
	if (ret != HG_SUCCESS) { return ret; }
	ret = hg_proc_uint8_t(proc, &in->sha256_valid);
	if (ret != HG_SUCCESS) { return ret; }

	/* module locator strings. */
	ret = nkvx_proc_string(proc, &in->module_key);
	if (ret != HG_SUCCESS) { return ret; }
	ret = nkvx_proc_string(proc, &in->module_ns);
	if (ret != HG_SUCCESS) { return ret; }

	ret = hg_proc_uint32_t(proc, &in->osize);
	if (ret != HG_SUCCESS) { return ret; }

	/*
	 * Input: the TRUE input_len is always on the wire. The inline bytes are
	 * carried iff input_len <= NKVX_INLINE_MAX; otherwise input_inline is empty
	 * (length 0) on the wire and the bytes ride the input_bulk PULL handle.
	 */
	ret = hg_proc_uint32_t(proc, &in->input_len);
	if (ret != HG_SUCCESS) { return ret; }
	{
		void *buf = in->input_inline;
		uint32_t inline_len = (in->input_len <= NKVX_INLINE_MAX) ? in->input_len : 0;
		uint32_t decoded_len = inline_len;

		ret = nkvx_proc_opaque(proc, &buf, &decoded_len);
		if (ret != HG_SUCCESS) { return ret; }
		in->input_inline = buf;
	}

	/* Optional bulk handles (HG_BULK_NULL when absent; hg_proc handles that). */
	ret = hg_proc_hg_bulk_t(proc, &in->input_bulk);
	if (ret != HG_SUCCESS) { return ret; }
	ret = hg_proc_hg_bulk_t(proc, &in->result_sink);
	if (ret != HG_SUCCESS) { return ret; }

	return HG_SUCCESS;
}

hg_return_t
hg_proc_nkvx_exec_out_t(hg_proc_t proc, void *data)
{
	nkvx_exec_out_t *out = data;
	hg_return_t ret;

	/* Status is a fixed int32 on the wire (design §1.2/§3). */
	ret = hg_proc_int32_t(proc, &out->status);
	if (ret != HG_SUCCESS) { return ret; }

	/* TRUE result length (truncation semantics): always the full length. */
	ret = hg_proc_uint32_t(proc, &out->result_len);
	if (ret != HG_SUCCESS) { return ret; }

	/*
	 * Inline result bytes: present iff min(result_len, osize) <= NKVX_INLINE_MAX
	 * and no result_sink was used. The producer sets result_inline_len to the
	 * delivered length (0 when the result went via the WRITE-bulk sink). The
	 * length is carried explicitly so the decoder can recover it.
	 */
	{
		void *buf = out->result_inline;
		uint32_t len = out->result_inline_len;

		ret = nkvx_proc_opaque(proc, &buf, &len);
		if (ret != HG_SUCCESS) { return ret; }
		out->result_inline = buf;
		out->result_inline_len = len;
	}

	return HG_SUCCESS;
}

hg_return_t
hg_proc_nkvx_kv_in_t(hg_proc_t proc, void *data)
{
	nkvx_kv_in_t *in = data;
	hg_return_t ret;

	ret = hg_proc_uint8_t(proc, &in->verb);
	if (ret != HG_SUCCESS) { return ret; }
	ret = hg_proc_uint8_t(proc, &in->read_only);
	if (ret != HG_SUCCESS) { return ret; }
	ret = hg_proc_uint64_t(proc, &in->client_call_id);
	if (ret != HG_SUCCESS) { return ret; }

	/* key: uint8 length then exactly key_len raw bytes (fixed-cap struct field).
	 * key_len may be 0 for a List that starts at the very first key. */
	ret = hg_proc_uint8_t(proc, &in->key_len);
	if (ret != HG_SUCCESS) { return ret; }
	if (in->key_len > 0) {
		ret = hg_proc_bytes(proc, in->key, in->key_len);
		if (ret != HG_SUCCESS) { return ret; }
	}

	ret = hg_proc_uint32_t(proc, &in->osize);
	if (ret != HG_SUCCESS) { return ret; }

	/* Store-conditional options (SIKE/SINKE), slice S4. */
	ret = hg_proc_uint8_t(proc, &in->store_flags);
	if (ret != HG_SUCCESS) { return ret; }

	/*
	 * Store value: TRUE value_len always on the wire; the inline bytes are
	 * carried iff value_len <= NKVX_INLINE_MAX, else value_inline is empty
	 * (length 0) and the bytes ride the value_bulk PULL handle. Identical
	 * mechanics to nkvx_exec_in_t's input.
	 */
	ret = hg_proc_uint32_t(proc, &in->value_len);
	if (ret != HG_SUCCESS) { return ret; }
	{
		void *buf = in->value_inline;
		uint32_t inline_len = (in->value_len <= NKVX_INLINE_MAX) ? in->value_len : 0;
		uint32_t decoded_len = inline_len;

		ret = nkvx_proc_opaque(proc, &buf, &decoded_len);
		if (ret != HG_SUCCESS) { return ret; }
		in->value_inline = buf;
	}

	/* Optional bulk handles (HG_BULK_NULL when absent; hg_proc handles that). */
	ret = hg_proc_hg_bulk_t(proc, &in->value_bulk);
	if (ret != HG_SUCCESS) { return ret; }
	ret = hg_proc_hg_bulk_t(proc, &in->result_sink);
	if (ret != HG_SUCCESS) { return ret; }

	return HG_SUCCESS;
}

hg_return_t
hg_proc_nkvx_kv_out_t(hg_proc_t proc, void *data)
{
	nkvx_kv_out_t *out = data;
	hg_return_t ret;

	/* Status is a fixed int32 on the wire (same discipline as Exec, §3). */
	ret = hg_proc_int32_t(proc, &out->status);
	if (ret != HG_SUCCESS) { return ret; }

	/* TRUE result length (Retrieve/List truncation; Exist value length). */
	ret = hg_proc_uint32_t(proc, &out->result_len);
	if (ret != HG_SUCCESS) { return ret; }

	/* Inline result bytes: present iff min(result_len, osize) <= NKVX_INLINE_MAX
	 * and no result_sink was used. Length carried explicitly so the decoder
	 * recovers it. Mirrors nkvx_exec_out_t. */
	{
		void *buf = out->result_inline;
		uint32_t len = out->result_inline_len;

		ret = nkvx_proc_opaque(proc, &buf, &len);
		if (ret != HG_SUCCESS) { return ret; }
		out->result_inline = buf;
		out->result_inline_len = len;
	}

	return HG_SUCCESS;
}

hg_return_t
hg_proc_nkvx_cancel_in_t(hg_proc_t proc, void *data)
{
	nkvx_cancel_in_t *in = data;

	/* Only the call_id is on the wire; the origin address rides implicitly via
	 * HG_Get_info(handle)->addr on the executor side (design §C6b). */
	return hg_proc_uint64_t(proc, &in->call_id);
}

hg_return_t
hg_proc_nkvx_cancel_out_t(hg_proc_t proc, void *data)
{
	nkvx_cancel_out_t *out = data;

	/* The ack is a fixed int32 (one of enum nkvx_cancel_ack), endian-safe. */
	return hg_proc_int32_t(proc, &out->ack);
}

void
nkvx_exec_in_free(nkvx_exec_in_t *in)
{
	if (in == NULL) {
		return;
	}
	free(in->module_key);
	in->module_key = NULL;
	free(in->module_ns);
	in->module_ns = NULL;
	free(in->input_inline);
	in->input_inline = NULL;
}

void
nkvx_exec_out_free(nkvx_exec_out_t *out)
{
	if (out == NULL) {
		return;
	}
	free(out->result_inline);
	out->result_inline = NULL;
	out->result_inline_len = 0;
}

void
nkvx_kv_in_free(nkvx_kv_in_t *in)
{
	if (in == NULL) {
		return;
	}
	free(in->value_inline);
	in->value_inline = NULL;
}

void
nkvx_kv_out_free(nkvx_kv_out_t *out)
{
	if (out == NULL) {
		return;
	}
	free(out->result_inline);
	out->result_inline = NULL;
	out->result_inline_len = 0;
}
