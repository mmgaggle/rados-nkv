/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/*
 * Standalone round-trip unit test for the inter-tier RPC contract.
 *
 * Builds directly against Mercury (no SPDK unit-test framework, no reactor):
 *
 *   - Slice C3 Exec contract: round-trips a populated request/response through
 *     the hg_proc serializers using an in-memory Mercury proc and asserts
 *     field-for-field equality; exercises the wire-status table (all 10 enum
 *     values + unknown->FAILED) and the inline/bulk + truncation edge cases.
 *
 *   - Slice N1 KV-verb contract (Store/Retrieve/Delete/Exist/List): the same
 *     in-memory proc round-trip for nkvx_kv_in_t/nkvx_kv_out_t across every
 *     verb's payload shape (Store value inline/boundary/bulk; Retrieve/List
 *     inline-or-sink; Delete/Exist status-only; List start-key incl. key_len 0),
 *     PLUS a REAL na+sm Mercury RPC loopback (test_kv_loopback): a listening
 *     target + an origin in one process forward the `nkvx_kv` RPC per verb with
 *     an echo handler, driving the actual register/encode/SEND/decode/RESPOND
 *     wire path. The real librados Store/Retrieve + TB4 cache are later slices.
 *
 * Built+run by: make -f Makefile.ut (see that file for the Mercury flags).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <mercury.h>
#include <mercury_proc.h>

#include "nkvx_exec_rpc.h"

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do {						\
	g_checks++;							\
	if (!(cond)) {							\
		g_failures++;						\
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);	\
		fprintf(stderr, __VA_ARGS__);				\
		fprintf(stderr, "\n");					\
	}								\
} while (0)

/* A throwaway HG class on the null/sm transport just to mint hg_proc objects.
 * hg_proc needs a class for checksum/extra-buf bookkeeping; "na+sm" inits with
 * no network. */
static hg_class_t *g_hg_class;

/*
 * Encode `src` into a fixed scratch buffer, then decode it back into `dst`,
 * exercising the real Mercury in-memory proc both ways. The largest payload
 * that is ever serialized is the 4 KiB inline cap plus small fixed fields (the
 * 64 MiB cases ride bulk handles, so only lengths/handles are encoded), so a
 * 64 KiB scratch buffer is comfortably sufficient.
 *
 * `procfn` is the hg_proc routine; `srctmp`/`dst` are caller-typed buffers.
 */
#define PROC_SCRATCH_SIZE (64u * 1024u)

static void
proc_roundtrip(hg_return_t (*procfn)(hg_proc_t, void *), void *src_tmp,
	       void *dst, size_t struct_size)
{
	static unsigned char buf[PROC_SCRATCH_SIZE];
	hg_proc_t proc;
	hg_return_t ret;
	hg_size_t used;

	memset(buf, 0, sizeof(buf));

	/* Encode src_tmp into buf (procfn may mutate src_tmp on FREE/ENCODE; the
	 * caller passes a throwaway copy). */
	ret = hg_proc_create_set(g_hg_class, buf, sizeof(buf), HG_ENCODE, HG_NOHASH, &proc);
	assert(ret == HG_SUCCESS);
	ret = procfn(proc, src_tmp);
	assert(ret == HG_SUCCESS);
	ret = hg_proc_flush(proc);
	assert(ret == HG_SUCCESS);
	used = hg_proc_get_size_used(proc);
	assert(used <= sizeof(buf));
	hg_proc_free(proc);

	/* Decode into dst. */
	memset(dst, 0, struct_size);
	ret = hg_proc_create_set(g_hg_class, buf, sizeof(buf), HG_DECODE, HG_NOHASH, &proc);
	assert(ret == HG_SUCCESS);
	ret = procfn(proc, dst);
	assert(ret == HG_SUCCESS);
	hg_proc_free(proc);
}

static void
proc_roundtrip_in(const nkvx_exec_in_t *src, nkvx_exec_in_t *dst)
{
	nkvx_exec_in_t tmp = *src;	/* throwaway copy for the encode pass */

	proc_roundtrip(hg_proc_nkvx_exec_in_t, &tmp, dst, sizeof(*dst));
}

