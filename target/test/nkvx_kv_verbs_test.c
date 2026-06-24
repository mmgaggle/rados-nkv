/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/*
 * Direct executor-verb test (slice spdk-7sr.5 / S4 verification).
 *
 * Drives nkvx_executor_kv() against the TEST-ONLY in-memory backend
 * (nkvx_executor_open_mem) for the full KV CRUD surface — STORE (incl. SIKE/
 * SINKE), DELETE, EXIST, LIST (NRK-prefixed structure), and the authoritative
 * read-only gate (ADR-0008 D3) — WITHOUT Mercury or a Ceph cluster. This is the
 * executor-side counterpart to the na+sm loopback driver: the loopback proves the
 * transport (front->RPC->executor->result); this proves the verb SEMANTICS the
 * executor implements. The librados data path is dep-only (no cluster in CI).
 *
 * NOT part of the SPDK build graph; built by the nkvx_service Makefile target
 * nkvx_kv_verbs_test.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "nkvx_executor.h"
#include "spdk/kvdev.h"

static int g_fail;

#define CHECK(cond, msg, ...) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); \
		g_fail++; \
	} \
} while (0)

/* Build a base nkvx_kv_in_t for the given verb + ascii key. */
static nkvx_kv_in_t
mk_in(uint8_t verb, const char *key)
{
	nkvx_kv_in_t in;

	memset(&in, 0, sizeof(in));
	in.verb = verb;
	if (key != NULL) {
		size_t klen = strlen(key);
		in.key_len = (uint8_t)klen;
		memcpy(in.key, key, klen);
	}
	in.osize = 4096;
	return in;
}

static enum spdk_kvdev_io_status
do_store(struct nkvx_executor *ex, const char *key, const char *value,
	 uint8_t flags, bool read_only)
{
	nkvx_kv_in_t in = mk_in(NKVX_KV_VERB_STORE, key);
	struct nkvx_exec_result res;
	int rc;

	in.store_flags = flags;
	in.read_only = read_only ? 1 : 0;
	in.value_len = (uint32_t)strlen(value);
	in.value_inline = (void *)value;

	rc = nkvx_executor_kv(ex, &in, &res);
	CHECK(rc == 0, "store rc=%d", rc);
	enum spdk_kvdev_io_status st = res.status;
	nkvx_exec_result_free(&res);
	return st;
}

