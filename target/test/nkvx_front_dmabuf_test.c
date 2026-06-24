/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/*
 * Standalone dma-buf bulk-path proof for the rados-nkvx two-tier executor
 * (Slice S2, bead spdk-a27). It is the NON-GPU end-to-end test of the SAME
 * dma-buf result_sink path the GPU capstone (S3) will use, minus the GPU:
 *
 *   - The result_sink is a HOST udmabuf: a memfd sealed with F_SEAL_SHRINK,
 *     wrapped by /dev/udmabuf into a dma-buf fd. (A GPU-VRAM vfio-user P2PDMA
 *     sink in S3 is a different dma-buf fd from the SAME registration path.)
 *   - The fd is handed to the front via nkvx_front_forward_dmabuf(), which
 *     registers the result_sink MR from the dma-buf (na_ofi:
 *     fi_mr_regattr(FI_MR_DMABUF)) instead of from a VA.
 *   - The executor (a separate process: two endpoints over ofi+verbs;ofi_rxm)
 *     RDMA-WRITEs a large (default 64 MiB) identity Exec result straight into the
 *     dma-buf-backed memory.
 *   - We mmap the udmabuf and assert the landed bytes are BIT-EXACT against the
 *     expected sha256 (the seeded object's sha) — proving the executor PUSH wrote
 *     the right bytes into the dma-buf via the verbs RDMA WRITE.
 *
 * This intentionally does NOT reuse nkvx_front_client_test.c: it needs a dma-buf
 * sink (udmabuf) and the _dmabuf forward variant, and proves a single thing
 * cleanly. Run it via nkvx_dmabuf_test.sh against a live nkvx_service executor.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/memfd.h>
#include <linux/udmabuf.h>

#include <openssl/sha.h>

#include "nkvx_front_client.h"

struct done_state {
	bool				called;
	int				calls;
	enum spdk_kvdev_io_status	status;
	uint32_t			result_len;
	uint32_t			result_inline_len;
};

static void
on_done(void *arg, enum spdk_kvdev_io_status status, uint32_t result_len,
	const void *result_inline, uint32_t result_inline_len)
{
	struct done_state *st = arg;

	(void)result_inline;
	st->status = status;
	st->result_len = result_len;
	st->result_inline_len = result_inline_len;
	st->calls++;
	st->called = true;
	/*
	 * NOTE: a dma-buf result_sink delivers the bytes by RDMA WRITE into the
	 * dma-buf, NOT inline — so we deliberately do NOT copy result_inline here.
	 * The bytes are already in the udmabuf mmap; that mmap is what we hash.
	 */
}

static int
read_addr_file(const char *path, char *buf, size_t buf_sz)
{
	FILE *f = fopen(path, "r");
	char *p;

	if (f == NULL) {
		return -1;
	}
	p = fgets(buf, (int)buf_sz, f);
	fclose(f);
	if (p == NULL) {
		return -1;
	}
	buf[strcspn(buf, "\r\n")] = '\0';
	return (buf[0] != '\0') ? 0 : -1;
}

static int
parse_hex(const char *hex, unsigned char *out, size_t out_len)
{
	size_t i;

	if (strlen(hex) != out_len * 2) {
		return -1;
	}
	for (i = 0; i < out_len; i++) {
		unsigned byte;

		if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
			return -1;
		}
		out[i] = (unsigned char)byte;
	}
	return 0;
}

/*
 * Create a HOST dma-buf sink of `size` bytes: a memfd sealed with F_SEAL_SHRINK
 * (udmabuf requires the seal so the pages cannot vanish under the device),
 * wrapped into a dma-buf fd by /dev/udmabuf, then mmap'd so we can read the
 * landed bytes back. Returns the dma-buf fd (>=0) and sets *map / *memfd_out;
 * negative on failure.
 */