static void
proc_roundtrip_out(const nkvx_exec_out_t *src, nkvx_exec_out_t *dst)
{
	nkvx_exec_out_t tmp = *src;

	proc_roundtrip(hg_proc_nkvx_exec_out_t, &tmp, dst, sizeof(*dst));
}

static void
proc_roundtrip_kv_in(const nkvx_kv_in_t *src, nkvx_kv_in_t *dst)
{
	nkvx_kv_in_t tmp = *src;

	proc_roundtrip(hg_proc_nkvx_kv_in_t, &tmp, dst, sizeof(*dst));
}

static void
proc_roundtrip_kv_out(const nkvx_kv_out_t *src, nkvx_kv_out_t *dst)
{
	nkvx_kv_out_t tmp = *src;

	proc_roundtrip(hg_proc_nkvx_kv_out_t, &tmp, dst, sizeof(*dst));
}

static void
test_request_roundtrip(uint32_t input_len, const char *label)
{
	nkvx_exec_in_t in, out;
	uint8_t *input = NULL;
	uint32_t i;
	int expect_inline = (input_len <= NKVX_INLINE_MAX);

	memset(&in, 0, sizeof(in));
	in.op_id = 0xDEADBEEF;
	in.read_only = 1;
	in.runtime = SPDK_KV_EXEC_RUNTIME_NKVX;
	in.caps = 3;	/* LARGE tier */
	in.key_len = 200;	/* exercise a >16-byte exec key up to 255 */
	for (i = 0; i < in.key_len; i++) {
		in.key[i] = (uint8_t)(i * 7 + 1);
	}
	for (i = 0; i < SPDK_KV_EXEC_SHA256_LEN; i++) {
		in.sha256[i] = (uint8_t)(0xA0 + i);
	}
	in.sha256_valid = 1;
	in.module_key = strdup("kvcache.wasm");
	in.module_ns = strdup("nkvx-pool/modules");
	in.osize = 64u * 1024u * 1024u;	/* 64 MiB host output buffer */
	in.input_len = input_len;

	if (input_len > 0) {
		input = malloc(input_len);
		assert(input != NULL);
		for (i = 0; i < input_len; i++) {
			input[i] = (uint8_t)(i & 0xFF);
		}
	}
	if (expect_inline) {
		in.input_inline = input;	/* inline carries the bytes */
		in.input_bulk = HG_BULK_NULL;
	} else {
		in.input_inline = NULL;		/* large: bytes ride the bulk handle */
		in.input_bulk = HG_BULK_NULL;	/* handle creation is Slice C7; NULL here */
	}
	in.result_sink = HG_BULK_NULL;

	proc_roundtrip_in(&in, &out);

	CHECK(out.op_id == in.op_id, "[%s] op_id", label);
	CHECK(out.read_only == in.read_only, "[%s] read_only", label);
	CHECK(out.runtime == in.runtime, "[%s] runtime", label);
	CHECK(out.caps == in.caps, "[%s] caps", label);
	CHECK(out.key_len == in.key_len, "[%s] key_len", label);
	CHECK(memcmp(out.key, in.key, in.key_len) == 0, "[%s] key bytes", label);
	CHECK(memcmp(out.sha256, in.sha256, SPDK_KV_EXEC_SHA256_LEN) == 0, "[%s] sha256", label);
	CHECK(out.sha256_valid == in.sha256_valid, "[%s] sha256_valid", label);
	CHECK(out.module_key && strcmp(out.module_key, in.module_key) == 0, "[%s] module_key", label);
	CHECK(out.module_ns && strcmp(out.module_ns, in.module_ns) == 0, "[%s] module_ns", label);
	CHECK(out.osize == in.osize, "[%s] osize", label);
	CHECK(out.input_len == in.input_len, "[%s] input_len (TRUE len preserved)", label);

	if (expect_inline) {
		if (input_len == 0) {
			CHECK(out.input_inline == NULL, "[%s] empty input -> NULL inline", label);
		} else {
			CHECK(out.input_inline != NULL, "[%s] inline buf present", label);
			CHECK(memcmp(out.input_inline, input, input_len) == 0,
			      "[%s] inline input bytes match", label);
		}
	} else {
		/* Over the 4 KiB boundary: NOT carried inline; handle would carry it. */
		CHECK(out.input_inline == NULL, "[%s] >4KiB input not inlined", label);
	}

	nkvx_exec_in_free(&out);
	free(input);
	free(in.module_key);	/* free the strdup'd source strings (decoded copy freed above) */
	free(in.module_ns);
}