int
main(void)
{
	struct nkvx_executor *ex = NULL;
	int rc = nkvx_executor_open_mem(&ex);

	CHECK(rc == 0 && ex != NULL, "open_mem rc=%d", rc);
	if (ex == NULL) {
		return 1;
	}

	/* --- STORE (unconditional) then EXIST + RETRIEVE round-trip --- */
	CHECK(do_store(ex, "alpha", "hello", 0, false) == SPDK_KVDEV_IO_STATUS_SUCCESS,
	      "store alpha");

	{
		nkvx_kv_in_t in = mk_in(NKVX_KV_VERB_EXIST, "alpha");
		struct nkvx_exec_result res;

		nkvx_executor_kv(ex, &in, &res);
		CHECK(res.status == SPDK_KVDEV_IO_STATUS_SUCCESS, "exist alpha status");
		CHECK(res.result_len == 5, "exist alpha len=%u (want 5)", res.result_len);
		nkvx_exec_result_free(&res);
	}
	{
		nkvx_kv_in_t in = mk_in(NKVX_KV_VERB_RETRIEVE, "alpha");
		struct nkvx_exec_result res;

		nkvx_executor_kv(ex, &in, &res);
		CHECK(res.status == SPDK_KVDEV_IO_STATUS_SUCCESS, "retrieve alpha status");
		CHECK(res.result_len == 5 && res.buf_len == 5 &&
		      memcmp(res.buf, "hello", 5) == 0, "retrieve alpha value");
		nkvx_exec_result_free(&res);
	}

	/* --- EXIST / RETRIEVE of an absent key --- */
	{
		nkvx_kv_in_t in = mk_in(NKVX_KV_VERB_EXIST, "missing");
		struct nkvx_exec_result res;

		nkvx_executor_kv(ex, &in, &res);
		CHECK(res.status == SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, "exist missing");
		nkvx_exec_result_free(&res);
	}

	/* --- SINKE: store-if-no-key-exists --- */
	CHECK(do_store(ex, "alpha", "x", SPDK_KVDEV_STORE_FLAG_SINKE, false) ==
	      SPDK_KVDEV_IO_STATUS_KEY_EXIST, "SINKE on existing key -> KEY_EXIST");
	CHECK(do_store(ex, "beta", "world", SPDK_KVDEV_STORE_FLAG_SINKE, false) ==
	      SPDK_KVDEV_IO_STATUS_SUCCESS, "SINKE on new key -> SUCCESS");

	/* --- SIKE: store-if-key-exists --- */
	CHECK(do_store(ex, "gamma", "y", SPDK_KVDEV_STORE_FLAG_SIKE, false) ==
	      SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, "SIKE on absent key -> KEY_NOT_EXIST");
	CHECK(do_store(ex, "alpha", "HELLO2", SPDK_KVDEV_STORE_FLAG_SIKE, false) ==
	      SPDK_KVDEV_IO_STATUS_SUCCESS, "SIKE on existing key -> SUCCESS");

	/* Confirm the SIKE overwrite landed (cache coherence: the prior cached value
	 * must not be served — the mem backend has no cache, but the invalidate path
	 * still runs without error). */
	{
		nkvx_kv_in_t in = mk_in(NKVX_KV_VERB_RETRIEVE, "alpha");
		struct nkvx_exec_result res;

		nkvx_executor_kv(ex, &in, &res);
		CHECK(res.status == SPDK_KVDEV_IO_STATUS_SUCCESS &&
		      res.result_len == 6 && memcmp(res.buf, "HELLO2", 6) == 0,
		      "retrieve after SIKE overwrite");
		nkvx_exec_result_free(&res);
	}

	/* --- READ-ONLY gate (authoritative): STORE/DELETE rejected --- */
	CHECK(do_store(ex, "alpha", "z", 0, true) == SPDK_KVDEV_IO_STATUS_READ_ONLY,
	      "STORE on read-only ns -> READ_ONLY");
	{
		nkvx_kv_in_t in = mk_in(NKVX_KV_VERB_DELETE, "alpha");
		struct nkvx_exec_result res;

		in.read_only = 1;
		nkvx_executor_kv(ex, &in, &res);
		CHECK(res.status == SPDK_KVDEV_IO_STATUS_READ_ONLY,
		      "DELETE on read-only ns -> READ_ONLY");
		nkvx_exec_result_free(&res);
	}
	/* Read verbs always run even on a read-only ns. */
	{
		nkvx_kv_in_t in = mk_in(NKVX_KV_VERB_EXIST, "alpha");
		struct nkvx_exec_result res;

		in.read_only = 1;
		nkvx_executor_kv(ex, &in, &res);
		CHECK(res.status == SPDK_KVDEV_IO_STATUS_SUCCESS,
		      "EXIST on read-only ns still allowed");
		nkvx_exec_result_free(&res);
	}

	/* --- LIST: NRK-prefixed structure with the keys we stored (alpha, beta) --- */
	{
		nkvx_kv_in_t in = mk_in(NKVX_KV_VERB_LIST, NULL);	/* key_len 0 => from start */
		struct nkvx_exec_result res;
		uint32_t nrk;
		uint32_t off;

		nkvx_executor_kv(ex, &in, &res);
		CHECK(res.status == SPDK_KVDEV_IO_STATUS_SUCCESS, "list status");
		CHECK(res.buf != NULL && res.buf_len >= 4, "list has header");
		memcpy(&nrk, res.buf, sizeof(nrk));
		CHECK(nrk == 2, "list NRK=%u (want 2: alpha,beta)", nrk);

		/* Walk the entries: [u16 len][key][pad to 4]. Sorted order => alpha,beta. */
		off = 4;
		const char *expect[2] = { "alpha", "beta" };
		for (uint32_t i = 0; i < nrk && off + 2 <= res.buf_len; i++) {
			uint16_t kl;
			uint8_t *p = (uint8_t *)res.buf + off;

			memcpy(&kl, p, sizeof(kl));
			CHECK(kl == strlen(expect[i]) &&
			      memcmp(p + 2, expect[i], kl) == 0,
			      "list entry %u = '%.*s' (want '%s')", i, kl, p + 2, expect[i]);
			off += (uint32_t)((2 + kl + 3) & ~3u);	/* 4-byte aligned */
		}
		nkvx_exec_result_free(&res);
	}

	/* --- LIST truncation: osize too small for the full listing --- */
	{
		nkvx_kv_in_t in = mk_in(NKVX_KV_VERB_LIST, NULL);
		struct nkvx_exec_result res;
		uint32_t nrk;

		in.osize = 4 + 8;	/* header + one entry only */
		nkvx_executor_kv(ex, &in, &res);
		CHECK(res.status == SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL,
		      "list truncation -> BUFFER_TOO_SMALL (status=%d)", res.status);
		memcpy(&nrk, res.buf, sizeof(nrk));
		CHECK(nrk == 1, "list truncated NRK=%u (want 1 fit)", nrk);
		CHECK(res.result_len > in.osize, "list DW0 (full len %u) > osize %u",
		      res.result_len, in.osize);
		nkvx_exec_result_free(&res);
	}

	/* --- DELETE then EXIST confirms removal --- */
	{
		nkvx_kv_in_t in = mk_in(NKVX_KV_VERB_DELETE, "beta");
		struct nkvx_exec_result res;

		nkvx_executor_kv(ex, &in, &res);
		CHECK(res.status == SPDK_KVDEV_IO_STATUS_SUCCESS, "delete beta");
		nkvx_exec_result_free(&res);

		in = mk_in(NKVX_KV_VERB_EXIST, "beta");
		nkvx_executor_kv(ex, &in, &res);
		CHECK(res.status == SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST,
		      "exist beta after delete -> KEY_NOT_EXIST");
		nkvx_exec_result_free(&res);
	}

	/* --- DELETE absent key --- */
	{
		nkvx_kv_in_t in = mk_in(NKVX_KV_VERB_DELETE, "neverwas");
		struct nkvx_exec_result res;

		nkvx_executor_kv(ex, &in, &res);
		CHECK(res.status == SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST,
		      "delete absent -> KEY_NOT_EXIST");
		nkvx_exec_result_free(&res);
	}

	nkvx_executor_close(ex);

	if (g_fail == 0) {
		printf("nkvx_kv_verbs_test: PASS (all verbs + SIKE/SINKE + read-only + LIST)\n");
		return 0;
	}
	fprintf(stderr, "nkvx_kv_verbs_test: FAIL (%d checks)\n", g_fail);
	return 1;
}