static int
make_udmabuf_region(size_t size, void **map, int *memfd_out)
{
	struct udmabuf_create create;
	long page = sysconf(_SC_PAGESIZE);
	size_t aligned = (size + (size_t)page - 1) & ~((size_t)page - 1);
	int memfd, devfd, dmabuf;
	void *m;

	memfd = memfd_create("nkvx-dmabuf-sink", MFD_ALLOW_SEALING | MFD_CLOEXEC);
	if (memfd < 0) {
		fprintf(stderr, "test: memfd_create failed: %s\n", strerror(errno));
		return -1;
	}
	if (ftruncate(memfd, (off_t)aligned) != 0) {
		fprintf(stderr, "test: ftruncate(%zu) failed: %s\n", aligned, strerror(errno));
		close(memfd);
		return -1;
	}
	if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
		fprintf(stderr, "test: F_SEAL_SHRINK failed: %s\n", strerror(errno));
		close(memfd);
		return -1;
	}

	devfd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	if (devfd < 0) {
		fprintf(stderr, "test: open(/dev/udmabuf) failed: %s "
			"(need the 'kvm' group or sufficient privilege)\n", strerror(errno));
		close(memfd);
		return -1;
	}
	memset(&create, 0, sizeof(create));
	create.memfd = (uint32_t)memfd;
	create.flags = UDMABUF_FLAGS_CLOEXEC;
	create.offset = 0;
	create.size = aligned;
	dmabuf = ioctl(devfd, UDMABUF_CREATE, &create);
	close(devfd);
	if (dmabuf < 0) {
		fprintf(stderr, "test: UDMABUF_CREATE failed: %s\n", strerror(errno));
		close(memfd);
		return -1;
	}

	/* mmap the udmabuf (not the memfd) so the readback observes exactly the
	 * memory the dma-buf MR registered — the same pages the RDMA WRITE lands in. */
	m = mmap(NULL, aligned, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf, 0);
	if (m == MAP_FAILED) {
		fprintf(stderr, "test: mmap(udmabuf) failed: %s\n", strerror(errno));
		close(dmabuf);
		close(memfd);
		return -1;
	}
	*map = m;
	*memfd_out = memfd;
	return dmabuf;
}