static void
test_response_roundtrip(int32_t status, uint32_t result_len, uint32_t osize,
			int use_sink, const char *label)
{
	nkvx_exec_out_t in, out;
	uint8_t *result = NULL;
	uint32_t delivered = (result_len < osize) ? result_len : osize; /* min */
	uint32_t inline_len;
	int inline_path = (!use_sink && delivered <= NKVX_INLINE_MAX);
	uint32_t i;

	memset(&in, 0, sizeof(in));
	in.status = status;
	in.result_len = result_len;	/* TRUE length, even when truncated */

	if (inline_path && delivered > 0) {
		inline_len = delivered;
		result = malloc(inline_len);
		assert(result != NULL);
		for (i = 0; i < inline_len; i++) {
			result[i] = (uint8_t)(0x5A ^ (i & 0xFF));
		}
		in.result_inline = result;
		in.result_inline_len = inline_len;
	} else {
		in.result_inline = NULL;
		in.result_inline_len = 0;
	}

	proc_roundtrip_out(&in, &out);

	CHECK(out.status == in.status, "[%s] status int32 preserved", label);
	CHECK(out.result_len == in.result_len, "[%s] result_len TRUE len preserved", label);
	CHECK(out.result_inline_len == in.result_inline_len, "[%s] result_inline_len", label);
	if (in.result_inline_len > 0) {
		CHECK(out.result_inline != NULL && memcmp(out.result_inline, in.result_inline,
				in.result_inline_len) == 0, "[%s] inline result bytes", label);
	} else {
		CHECK(out.result_inline == NULL, "[%s] no inline result", label);
	}

	nkvx_exec_out_free(&out);
	free(result);
}

static void
test_status_table(void)
{
	struct { enum spdk_kvdev_io_status e; int32_t wire; } tbl[] = {
		{ SPDK_KVDEV_IO_STATUS_SUCCESS,          0 },
		{ SPDK_KVDEV_IO_STATUS_FAILED,          -1 },
		{ SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST,   -2 },
		{ SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL,-3 },
		{ SPDK_KVDEV_IO_STATUS_INVALID,         -4 },
		{ SPDK_KVDEV_IO_STATUS_NOMEM,           -5 },
		{ SPDK_KVDEV_IO_STATUS_KEY_EXIST,       -6 },
		{ SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED,   -7 },
		{ SPDK_KVDEV_IO_STATUS_ABORTED,         -8 },
		{ SPDK_KVDEV_IO_STATUS_READ_ONLY,       -9 },
	};
	size_t n = sizeof(tbl) / sizeof(tbl[0]);
	size_t i;

	CHECK(n == 10, "status table covers all 10 enum values");

	for (i = 0; i < n; i++) {
		int32_t w = nkvx_status_to_wire(tbl[i].e);
		enum spdk_kvdev_io_status back = nkvx_status_from_wire(w);
		CHECK(w == tbl[i].wire, "to_wire(%d) == %d", (int)tbl[i].e, (int)tbl[i].wire);
		CHECK(back == tbl[i].e, "from_wire round-trip for %d", (int)tbl[i].e);
	}

	/* Unknown wire values -> FAILED (design §3). */
	CHECK(nkvx_status_from_wire(1) == SPDK_KVDEV_IO_STATUS_FAILED, "unknown +1 -> FAILED");
	CHECK(nkvx_status_from_wire(-100) == SPDK_KVDEV_IO_STATUS_FAILED, "unknown -100 -> FAILED");
	CHECK(nkvx_status_from_wire(0x7FFFFFFF) == SPDK_KVDEV_IO_STATUS_FAILED, "INT32_MAX -> FAILED");
}

