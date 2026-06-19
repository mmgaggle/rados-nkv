/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Standalone test for the in-process NVMe KV host shim.
 *
 * Usage: kv_shim_test <vfio-user-socket-path>
 *
 * Opens the shim (init_env=true), DMA-allocs store/retrieve buffers, then runs
 * Store + Retrieve with both a non-trivial 4096-byte deterministic binary value
 * and a tiny value, to prove binary-safety and TRUE length reporting. Asserts
 * the retrieved bytes match byte-for-byte AND value_len_out == stored length.
 *
 * Prints "kv_shim_test: PASS" and exits 0 on success; prints "FAIL: ..." and
 * exits non-zero otherwise.
 */

#include "spdk/stdinc.h"

#include "kv_host_shim.h"

#define BIG_VALUE_LEN 4096u

static void
fill_pattern(uint8_t *buf, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++) {
		buf[i] = (uint8_t)((i * 31 + 7) & 0xff);
	}
}

/*
 * Store then Retrieve a value, asserting byte-for-byte equality and that the
 * device-reported TRUE length (value_len_out) equals the stored length. Uses a
 * retrieve buffer of at least BIG_VALUE_LEN so the value always fits. Returns 0
 * on success, non-zero on any failure (a FAIL line is printed).
 */
static int
store_retrieve_check(struct kv_host_shim *sh, const char *what,
		     const void *key, uint8_t key_len,
		     uint8_t *store_buf, const uint8_t *expect, uint32_t value_len,
		     uint8_t *retrieve_buf, uint32_t retrieve_buf_len)
{
	uint32_t value_len_out = 0;
	int rc;

	memcpy(store_buf, expect, value_len);
	rc = kv_host_shim_store(sh, key, key_len, store_buf, value_len);
	if (rc != 0) {
		fprintf(stderr, "FAIL: %s Store rc=%d (expected 0)\n", what, rc);
		return 1;
	}

	memset(retrieve_buf, 0, retrieve_buf_len);
	rc = kv_host_shim_retrieve(sh, key, key_len, retrieve_buf, retrieve_buf_len,
				   &value_len_out);
	if (rc != 0) {
		fprintf(stderr, "FAIL: %s Retrieve rc=%d (expected 0)\n", what, rc);
		return 1;
	}
	if (value_len_out != value_len) {
		fprintf(stderr, "FAIL: %s value_len_out=%u, expected %u (TRUE length)\n",
			what, value_len_out, value_len);
		return 1;
	}
	if (memcmp(retrieve_buf, expect, value_len) != 0) {
		fprintf(stderr, "FAIL: %s retrieved bytes do not match stored bytes\n", what);
		return 1;
	}
	fprintf(stderr, "OK: %s Store+Retrieve (%u bytes, value_len_out=%u, byte-exact)\n",
		what, value_len, value_len_out);
	return 0;
}

int
main(int argc, char **argv)
{
	struct kv_host_shim_opts opts;
	struct kv_host_shim *sh = NULL;
	uint8_t *store_buf = NULL;
	uint8_t *retrieve_buf = NULL;
	uint8_t big_expect[BIG_VALUE_LEN];
	const uint8_t tiny_expect[] = { 0xde, 0xad };
	const char big_key[] = "kvshim-big";
	const char tiny_key[] = "kvshim-tiny";
	const uint32_t buf_len = BIG_VALUE_LEN;
	int rc;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <vfio-user-socket-path>\n", argv[0]);
		return 2;
	}

	opts.opts_size = sizeof(opts);
	opts.name = "kv_shim_test";
	opts.vfu_addr = argv[1];
	opts.nsid = 0;		/* first CSI==KV namespace */
	opts.init_env = true;	/* this process owns the SPDK env */

	rc = kv_host_shim_open(&opts, &sh);
	if (rc != 0 || sh == NULL) {
		fprintf(stderr, "FAIL: kv_host_shim_open('%s') rc=%d\n", argv[1], rc);
		return 1;
	}
	fprintf(stderr, "shim open OK: max_key_len=%u max_value_len=%u\n",
		kv_host_shim_max_key_len(sh), kv_host_shim_max_value_len(sh));

	store_buf = kv_host_shim_dma_alloc(buf_len);
	retrieve_buf = kv_host_shim_dma_alloc(buf_len);
	if (store_buf == NULL || retrieve_buf == NULL) {
		fprintf(stderr, "FAIL: DMA buffer allocation failed\n");
		rc = 1;
		goto out;
	}

	/* Non-trivial binary value: 4096 bytes of b[i]=(i*31+7)&0xff. */
	fill_pattern(big_expect, BIG_VALUE_LEN);
	rc = store_retrieve_check(sh, "big(4096B binary)", big_key, (uint8_t)strlen(big_key),
				  store_buf, big_expect, BIG_VALUE_LEN,
				  retrieve_buf, buf_len);
	if (rc != 0) {
		goto out;
	}

	/* Tiny value to prove length reporting at the small end. */
	rc = store_retrieve_check(sh, "tiny(2B binary)", tiny_key, (uint8_t)strlen(tiny_key),
				  store_buf, tiny_expect, (uint32_t)sizeof(tiny_expect),
				  retrieve_buf, buf_len);
	if (rc != 0) {
		goto out;
	}

	/* Exist (present) + Delete + Exist (absent) on the tiny key, to exercise
	 * the logical-status return convention (0 vs 0x87). */
	rc = kv_host_shim_exist(sh, tiny_key, (uint8_t)strlen(tiny_key));
	if (rc != 0) {
		fprintf(stderr, "FAIL: Exist(present) rc=%d (expected 0)\n", rc);
		goto out;
	}
	rc = kv_host_shim_delete(sh, tiny_key, (uint8_t)strlen(tiny_key));
	if (rc != 0) {
		fprintf(stderr, "FAIL: Delete rc=%d (expected 0)\n", rc);
		goto out;
	}
	rc = kv_host_shim_exist(sh, tiny_key, (uint8_t)strlen(tiny_key));
	if (rc != 0x87) {
		fprintf(stderr, "FAIL: Exist(absent) rc=%d (expected 0x87 KEY_DOES_NOT_EXIST)\n", rc);
		rc = rc ? rc : 1;
		goto out;
	}
	fprintf(stderr, "OK: Exist/Delete/Exist convention (present=0, delete=0, absent=0x87)\n");

	rc = 0;
	printf("kv_shim_test: PASS\n");

out:
	kv_host_shim_dma_free(store_buf);
	kv_host_shim_dma_free(retrieve_buf);
	kv_host_shim_close(sh);
	return rc;
}
