/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Phase 2 / Milestone 2c (epic spdk-jhk, bead spdk-jhk.3): Option A --
 * rocm-xio as a DIRECT host vfio-user NVMe client, CPU-driven variant.
 *
 * Brings up an SPDK vfio-user NVMe-KV controller with our own admin queue,
 * creates an IO queue, and does a KV Store + Retrieve -- every SQE built by us
 * and every doorbell rung by us, over SPDK's raw lib/vfio_user client (NOT
 * lib/nvme), no QEMU. The GPU variant (2d, nkv_vfu_gpu.hip) shares all of this
 * via nkv_vfu.h and only swaps nvfu_produce() for a GPU kernel.
 *
 * Usage: nkv_vfu_host <traddr-dir-containing-cntrl>
 */

#include "nkv_vfu.h"

#include "spdk/log.h"

/* CPU producer: write the SQE into the SQ slot, fence, ring the SQ doorbell. */
int
nvfu_produce(struct nvfu_dev *d, struct nvfu_queue *q,
	     const struct spdk_nvme_cmd *cmd, uint16_t slot, uint16_t new_tail)
{
	q->sq[slot] = *cmd;
	spdk_wmb();
	d->doorbells[q->sq_db] = new_tail;
	return 0;
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	struct nvfu_dev d = {};
	char ealargs[64];
	const char *key = "kvkey01";
	const char *value = "the quick brown fox jumps over the lazy dog";
	char retrieved[256] = {};
	uint32_t got = 0;
	int rc = 1;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <traddr-dir-containing-cntrl>\n", argv[0]);
		return 1;
	}

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "nkv_vfu_host";
	snprintf(ealargs, sizeof(ealargs), "--iova-mode=va%s",
		 getenv("NKV_EAL_DEBUG") ? " --log-level=lib.eal:8" : "");
	opts.env_context = ealargs;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	if (nvfu_open(&d, argv[1]) != 0) {
		goto out;
	}

	if (nvfu_kv_store(&d, KV_NSID, key, value, (uint32_t)strlen(value)) != 0) {
		goto out;
	}
	printf("KV Store OK (key='%s', %zu bytes)\n", key, strlen(value));

	if (nvfu_kv_retrieve(&d, KV_NSID, key, retrieved, sizeof(retrieved) - 1, &got) != 0) {
		goto out;
	}
	printf("KV Retrieve OK (%u bytes): '%s'\n", got, retrieved);

	if (got != strlen(value) || memcmp(retrieved, value, got) != 0) {
		fprintf(stderr, "FAIL: value mismatch\n");
		goto out;
	}

	printf("Milestone 2c PASS: KV Store+Retrieve round-tripped byte-exact "
	       "through our own vfio-user client -- CPU built the SQE and rang the "
	       "doorbell, no QEMU, no lib/nvme.\n");
	rc = 0;

out:
	nvfu_close(&d);
	return rc;
}