/*
 * Per-verb KV request round-trip. Exercises every verb's payload shape through
 * the real Mercury in-memory proc and asserts field-for-field equality,
 * including the inline-vs-bulk Store value boundary.
 */
static void
test_kv_request_roundtrip(uint8_t verb, uint32_t value_len, uint32_t osize,
			  uint8_t key_len, int use_sink, const char *label)
{
	nkvx_kv_in_t in, out;
	uint8_t *value = NULL;
	uint32_t i;
	int expect_inline = (value_len > 0 && value_len <= NKVX_INLINE_MAX);

	memset(&in, 0, sizeof(in));
	in.verb = verb;
	in.read_only = (verb == NKVX_KV_VERB_RETRIEVE || verb == NKVX_KV_VERB_EXIST ||
			verb == NKVX_KV_VERB_LIST) ? 1 : 0;
	in.client_call_id = 0xABCDEF01ull;
	in.key_len = key_len;
	for (i = 0; i < key_len; i++) {
		in.key[i] = (uint8_t)(i * 5 + 3);
	}
	in.osize = osize;
	in.value_len = value_len;

	if (value_len > 0) {
		value = malloc(value_len);
		assert(value != NULL);
		for (i = 0; i < value_len; i++) {
			value[i] = (uint8_t)((i * 3) & 0xFF);
		}
	}
	if (expect_inline) {
		in.value_inline = value;
		in.value_bulk = HG_BULK_NULL;
	} else {
		in.value_inline = NULL;		/* large value rides the bulk handle */
		in.value_bulk = HG_BULK_NULL;	/* handle creation is a later slice; NULL here */
	}
	in.result_sink = HG_BULK_NULL;
	(void)use_sink;	/* sink is a NULL handle in the contract test (transfer is later) */

	proc_roundtrip_kv_in(&in, &out);

	CHECK(out.verb == in.verb, "[%s] verb", label);
	CHECK(out.read_only == in.read_only, "[%s] read_only", label);
	CHECK(out.client_call_id == in.client_call_id, "[%s] client_call_id", label);
	CHECK(out.key_len == in.key_len, "[%s] key_len", label);
	if (key_len > 0) {
		CHECK(memcmp(out.key, in.key, key_len) == 0, "[%s] key bytes", label);
	}
	CHECK(out.osize == in.osize, "[%s] osize", label);
	CHECK(out.value_len == in.value_len, "[%s] value_len (TRUE len preserved)", label);

	if (expect_inline) {
		CHECK(out.value_inline != NULL, "[%s] inline value present", label);
		CHECK(memcmp(out.value_inline, value, value_len) == 0,
		      "[%s] inline value bytes match", label);
	} else {
		CHECK(out.value_inline == NULL, "[%s] no inline value (empty or >4KiB)", label);
	}

	nkvx_kv_in_free(&out);
	free(value);
}

/* Per-verb KV response round-trip (status + truncation + inline/sink result). */
static void
test_kv_response_roundtrip(int32_t status, uint32_t result_len, uint32_t osize,
			   int use_sink, const char *label)
{
	nkvx_kv_out_t in, out;
	uint8_t *result = NULL;
	uint32_t delivered = (result_len < osize) ? result_len : osize;
	int inline_path = (!use_sink && delivered <= NKVX_INLINE_MAX);
	uint32_t i;

	memset(&in, 0, sizeof(in));
	in.status = status;
	in.result_len = result_len;

	if (inline_path && delivered > 0) {
		result = malloc(delivered);
		assert(result != NULL);
		for (i = 0; i < delivered; i++) {
			result[i] = (uint8_t)(0x3C ^ (i & 0xFF));
		}
		in.result_inline = result;
		in.result_inline_len = delivered;
	} else {
		in.result_inline = NULL;
		in.result_inline_len = 0;
	}

	proc_roundtrip_kv_out(&in, &out);

	CHECK(out.status == in.status, "[%s] status int32 preserved", label);
	CHECK(out.result_len == in.result_len, "[%s] result_len TRUE len preserved", label);
	CHECK(out.result_inline_len == in.result_inline_len, "[%s] result_inline_len", label);
	if (in.result_inline_len > 0) {
		CHECK(out.result_inline != NULL && memcmp(out.result_inline, in.result_inline,
				in.result_inline_len) == 0, "[%s] inline result bytes", label);
	} else {
		CHECK(out.result_inline == NULL, "[%s] no inline result", label);
	}

	nkvx_kv_out_free(&out);
	free(result);
}

