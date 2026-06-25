/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/*
 * Standalone STORE round-trip driver (slice spdk-7sr.17.18 / "B" verification).
 *
 * A non-SPDK Mercury ORIGIN that drives the SAME nkvx_front_client.c the integrated
 * bdev_kvrados front uses: it STOREs a value of a configurable length and then
 * RETRIEVEs it back, asserting byte-exactness and the CQE-DW0 (TRUE value length).
 * For value_len > NKVX_INLINE_MAX the STORE exercises the zero-copy value_bulk
 * RDMA-PULL added in slice B (front registers the value READ-mode; the executor
 * PULLs it before the backend write) and the RETRIEVE exercises the symmetric
 * result_sink PUSH. A small value rides inline both ways. Runs over na+sm against a
 * running nkvx_service started with --mem-object (no Ceph cluster needed).
 *
 * NOT part of the SPDK build graph; built by the nkvx_service Makefile target
 * nkvx_kv_store_test.
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
	unsigned char			inl[4096];	/* inline value bytes (small retrieve) */
	uint32_t			inl_n;
};

static void
done_cb(void *arg, enum spdk_kvdev_io_status status, uint32_t result_len,
	const void *result_inline, uint32_t result_inline_len)
{
	struct done_ctx *d = arg;

	d->status = status;
	d->result_len = result_len;
	d->inl_n = 0;
	if (result_inline != NULL && result_inline_len > 0) {
		uint32_t n = result_inline_len < sizeof(d->inl) ?
			result_inline_len : (uint32_t)sizeof(d->inl);
		memcpy(d->inl, result_inline, n);
		d->inl_n = n;
	}
	d->done = true;
}

static int
pump(struct nkvx_front *front, struct done_ctx *d)
{
	int spins = 0;

	while (!d->done && spins++ < 2000000) {
		nkvx_front_progress(front, 50);
	}
	return d->done ? 0 : -1;
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
	const char *key = "big";
	long value_len = 65536;			/* default: > NKVX_INLINE_MAX -> PULL/PUSH */

	enum { OPT_KEY = 256, OPT_ADDR_FILE, OPT_VALUE_LEN, OPT_LISTEN };
	static const struct option opts[] = {
		{ "listen",      required_argument, NULL, OPT_LISTEN },
		{ "addr-file",   required_argument, NULL, OPT_ADDR_FILE },
		{ "key",         required_argument, NULL, OPT_KEY },
		{ "value-len",   required_argument, NULL, OPT_VALUE_LEN },
		{ NULL, 0, NULL, 0 },
	};
	int c;
	while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (c) {
		case OPT_LISTEN: na_init = optarg; break;
		case OPT_ADDR_FILE: addr_file = optarg; break;
		case OPT_KEY: key = optarg; break;
		case OPT_VALUE_LEN: value_len = atol(optarg); break;
		default: return 2;
		}
	}
	if (addr_file == NULL || value_len < 0) {
		fprintf(stderr, "usage: %s --addr-file PATH [--key K] [--value-len N] "
			"[--listen NA]\n", argv[0]);
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

	/* Deterministic value pattern so the retrieve comparison needs only this buffer. */
	unsigned char *vbuf = malloc((size_t)value_len ? (size_t)value_len : 1);
	if (vbuf == NULL) {
		fprintf(stderr, "test: OOM for %ld-byte value\n", value_len);
		nkvx_front_fini(front);
		return 1;
	}
	for (long i = 0; i < value_len; i++) {
		vbuf[i] = (unsigned char)((i * 131u + 7u) & 0xff);
	}

	size_t klen = strlen(key);
	int result = 0;

	/* ---- STORE ---- */
	struct done_ctx ds;
	memset(&ds, 0, sizeof(ds));
	nkvx_kv_in_t sin;
	memset(&sin, 0, sizeof(sin));
	sin.verb = NKVX_KV_VERB_STORE;
	sin.read_only = 0;
	sin.key_len = (uint8_t)klen;
	memcpy(sin.key, key, klen);
	sin.value_len = (uint32_t)value_len;
	sin.value_inline = (value_len > 0) ? vbuf : NULL;

	rc = nkvx_front_kv_forward(front, &sin, NULL, 0, done_cb, &ds);
	if (rc != 0) {
		fprintf(stderr, "test: STORE forward failed: %d\n", rc);
		result = 1;
		goto out;
	}
	if (pump(front, &ds) != 0) {
		fprintf(stderr, "test: STORE timed out\n");
		result = 1;
		goto out;
	}
	if (ds.status != SPDK_KVDEV_IO_STATUS_SUCCESS) {
		fprintf(stderr, "test: FAIL STORE status %d != SUCCESS\n", ds.status);
		result = 1;
		goto out;
	}
	printf("test: STORE key='%s' value_len=%ld -> SUCCESS\n", key, value_len);

	/* ---- RETRIEVE back + compare ---- */
	unsigned char *out_buf = calloc(1, (size_t)value_len ? (size_t)value_len : 1);
	if (out_buf == NULL) {
		fprintf(stderr, "test: OOM for retrieve buffer\n");
		result = 1;
		goto out;
	}
	struct done_ctx dr;
	memset(&dr, 0, sizeof(dr));
	nkvx_kv_in_t rin;
	memset(&rin, 0, sizeof(rin));
	rin.verb = NKVX_KV_VERB_RETRIEVE;
	rin.read_only = 1;
	rin.key_len = (uint8_t)klen;
	memcpy(rin.key, key, klen);
	rin.osize = (uint32_t)value_len;

	rc = nkvx_front_kv_forward(front, &rin, out_buf, (uint32_t)value_len, done_cb, &dr);
	if (rc != 0) {
		fprintf(stderr, "test: RETRIEVE forward failed: %d\n", rc);
		free(out_buf);
		result = 1;
		goto out;
	}
	if (pump(front, &dr) != 0) {
		fprintf(stderr, "test: RETRIEVE timed out\n");
		free(out_buf);
		result = 1;
		goto out;
	}

	printf("test: RETRIEVE key='%s' -> status=%d result_len(DW0)=%u delivered_inline=%u\n",
	       key, dr.status, dr.result_len, dr.inl_n);

	if (dr.status != SPDK_KVDEV_IO_STATUS_SUCCESS) {
		fprintf(stderr, "test: FAIL RETRIEVE status %d != SUCCESS\n", dr.status);
		result = 1;
	} else if (dr.result_len != (uint32_t)value_len) {
		fprintf(stderr, "test: FAIL DW0 %u != value_len %ld\n", dr.result_len, value_len);
		result = 1;
	} else if (value_len > 0) {
		/* Large value landed in out_buf via PUSH; small value came back inline. */
		const unsigned char *got = dr.inl_n > 0 ? dr.inl : out_buf;
		if (memcmp(got, vbuf, (size_t)value_len) != 0) {
			fprintf(stderr, "test: FAIL value mismatch (%ld bytes)\n", value_len);
			result = 1;
		}
	}
	free(out_buf);

	if (result == 0) {
		printf("test: PASS (byte-exact %ld-byte store+retrieve)\n", value_len);
	}

out:
	free(vbuf);
	nkvx_front_fini(front);
	return result;
}