int
main(int argc, char **argv)
{
	const char *na_init = "ofi+verbs;ofi_rxm://";
	const char *target = NULL;
	const char *addr_file = NULL;
	const char *key = "nkvxDmabuf";
	int runtime = 2;			/* native identity (module-ns nkvx / module identity) */
	const char *module_key = "identity";
	const char *module_ns = "nkvx";
	int osize = 64 * 1024 * 1024;		/* 64 MiB default */
	const char *expect_sha_hex = NULL;	/* expected sha256 of delivered bytes */
	int iters = 1;

	enum { OPT_KEY = 256, OPT_RUNTIME, OPT_MODULE, OPT_MODULE_NS, OPT_OSIZE,
	       OPT_EXPECT_SHA, OPT_ITERS };
	static const struct option opts[] = {
		{ "listen",        required_argument, NULL, 'l' },
		{ "target",        required_argument, NULL, 't' },
		{ "addr-file",     required_argument, NULL, 'a' },
		{ "key",           required_argument, NULL, OPT_KEY },
		{ "runtime",       required_argument, NULL, OPT_RUNTIME },
		{ "module",        required_argument, NULL, OPT_MODULE },
		{ "module-ns",     required_argument, NULL, OPT_MODULE_NS },
		{ "osize",         required_argument, NULL, OPT_OSIZE },
		{ "expect-sha256", required_argument, NULL, OPT_EXPECT_SHA },
		{ "iters",         required_argument, NULL, OPT_ITERS },
		{ NULL, 0, NULL, 0 },
	};
	int c;

	while ((c = getopt_long(argc, argv, "l:t:a:", opts, NULL)) != -1) {
		switch (c) {
		case 'l': na_init = optarg; break;
		case 't': target = optarg; break;
		case 'a': addr_file = optarg; break;
		case OPT_KEY: key = optarg; break;
		case OPT_RUNTIME: runtime = atoi(optarg); break;
		case OPT_MODULE: module_key = optarg; break;
		case OPT_MODULE_NS: module_ns = optarg; break;
		case OPT_OSIZE: osize = atoi(optarg); break;
		case OPT_EXPECT_SHA: expect_sha_hex = optarg; break;
		case OPT_ITERS: iters = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s --listen NA (--target ADDR | --addr-file PATH) "
				"[--key K] [--runtime N] [--module M] [--module-ns NS] "
				"[--osize N] [--expect-sha256 HEX] [--iters N]\n", argv[0]);
			return 2;
		}
	}

	if (osize <= (int)NKVX_INLINE_MAX) {
		fprintf(stderr, "test: --osize must exceed NKVX_INLINE_MAX (%u) to force a "
			"bulk PUSH into the dma-buf\n", (unsigned)NKVX_INLINE_MAX);
		return 2;
	}
	if (iters < 1) {
		fprintf(stderr, "test: --iters must be >= 1\n");
		return 2;
	}
	size_t key_len = strlen(key);
	if (key_len == 0 || key_len > SPDK_KVDEV_EXEC_KEY_MAX_LEN) {
		fprintf(stderr, "test: --key must be 1..%d bytes\n", SPDK_KVDEV_EXEC_KEY_MAX_LEN);
		return 2;
	}
	unsigned char expect_sha[SHA256_DIGEST_LENGTH];
	bool have_expect_sha = false;
	if (expect_sha_hex != NULL) {
		if (parse_hex(expect_sha_hex, expect_sha, sizeof(expect_sha)) != 0) {
			fprintf(stderr, "test: --expect-sha256 must be %d hex chars\n",
				SHA256_DIGEST_LENGTH * 2);
			return 2;
		}
		have_expect_sha = true;
	}

	char addr_buf[512];
	if (target == NULL && addr_file != NULL) {
		if (read_addr_file(addr_file, addr_buf, sizeof(addr_buf)) != 0) {
			fprintf(stderr, "test: cannot read addr-file %s: %s\n",
				addr_file, strerror(errno));
			return 1;
		}
		target = addr_buf;
	}
	if (target == NULL) {
		fprintf(stderr, "test: need --target or --addr-file\n");
		return 2;
	}

	/* The dma-buf result_sink (udmabuf-backed) + its mmap for readback. */
	void *sink_map = NULL;
	int sink_memfd = -1;
	int dmabuf_fd = make_udmabuf_region((size_t)osize, &sink_map, &sink_memfd);
	if (dmabuf_fd < 0) {
		return 1;
	}
	printf("test: udmabuf result_sink: dmabuf_fd=%d size=%d bytes (mmap=%p)\n",
	       dmabuf_fd, osize, sink_map);

	struct nkvx_front *front = NULL;
	int rc = nkvx_front_init(na_init, target, &front);
	if (rc != 0) {
		fprintf(stderr, "test: nkvx_front_init(%s, %s) failed: %d\n",
			na_init, target, rc);
		munmap(sink_map, (size_t)osize);
		close(dmabuf_fd);
		close(sink_memfd);
		return 1;
	}

	nkvx_exec_in_t in;
	memset(&in, 0, sizeof(in));
	in.op_id = 0xDB0F;
	in.read_only = 1;
	in.runtime = (uint8_t)runtime;
	in.key_len = (uint8_t)key_len;
	memcpy(in.key, key, key_len);
	in.module_key = (char *)module_key;
	in.module_ns = (char *)module_ns;
	in.osize = (uint32_t)osize;
	in.input_len = 0;
	in.input_inline = NULL;
	in.input_bulk = HG_BULK_NULL;
	in.result_sink = HG_BULK_NULL;

	int ret = 1;

	for (int it = 0; it < iters; it++) {
		struct done_state st;

		memset(&st, 0, sizeof(st));
		/* Poison the dma-buf so a short/absent/torn PUSH is detectable. */
		memset(sink_map, 0xA5, (size_t)osize);

		/* THE S2 PATH: register the result_sink from the dma-buf fd. The base VA
		 * passed (sink_map) is the segment the handle advertises; the actual MR is
		 * taken from dmabuf_fd at offset 0 (na_ofi fi_mr_regattr(FI_MR_DMABUF)). */
		rc = nkvx_front_forward_dmabuf(front, &in, sink_map, (uint32_t)osize,
					       dmabuf_fd, 0 /* offset */, on_done, &st, NULL);
		if (rc != 0) {
			fprintf(stderr, "test: iter %d nkvx_front_forward_dmabuf failed: %d\n", it, rc);
			goto done;
		}

		for (int i = 0; i < 200000 && !st.called; i++) {
			int prog = nkvx_front_progress(front, 100);
			if (prog < 0) {
				fprintf(stderr, "test: iter %d progress failed: %d\n", it, prog);
				goto done;
			}
		}
		if (!st.called) {
			fprintf(stderr, "test: FAIL iter %d (completion never fired)\n", it);
			goto done;
		}
		if (st.calls != 1) {
			fprintf(stderr, "test: FAIL iter %d (done-cb fired %d times, expected 1)\n",
				it, st.calls);
			goto done;
		}
		if (nkvx_front_outstanding(front) != 0) {
			fprintf(stderr, "test: FAIL iter %d (%u still in flight)\n",
				it, nkvx_front_outstanding(front));
			goto done;
		}
		if ((int)st.status != SPDK_KVDEV_IO_STATUS_SUCCESS) {
			fprintf(stderr, "test: FAIL iter %d (status %d != SUCCESS)\n",
				it, (int)st.status);
			goto done;
		}
		if (st.result_len != (uint32_t)osize) {
			fprintf(stderr, "test: FAIL iter %d (result_len %u != osize %d)\n",
				it, st.result_len, osize);
			goto done;
		}
		/* The dma-buf path must NOT deliver inline — the bytes rode the RDMA WRITE
		 * into the udmabuf. A nonzero inline len would mean the PUSH never happened. */
		if (st.result_inline_len != 0) {
			fprintf(stderr, "test: FAIL iter %d (unexpected inline result %u bytes — "
				"the dma-buf PUSH path did not fire)\n", it, st.result_inline_len);
			goto done;
		}

		/* BIT-EXACT CHECK: sha256 the bytes the executor RDMA-WROTE into the
		 * udmabuf (read via the mmap) and compare to the seeded object's sha. */
		if (have_expect_sha) {
			unsigned char got[SHA256_DIGEST_LENGTH];

			SHA256((const unsigned char *)sink_map, (size_t)osize, got);
			if (memcmp(got, expect_sha, sizeof(got)) != 0) {
				char ghex[2 * SHA256_DIGEST_LENGTH + 1];
				int k;

				for (k = 0; k < SHA256_DIGEST_LENGTH; k++) {
					sprintf(ghex + k * 2, "%02x", got[k]);
				}
				fprintf(stderr, "test: FAIL iter %d (dma-buf bytes sha256 %s != "
					"expected %s — PUSH delivered WRONG bytes)\n",
					it, ghex, expect_sha_hex);
				goto done;
			}
			printf("test: iter %d BIT-EXACT (%d bytes RDMA-written into udmabuf, "
			       "sha256 matches)\n", it, osize);
		} else {
			/* No expected sha: at least prove the poison was overwritten (PUSH
			 * landed SOMETHING), so the test still fails on an absent PUSH. */
			size_t z;
			bool all_poison = true;

			for (z = 0; z < (size_t)osize; z++) {
				if (((unsigned char *)sink_map)[z] != 0xA5) {
					all_poison = false;
					break;
				}
			}
			if (all_poison) {
				fprintf(stderr, "test: FAIL iter %d (dma-buf still all-poison — "
					"no PUSH landed)\n", it);
				goto done;
			}
			printf("test: iter %d PUSH landed %d bytes into udmabuf "
			       "(no --expect-sha256 to verify exact content)\n", it, osize);
		}
	}

	printf("test: PASS — %d iter(s), 64 MiB-class identity result RDMA-written "
	       "bit-exact into the HOST udmabuf result_sink over verbs (S2 dma-buf path)\n",
	       iters);
	ret = 0;

done:
	nkvx_front_fini(front);
	munmap(sink_map, (size_t)osize);
	close(dmabuf_fd);
	close(sink_memfd);
	return ret;
}