static void
test_kv_verb_enum(void)
{
	/* 0 must be reserved (deny-by-default safety, ADR-0008 D1). */
	CHECK(NKVX_KV_VERB_INVALID == 0, "INVALID reserved as 0");
	CHECK(NKVX_KV_VERB_STORE == 1 && NKVX_KV_VERB_RETRIEVE == 2 &&
	      NKVX_KV_VERB_DELETE == 3 && NKVX_KV_VERB_EXIST == 4 &&
	      NKVX_KV_VERB_LIST == 5, "verb values stable");
}

/*
 * ====================================================================
 * Real na+sm RPC loopback (one process: a Mercury target listening on na+sm,
 * an origin forwarding to it). This drives the nkvx_kv contract over an ACTUAL
 * Mercury transport — registration, encode on SEND, decode on the target,
 * encode on RESPONSE, decode on the origin — for every verb, with an echo
 * handler. The real librados Store/Retrieve + TB4 cache are later slices; here
 * the handler just echoes a per-verb status + payload to prove the wire path.
 * ====================================================================
 */
static hg_class_t  *g_lb_class;
static hg_context_t *g_lb_ctx;
static hg_id_t      g_lb_kv_id;
static int          g_lb_done;
static nkvx_kv_out_t g_lb_reply;	/* origin's decoded reply (copied out of handle) */

/* Target-side handler: decode the request, synthesize a verb-appropriate reply. */
static hg_return_t
lb_kv_handler(hg_handle_t handle)
{
	nkvx_kv_in_t in;
	nkvx_kv_out_t out;
	hg_return_t ret;

	memset(&in, 0, sizeof(in));
	ret = HG_Get_input(handle, &in);
	assert(ret == HG_SUCCESS);

	memset(&out, 0, sizeof(out));
	switch (in.verb) {
	case NKVX_KV_VERB_STORE:
		/* Echo back the stored length as a SUCCESS; no payload. */
		out.status = nkvx_status_to_wire(SPDK_KVDEV_IO_STATUS_SUCCESS);
		out.result_len = 0;
		break;
	case NKVX_KV_VERB_RETRIEVE:
	case NKVX_KV_VERB_LIST: {
		/* Echo a small inline payload derived from the key. */
		static uint8_t payload[64];
		uint32_t n = in.key_len ? in.key_len : 1;
		uint32_t i;
		for (i = 0; i < n; i++) {
			payload[i] = (uint8_t)(in.key[i % (in.key_len ? in.key_len : 1)] ^ 0x11);
		}
		out.status = nkvx_status_to_wire(SPDK_KVDEV_IO_STATUS_SUCCESS);
		out.result_len = n;
		out.result_inline = payload;
		out.result_inline_len = (n <= in.osize) ? n : in.osize;
		break;
	}
	case NKVX_KV_VERB_EXIST:
		out.status = nkvx_status_to_wire(SPDK_KVDEV_IO_STATUS_SUCCESS);
		out.result_len = 123;	/* echoed stored value length */
		break;
	case NKVX_KV_VERB_DELETE:
		out.status = nkvx_status_to_wire(SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST);
		break;
	default:
		out.status = nkvx_status_to_wire(SPDK_KVDEV_IO_STATUS_INVALID);
		break;
	}

	ret = HG_Respond(handle, NULL, NULL, &out);
	assert(ret == HG_SUCCESS);

	HG_Free_input(handle, &in);
	HG_Destroy(handle);
	return HG_SUCCESS;
}

