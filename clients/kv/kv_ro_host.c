/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Read-only KV namespace host (initiator) for the per-namespace read-only
 * enforcement test (ADR-0008 trust split).
 *
 * It attaches to an NVMf controller over the VFIOUSER transport, finds the
 * first Key-Value namespace (CSI == SPDK_NVME_CSI_KV), and asserts the
 * read-only trust split enforced by lib/nvmf/ctrlr_kvdev.c:
 *
 *   - KV Store  MUST be REJECTED with Command-Specific status
 *               "Attempted Write to Read Only Range" (sct=0x01, sc=0x82).
 *   - KV Delete MUST be REJECTED with the same status.
 *   - KV Exec   MUST be REJECTED with the same status.
 *   - KV Exist  MUST be ALLOWED (a read passes); for an absent key it returns
 *               the normal "key does not exist" status (sct=0x00, sc=0x87).
 *
 * Usage: kv_ro_host <vfio-user-socket-dir>
 *
 * Prints "kv_ro_host: PASS" and exits 0 on success.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvme_kv.h"
#include "spdk/string.h"

static const char g_key[] = "rokey001";
static const char g_value[] = "loaders may read but never write";

struct kv_ctx {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct spdk_nvme_qpair	*qpair;
	volatile bool		done;
	volatile uint8_t	last_sct;
	volatile uint8_t	last_sc;
};

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct kv_ctx *ctx = cb_ctx;

	ctx->ctrlr = ctrlr;
}

static void
io_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct kv_ctx *ctx = arg;

	ctx->last_sct = cpl->status.sct;
	ctx->last_sc = cpl->status.sc;
	ctx->done = true;
}

static void
wait_done(struct kv_ctx *ctx)
{
	while (!ctx->done) {
		spdk_nvme_qpair_process_completions(ctx->qpair, 0);
	}
}

/* Expected reject status for write/compute ops on a read-only KV namespace. */
#define RO_REJECT_SCT SPDK_NVME_SCT_COMMAND_SPECIFIC		/* 0x01 */
#define RO_REJECT_SC  SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE	/* 0x82 */

static bool
expect_status(struct kv_ctx *ctx, const char *what, uint8_t sct, uint8_t sc)
{
	if (ctx->last_sct != sct || ctx->last_sc != sc) {
		fprintf(stderr, "FAIL: %s: expected sct=0x%02x sc=0x%02x, got sct=0x%02x sc=0x%02x\n",
			what, sct, sc, ctx->last_sct, ctx->last_sc);
		return false;
	}
	fprintf(stderr, "PASS: %s (sct=0x%02x sc=0x%02x)\n", what, sct, sc);
	return true;
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	struct spdk_nvme_transport_id trid = {};
	struct kv_ctx ctx = {};
	uint32_t nsid;
	char *store_buf = NULL;
	const uint32_t buf_len = 256;
	int rc = 1;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <vfio-user-socket-path>\n", argv[0]);
		return 1;
	}

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "kv_ro_host";
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	trid.trtype = SPDK_NVME_TRANSPORT_VFIOUSER;
	snprintf(trid.traddr, sizeof(trid.traddr), "%s", argv[1]);

	if (spdk_nvme_probe(&trid, &ctx, probe_cb, attach_cb, NULL) != 0 || ctx.ctrlr == NULL) {
		fprintf(stderr, "spdk_nvme_probe() failed for '%s'\n", trid.traddr);
		goto out;
	}

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctx.ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctx.ctrlr, nsid)) {
		struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctx.ctrlr, nsid);

		if (ns && spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_KV) {
			ctx.ns = ns;
			break;
		}
	}
	if (ctx.ns == NULL) {
		fprintf(stderr, "No KV namespace found on controller\n");
		goto detach;
	}
	fprintf(stderr, "Found KV namespace nsid=%u\n", spdk_nvme_ns_get_id(ctx.ns));

	ctx.qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctx.ctrlr, NULL, 0);
	if (ctx.qpair == NULL) {
		fprintf(stderr, "Failed to allocate IO qpair\n");
		goto detach;
	}

	store_buf = spdk_dma_zmalloc(buf_len, 0, NULL);
	if (store_buf == NULL) {
		fprintf(stderr, "Failed to allocate DMA buffer\n");
		goto free_qpair;
	}
	memcpy(store_buf, g_value, sizeof(g_value));

	rc = 0;

	/* 1) KV Store MUST be rejected (write op on a read-only namespace). */
	ctx.done = false;
	if (spdk_nvme_kv_store(ctx.ns, ctx.qpair, g_key, strlen(g_key),
			       store_buf, sizeof(g_value), io_complete, &ctx, 0) != 0) {
		fprintf(stderr, "KV Store submit failed\n");
		rc = 1;
		goto free_buf;
	}
	wait_done(&ctx);
	if (!expect_status(&ctx, "KV Store rejected on read-only ns", RO_REJECT_SCT, RO_REJECT_SC)) {
		rc = 1;
	}

	/* 2) KV Exist MUST be allowed (a read passes). The key was never stored
	 * (the Store above was rejected), so the device reports the normal
	 * "key does not exist" status — proving the read reached the backend. */
	ctx.done = false;
	if (spdk_nvme_kv_exist(ctx.ns, ctx.qpair, g_key, strlen(g_key), io_complete, &ctx) != 0) {
		fprintf(stderr, "KV Exist submit failed\n");
		rc = 1;
		goto free_buf;
	}
	wait_done(&ctx);
	if (!expect_status(&ctx, "KV Exist allowed on read-only ns (absent key)",
			   SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST)) {
		rc = 1;
	}

	/* 3) KV Delete MUST be rejected (write op). */
	ctx.done = false;
	if (spdk_nvme_kv_delete(ctx.ns, ctx.qpair, g_key, strlen(g_key), io_complete, &ctx) != 0) {
		fprintf(stderr, "KV Delete submit failed\n");
		rc = 1;
		goto free_buf;
	}
	wait_done(&ctx);
	if (!expect_status(&ctx, "KV Delete rejected on read-only ns", RO_REJECT_SCT, RO_REJECT_SC)) {
		rc = 1;
	}

	/* 4) KV Exec MUST be rejected (compute op, can mutate the value). */
	ctx.done = false;
	if (spdk_nvme_kv_exec(ctx.ns, ctx.qpair, g_key, strlen(g_key), 1,
			      store_buf, sizeof(g_value), store_buf, buf_len,
			      io_complete, &ctx) != 0) {
		fprintf(stderr, "KV Exec submit failed\n");
		rc = 1;
		goto free_buf;
	}
	wait_done(&ctx);
	if (!expect_status(&ctx, "KV Exec rejected on read-only ns", RO_REJECT_SCT, RO_REJECT_SC)) {
		rc = 1;
	}

free_buf:
	spdk_free(store_buf);
free_qpair:
	spdk_nvme_ctrlr_free_io_qpair(ctx.qpair);
detach:
	spdk_nvme_detach(ctx.ctrlr);
out:
	if (rc == 0) {
		fprintf(stderr, "kv_ro_host: PASS\n");
	} else {
		fprintf(stderr, "kv_ro_host: FAIL\n");
	}
	return rc;
}
