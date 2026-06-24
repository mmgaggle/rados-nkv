/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/*
 * Standalone RETRIEVE round-trip driver (slice spdk-7sr.4 / S3 verification).
 *
 * A non-SPDK Mercury ORIGIN that drives the SAME nkvx_front_client.c the integrated
 * bdev_kvrados front uses, calling nkvx_front_kv_forward() for the NKVX_KV_VERB_RETRIEVE
 * verb against a running nkvx_service (started with --mem-object so no Ceph cluster is
 * needed). Proves the front->RPC->executor->result path end-to-end over na+sm and
 * checks the value bytes + CQE-DW0 (TRUE value length) + truncation status.
 *
 * NOT part of the SPDK build graph; built by the nkvx_service Makefile target
 * nkvx_kv_retrieve_test.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <getopt.h>
#include <errno.h>

#include "nkvx_front_client.h"

struct done_ctx {
	bool				done;
	enum spdk_kvdev_io_status	status;
	uint32_t			result_len;
	unsigned char			value[4096];
	uint32_t			value_n;
};

static void
done_cb(void *arg, enum spdk_kvdev_io_status status, uint32_t result_len,
	const void *result_inline, uint32_t result_inline_len)
{
	struct done_ctx *d = arg;

	d->status = status;
	d->result_len = result_len;
	if (result_inline != NULL && result_inline_len > 0) {
		uint32_t n = result_inline_len < sizeof(d->value) ?
			result_inline_len : (uint32_t)sizeof(d->value);
		memcpy(d->value, result_inline, n);
		d->value_n = n;
	}
	d->done = true;
}

static int
read_addr_file(const char *path, char *buf, size_t sz)
{
	FILE *f = fopen(path, "r");
	char *p;

	if (f == NULL) {
		return -1;
	}
	p = fgets(buf, (int)sz, f);
	fclose(f);
	if (p == NULL) {
		return -1;
	}
	buf[strcspn(buf, "\r\n")] = '\0';
	return (buf[0] != '\0') ? 0 : -1;
}

int
main(int argc, char **argv)
{
	const char *na_init = "na+sm://";
	const char *addr_file = NULL;
	const char *key = "ping";
	const char *expect_value = NULL;	/* assert value bytes match */
	int expect_status = SPDK_KVDEV_IO_STATUS_SUCCESS;
	int osize = 4096;			/* host output cap; small -> inline */

	enum { OPT_KEY = 256, OPT_ADDR_FILE, OPT_EXPECT_VALUE, OPT_EXPECT_STATUS,
	       OPT_OSIZE, OPT_LISTEN };
	static const struct option opts[] = {
		{ "listen",        required_argument, NULL, OPT_LISTEN },
		{ "addr-file",     required_argument, NULL, OPT_ADDR_FILE },
		{ "key",           required_argument, NULL, OPT_KEY },
		{ "expect-value",  required_argument, NULL, OPT_EXPECT_VALUE },
		{ "expect-status", required_argument, NULL, OPT_EXPECT_STATUS },
		{ "osize",         required_argument, NULL, OPT_OSIZE },
		{ NULL, 0, NULL, 0 },
	};
	int c;
	while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (c) {
		case OPT_LISTEN: na_init = optarg; break;
		case OPT_ADDR_FILE: addr_file = optarg; break;
		case OPT_KEY: key = optarg; break;
		case OPT_EXPECT_VALUE: expect_value = optarg; break;
		case OPT_EXPECT_STATUS: expect_status = atoi(optarg); break;
		case OPT_OSIZE: osize = atoi(optarg); break;
		default: return 2;
		}
	}
	if (addr_file == NULL) {
		fprintf(stderr, "usage: %s --addr-file PATH --key K [--osize N] "
			"[--expect-value V] [--expect-status S]\n", argv[0]);
		return 2;
	}

	char target[512];
	if (read_addr_file(addr_file, target, sizeof(target)) != 0) {
		fprintf(stderr, "test: cannot read addr-file %s\n", addr_file);
		return 1;
	}

	struct nkvx_front *front = NULL;
	int rc = nkvx_front_init(na_init, target, &front);
	if (rc != 0) {
		fprintf(stderr, "test: nkvx_front_init failed: %d\n", rc);
		return 1;
	}

	size_t klen = strlen(key);
	void *out_buf = calloc(1, (size_t)osize ? (size_t)osize : 1);
	struct done_ctx d;
	memset(&d, 0, sizeof(d));

	nkvx_kv_in_t in;
	memset(&in, 0, sizeof(in));
	in.verb = NKVX_KV_VERB_RETRIEVE;
	in.read_only = 1;
	in.key_len = (uint8_t)klen;
	memcpy(in.key, key, klen);
	in.osize = (uint32_t)osize;

	rc = nkvx_front_kv_forward(front, &in, out_buf, (uint32_t)osize, done_cb, &d);
	if (rc != 0) {
		fprintf(stderr, "test: nkvx_front_kv_forward failed: %d\n", rc);
		free(out_buf);
		nkvx_front_fini(front);
		return 1;
	}

	int spins = 0;
	while (!d.done && spins++ < 100000) {
		nkvx_front_progress(front, 50);
	}
	if (!d.done) {
		fprintf(stderr, "test: timed out waiting for completion\n");
		free(out_buf);
		nkvx_front_fini(front);
		return 1;
	}

	printf("test: RETRIEVE key='%s' -> status=%d result_len(DW0)=%u delivered=%u\n",
	       key, d.status, d.result_len, d.value_n);

	int result = 0;
	if ((int)d.status != expect_status) {
		fprintf(stderr, "test: FAIL status %d != expected %d\n", d.status, expect_status);
		result = 1;
	}
	if (result == 0 && expect_value != NULL) {
		size_t vlen = strlen(expect_value);
		/* The value lands in out_buf (inline-copied by the bridge done-cb path; in
		 * this driver the done_cb captured the inline bytes directly). On the PUSH
		 * path the value is in out_buf instead. Compare against whichever holds it. */
		const unsigned char *got = d.value_n > 0 ? d.value : (unsigned char *)out_buf;
		uint32_t got_n = d.value_n > 0 ? d.value_n : d.result_len;

		if (d.result_len != (uint32_t)vlen) {
			fprintf(stderr, "test: FAIL DW0 %u != true value len %zu\n",
				d.result_len, vlen);
			result = 1;
		} else if (got_n < vlen || memcmp(got, expect_value, vlen) != 0) {
			fprintf(stderr, "test: FAIL value mismatch\n");
			result = 1;
		}
	}
	if (result == 0) {
		printf("test: PASS\n");
	}

	free(out_buf);
	nkvx_front_fini(front);
	return result;
}