/* Origin-side completion: copy the reply out so the driver can assert on it. */
static hg_return_t
lb_origin_cb(const struct hg_cb_info *info)
{
	nkvx_kv_out_t out;

	assert(info->ret == HG_SUCCESS);
	memset(&out, 0, sizeof(out));
	assert(HG_Get_output(info->info.forward.handle, &out) == HG_SUCCESS);

	/* Deep-copy the inline result so it survives HG_Free_output. */
	g_lb_reply.status = out.status;
	g_lb_reply.result_len = out.result_len;
	g_lb_reply.result_inline_len = out.result_inline_len;
	if (out.result_inline_len > 0) {
		g_lb_reply.result_inline = malloc(out.result_inline_len);
		assert(g_lb_reply.result_inline != NULL);
		memcpy(g_lb_reply.result_inline, out.result_inline, out.result_inline_len);
	} else {
		g_lb_reply.result_inline = NULL;
	}

	HG_Free_output(info->info.forward.handle, &out);
	HG_Destroy(info->info.forward.handle);
	g_lb_done = 1;
	return HG_SUCCESS;
}

/* Pump the (single-threaded) Mercury progress loop until g_lb_done flips. */
static void
lb_progress_until_done(void)
{
	unsigned int count;
	hg_return_t ret;

	while (!g_lb_done) {
		do {
			count = 0;
			ret = HG_Trigger(g_lb_ctx, 0, 1, &count);
		} while (ret == HG_SUCCESS && count > 0);
		ret = HG_Progress(g_lb_ctx, 100);
		assert(ret == HG_SUCCESS || ret == HG_TIMEOUT);
	}
	/* Drain any trailing target-side callbacks (the Respond completion). */
	do {
		count = 0;
		HG_Trigger(g_lb_ctx, 0, 8, &count);
	} while (count > 0);
}

static void
test_kv_loopback_verb(const char *self_addr, uint8_t verb, uint32_t value_len,
		      int32_t expect_status, const char *label)
{
	hg_addr_t target = HG_ADDR_NULL;
	hg_handle_t handle;
	nkvx_kv_in_t in;
	uint8_t *value = NULL;
	uint32_t i;
	hg_return_t ret;

	ret = HG_Addr_lookup2(g_lb_class, self_addr, &target);
	assert(ret == HG_SUCCESS);

	memset(&in, 0, sizeof(in));
	in.verb = verb;
	in.client_call_id = 0x42;
	in.key_len = 8;
	for (i = 0; i < in.key_len; i++) {
		in.key[i] = (uint8_t)(0x10 + i);
	}
	in.osize = 4096;
	in.value_len = value_len;
	if (value_len > 0 && value_len <= NKVX_INLINE_MAX) {
		value = malloc(value_len);
		assert(value != NULL);
		for (i = 0; i < value_len; i++) {
			value[i] = (uint8_t)i;
		}
		in.value_inline = value;
	}
	in.value_bulk = HG_BULK_NULL;
	in.result_sink = HG_BULK_NULL;

	ret = HG_Create(g_lb_ctx, target, g_lb_kv_id, &handle);
	assert(ret == HG_SUCCESS);

	g_lb_done = 0;
	memset(&g_lb_reply, 0, sizeof(g_lb_reply));
	ret = HG_Forward(handle, lb_origin_cb, NULL, &in);
	assert(ret == HG_SUCCESS);

	lb_progress_until_done();

	CHECK(g_lb_reply.status == expect_status, "[%s] loopback status %d (got %d)",
	      label, (int)expect_status, (int)g_lb_reply.status);

	free(g_lb_reply.result_inline);
	g_lb_reply.result_inline = NULL;
	free(value);
	HG_Addr_free(g_lb_class, target);
}

static void
test_kv_loopback(void)
{
	char self_addr[256];
	hg_size_t addr_len = sizeof(self_addr);
	hg_addr_t self = HG_ADDR_NULL;
	hg_return_t ret;

	/* A listening na+sm class for the real RPC loopback. */
	g_lb_class = HG_Init("na+sm://", HG_TRUE);
	if (g_lb_class == NULL) {
		fprintf(stderr, "SKIP loopback: HG_Init(na+sm, listen) failed\n");
		return;
	}
	g_lb_ctx = HG_Context_create(g_lb_class);
	assert(g_lb_ctx != NULL);

	g_lb_kv_id = HG_Register_name(g_lb_class, "nkvx_kv",
				      hg_proc_nkvx_kv_in_t, hg_proc_nkvx_kv_out_t,
				      lb_kv_handler);
	assert(g_lb_kv_id != 0);

	ret = HG_Addr_self(g_lb_class, &self);
	assert(ret == HG_SUCCESS);
	ret = HG_Addr_to_string(g_lb_class, self_addr, &addr_len, self);
	assert(ret == HG_SUCCESS);
	HG_Addr_free(g_lb_class, self);

	printf("== KV verb na+sm RPC loopback (self=%s) ==\n", self_addr);
	test_kv_loopback_verb(self_addr, NKVX_KV_VERB_STORE, 256,
			      nkvx_status_to_wire(SPDK_KVDEV_IO_STATUS_SUCCESS), "STORE");
	test_kv_loopback_verb(self_addr, NKVX_KV_VERB_RETRIEVE, 0,
			      nkvx_status_to_wire(SPDK_KVDEV_IO_STATUS_SUCCESS), "RETRIEVE");
	test_kv_loopback_verb(self_addr, NKVX_KV_VERB_DELETE, 0,
			      nkvx_status_to_wire(SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST), "DELETE");
	test_kv_loopback_verb(self_addr, NKVX_KV_VERB_EXIST, 0,
			      nkvx_status_to_wire(SPDK_KVDEV_IO_STATUS_SUCCESS), "EXIST");
	test_kv_loopback_verb(self_addr, NKVX_KV_VERB_LIST, 0,
			      nkvx_status_to_wire(SPDK_KVDEV_IO_STATUS_SUCCESS), "LIST");

	HG_Context_destroy(g_lb_ctx);
	HG_Finalize(g_lb_class);
}

int
main(void)
{
	hg_return_t ret;

	/* "na+sm" inits Mercury with the shared-memory NA plugin and no real
	 * network — enough to mint hg_proc objects for in-memory (de)serialization. */
	g_hg_class = HG_Init("na+sm://", HG_TRUE);
	if (g_hg_class == NULL) {
		fprintf(stderr, "HG_Init(na+sm) failed; trying na+sm without listen\n");
		g_hg_class = HG_Init("na+sm://", HG_FALSE);
	}
	assert(g_hg_class != NULL);

	printf("== status table ==\n");
	test_status_table();

	printf("== request round-trip: input edge cases ==\n");
	test_request_roundtrip(0, "empty input");
	test_request_roundtrip(1, "1-byte input");
	test_request_roundtrip(100, "100-byte input");
	test_request_roundtrip(NKVX_INLINE_MAX - 1, "4KiB-1 (just under boundary)");
	test_request_roundtrip(NKVX_INLINE_MAX, "exactly 4KiB (inline boundary)");
	test_request_roundtrip(NKVX_INLINE_MAX + 1, "4KiB+1 (over -> bulk)");
	test_request_roundtrip(64u * 1024u * 1024u, "64 MiB (over -> bulk)");

	printf("== response round-trip: result/truncation cases ==\n");
	/* SUCCESS, small inline result. */
	test_response_roundtrip(0, 256, 1u << 20, 0, "success small inline");
	/* SUCCESS, result exactly at the 4KiB inline boundary. */
	test_response_roundtrip(0, NKVX_INLINE_MAX, 1u << 20, 0, "success 4KiB inline boundary");
	/* SUCCESS, empty result. */
	test_response_roundtrip(0, 0, 1u << 20, 0, "success empty result");
	/* BUFFER_TOO_SMALL: TRUE result_len (10) exceeds osize (4); delivered=4 inline,
	 * but result_len must still carry the TRUE length 10. */
	test_response_roundtrip(-3, 10, 4, 0, "truncated: result_len > osize");
	/* Large result truncated to a small osize -> still inline-delivered min(). */
	test_response_roundtrip(-3, 1u << 20, 100, 0, "truncated 1MiB->100");
	/* Large result via WRITE-bulk sink: no inline bytes, result_len is TRUE len. */
	test_response_roundtrip(0, 64u * 1024u * 1024u, 64u * 1024u * 1024u, 1, "64MiB via sink");
	/* A non-success status with no inline payload. */
	test_response_roundtrip(-2, 0, 1u << 20, 0, "KEY_NOT_EXIST");

	printf("== KV verb enum ==\n");
	test_kv_verb_enum();

	printf("== KV request round-trip: per-verb payload shapes ==\n");
	/* Store: value inline / boundary / over-boundary (bulk). */
	test_kv_request_roundtrip(NKVX_KV_VERB_STORE, 0, 0, 16, 0, "store empty value");
	test_kv_request_roundtrip(NKVX_KV_VERB_STORE, 100, 0, 16, 0, "store 100B inline");
	test_kv_request_roundtrip(NKVX_KV_VERB_STORE, NKVX_INLINE_MAX, 0, 200, 0,
				  "store 4KiB boundary inline");
	test_kv_request_roundtrip(NKVX_KV_VERB_STORE, NKVX_INLINE_MAX + 1, 0, 16, 0,
				  "store 4KiB+1 -> bulk");
	test_kv_request_roundtrip(NKVX_KV_VERB_STORE, 64u * 1024u * 1024u, 0, 16, 0,
				  "store 64MiB -> bulk");
	/* Retrieve: no value in, osize set, inline result vs sink. */
	test_kv_request_roundtrip(NKVX_KV_VERB_RETRIEVE, 0, 4096, 16, 0, "retrieve inline osize");
	test_kv_request_roundtrip(NKVX_KV_VERB_RETRIEVE, 0, 64u * 1024u * 1024u, 16, 1,
				  "retrieve 64MiB osize (sink)");
	/* Delete / Exist: status-only, key carried. */
	test_kv_request_roundtrip(NKVX_KV_VERB_DELETE, 0, 0, 16, 0, "delete");
	test_kv_request_roundtrip(NKVX_KV_VERB_EXIST, 0, 0, 255, 0, "exist max key");
	/* List: start-position key (0 => from beginning) + osize. */
	test_kv_request_roundtrip(NKVX_KV_VERB_LIST, 0, 4096, 0, 0, "list from-beginning (key_len 0)");
	test_kv_request_roundtrip(NKVX_KV_VERB_LIST, 0, 4096, 32, 0, "list from start-key");

	printf("== KV response round-trip: status/truncation/result ==\n");
	test_kv_response_roundtrip(0, 256, 1u << 20, 0, "retrieve small inline");
	test_kv_response_roundtrip(0, NKVX_INLINE_MAX, 1u << 20, 0, "retrieve 4KiB boundary");
	test_kv_response_roundtrip(-3, 10, 4, 0, "retrieve truncated result_len>osize");
	test_kv_response_roundtrip(0, 64u * 1024u * 1024u, 64u * 1024u * 1024u, 1, "retrieve 64MiB via sink");
	test_kv_response_roundtrip(0, 0, 1u << 20, 0, "store/delete SUCCESS no payload");
	test_kv_response_roundtrip(-2, 0, 1u << 20, 0, "KEY_NOT_EXIST");
	test_kv_response_roundtrip(-9, 0, 1u << 20, 0, "READ_ONLY (mutating verb on RO ns)");

	test_kv_loopback();

	HG_Finalize(g_hg_class);

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures == 0) {
		printf("PASS\n");
		return 0;
	}
	printf("FAIL\n");
	(void)ret;
	return 1;
}
