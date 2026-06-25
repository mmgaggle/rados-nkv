/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * FFI shim implementation for the rados-nkv Rust CLI (bead spdk-jhk.7.1).
 *
 * Includes the modified driver header directly (the nsid-threaded copy in
 * ../../nvme-kv/kv/vfu_host/nkv_vfu.h) and provides the CPU nvfu_produce() that the header
 * declares but does not define -- copied verbatim from nkv_vfu_host.c so the CLI
 * drives the datapath from the CPU (the GPU path is a separate, flagged route).
 *
 * One process == one vfio-user session: spdk_env_init() runs at most once per
 * process (EAL is process-global), then each nkvx_open() attaches a controller.
 */

#include "nkvx_shim.h"

#include "../../nvme-kv/kv/vfu_host/nkv_vfu.h"

#include "spdk/log.h"

/* CPU producer: write the SQE into the SQ slot, fence, ring the SQ doorbell.
 * Copied verbatim from nkv_vfu_host.c.
 *
 * When the crate is built with the `gpu-native` feature, the HIP TU
 * (nkvx_gpu.hip) provides the single, GPU-gated nvfu_produce for the whole
 * binary (it falls back to this exact CPU path when g_use_gpu is unset, which
 * covers controller bring-up and the CPU datapath). Compiling it out here under
 * -DNKVX_GPU_NATIVE avoids a duplicate-symbol link error; the default build
 * (no feature) keeps this definition and never compiles the HIP TU. */
#ifndef NKVX_GPU_NATIVE
int
nvfu_produce(struct nvfu_dev *d, struct nvfu_queue *q,
	     const struct spdk_nvme_cmd *cmd, uint16_t slot, uint16_t new_tail)
{
	q->sq[slot] = *cmd;
	spdk_wmb();
	d->doorbells[q->sq_db] = new_tail;
	return 0;
}
#endif

struct nkvx_session {
	struct nvfu_dev dev;
};

/* EAL is process-global; spdk_env_init() must run exactly once per process. */
static int g_env_inited;

static int
nkvx_env_init_once(void)
{
	struct spdk_env_opts opts;

	if (g_env_inited) {
		return 0;
	}
	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "rados-nkv";
	opts.env_context = (void *)"--iova-mode=va";
	/*
	 * Single-file hugepage segments (bead spdk-6ar). A retrieve/exec host buffer
	 * (RETRIEVE_HINT = 4 MiB) is VA-contiguous but spans multiple 2 MiB hugepages.
	 * SPDK's vfio-user client registers that span as ONE DMA region carrying a
	 * SINGLE (fd, offset) (vfio_mr_map_notify -> spdk_mem_get_fd_and_offset of the
	 * region start), and the vfio-user target mmaps the whole region MAP_SHARED
	 * from that one fd. With the default one-file-per-page layout only the FIRST
	 * hugepage maps to the client's real memory; the controller's writes to the
	 * value buffer (2nd+ hugepage) land on unshared pages, so the client reads
	 * zeros. Single-file segments back the memseg list with ONE fd at contiguous
	 * offsets, so the target's single-fd mmap covers the whole region. The two
	 * single-file modes are mutually exclusive in DPDK, so unlink must be off.
	 */
	opts.hugepage_single_segments = true;
	opts.unlink_hugepage = false;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return -EIO;
	}
	g_env_inited = 1;
	return 0;
}

nkvx_session *
nkvx_open(const char *traddr_dir)
{
	struct nkvx_session *s;

	if (nkvx_env_init_once() != 0) {
		return NULL;
	}
	s = (struct nkvx_session *)calloc(1, sizeof(*s));
	if (s == NULL) {
		return NULL;
	}
	if (nvfu_open(&s->dev, traddr_dir) != 0) {
		free(s);
		return NULL;
	}
	return s;
}

void
nkvx_close(nkvx_session *s)
{
	if (s == NULL) {
		return;
	}
	nvfu_close(&s->dev);
	free(s);
}

/*
 * Store via the region-bounded SGL path (nvfu_kv_xfer_sgl) instead of the
 * single-PRP nvfu_kv_store, which caps transfers at one 4096-byte page (target
 * returns status 0x6 above that). The SGL path allows up to the controller
 * max_io_size (64 MiB). We allocate one DMA buffer, stage the value, and let the
 * driver describe it with one region-bounded data block per 2 MiB DMA region.
 */
int
nkvx_store(nkvx_session *s, uint32_t nsid, const char *key,
	   const void *val, uint32_t len, uint8_t store_opt, uint32_t ttl)
{
	void *buf;
	uint64_t iova;
	int rc;

	if (s == NULL || key == NULL) {
		return -EINVAL;
	}
	/* spdk_dma_zmalloc rejects size 0; allocate at least one byte. */
	buf = nvfu_dma_alloc(spdk_max(len, 1), &iova);
	if (buf == NULL) {
		return -ENOMEM;
	}
	if (len > 0) {
		memcpy(buf, val, len);
	}
	spdk_wmb();
	/* store_opt carries the CDW11 Store Option byte (SIKE/SINKE, TTL_VALID,
	 * EPHEMERAL, TOUCH); ttl the CDW12 TTL seconds (spdk-jhk.7.14). */
	rc = nvfu_kv_xfer_sgl(&s->dev, nsid, SPDK_NVME_OPC_KV_STORE, key, len, iova,
			      store_opt, ttl, NULL);
	spdk_dma_free(buf);
	if (rc != 0) {
		return -EIO;
	}
	return 0;
}

/*
 * Retrieve via the region-bounded SGL path (mirrors nkvx_store). The TRUE value
 * length comes back in cpl.cdw0; the target reports it even when the host buffer
 * is too small (BUFFER_TOO_SMALL still completes with SUCCESS and the full length
 * in cdw0 -- see lib/nvmf/ctrlr_kvdev.c). We copy at most out_len bytes into the
 * caller's buffer but report the TRUE length in *got, so the caller can detect a
 * short buffer (*got > out_len) and re-read with a right-sized buffer instead of
 * silently truncating (bead spdk-jhk.7.9).
 */
int
nkvx_retrieve(nkvx_session *s, uint32_t nsid, const char *key,
	      void *out, uint32_t out_len, uint32_t *got)
{
	struct spdk_nvme_cpl cpl;
	void *buf;
	uint64_t iova;
	uint32_t n;
	int rc;

	if (s == NULL || key == NULL) {
		return -EINVAL;
	}
	memset(&cpl, 0, sizeof(cpl));
	buf = nvfu_dma_alloc(spdk_max(out_len, 1), &iova);
	if (buf == NULL) {
		return -ENOMEM;
	}
	rc = nvfu_kv_xfer_sgl(&s->dev, nsid, SPDK_NVME_OPC_KV_RETRIEVE, key, out_len, iova, 0, 0, &cpl);
	if (rc != 0) {
		spdk_dma_free(buf);
		return -EIO;
	}
	spdk_rmb();
	/* Copy only what fits, but report the TRUE length so the caller can detect
	 * and recover from a short buffer rather than truncating silently. */
	n = spdk_min(cpl.cdw0, out_len);
	if (n > 0) {
		memcpy(out, buf, n);
	}
	if (got != NULL) {
		*got = cpl.cdw0;
	}
	spdk_dma_free(buf);
	return 0;
}

/*
 * KV Exec mirroring nkvx_retrieve's short-buffer contract (bead spdk-jhk.7.10).
 *
 * nvfu_kv_exec() (nkv_vfu.h) clamps its result_len to out_len, which the GPU/CPU
 * self-tests rely on (they iterate up to result_len into a fixed buffer). The CLI
 * needs the opposite: the TRUE result length even when the host buffer is too
 * small, so it can size-probe and re-read instead of silently truncating. So we
 * inline the exec staging here (rather than calling nvfu_kv_exec) and report the
 * unclamped cpl.cdw0 in *rlen, copying at most out_len bytes into *out. Leaving
 * nvfu_kv_exec untouched keeps the GPU client's clamped contract intact.
 */
int
nkvx_exec(nkvx_session *s, uint32_t nsid, const char *key, uint32_t op_id,
	  const void *in, uint32_t in_len,
	  void *out, uint32_t out_len, uint32_t *rlen)
{
	struct nvfu_dev *d;
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_cpl cpl;
	struct spdk_nvme_sgl_descriptor *segs;
	void *buf;
	uint64_t iova;
	uint8_t key_len;
	uint16_t klp;
	uint32_t payload_len, bufsz, xfer_len, n;
	int status, err = 0;

	if (s == NULL || key == NULL) {
		return -EINVAL;
	}
	d = &s->dev;
	key_len = (uint8_t)strlen(key);
	klp = key_len;
	payload_len = (uint32_t)sizeof(uint16_t) + key_len + in_len;
	bufsz = spdk_max(out_len, 4096);
	/* Target maps max(vsize, osize): input gathered in, result scattered back. */
	xfer_len = spdk_max(payload_len, out_len);

	memset(&cmd, 0, sizeof(cmd));
	memset(&cpl, 0, sizeof(cpl));
	if (payload_len > bufsz) {
		return -EINVAL;
	}
	buf = nvfu_dma_alloc(bufsz, &iova);
	if (buf == NULL) {
		return -ENOMEM;
	}
	/* Stage [u16 key_len][key][input] at the buffer head. */
	memcpy(buf, &klp, sizeof(klp));
	memcpy((char *)buf + sizeof(klp), key, key_len);
	if (in_len) {
		memcpy((char *)buf + sizeof(klp) + key_len, in, in_len);
	}
	spdk_wmb();

	cmd.opc = SPDK_NVME_OPC_KV_EXEC;
	cmd.nsid = nsid;
	cmd.cdw10_bits.kv.vsize = payload_len;		/* request payload length */
	/* KV Exec osize/op_id were named bitfields (full-width :32) in the old SPDK
	 * fork; the out-of-tree base SPDK (Gerrit 28298) has no kv_exec cdw members,
	 * so write the raw CDW words — byte-identical wire output. (See the same fix
	 * in clients/nvme-kv/kv/vfu_host/nkv_vfu.h.) */
	cmd.cdw12 = out_len;		/* KV Exec output buffer size (vendor ext) */
	cmd.cdw13 = op_id;		/* KV Exec op_id (vendor ext) */

	segs = nvfu_sgl_set_dptr(&cmd, iova, xfer_len, &err);
	if (err != 0) {
		spdk_dma_free(buf);
		return err;
	}

	status = nvfu_submit_poll(d, &d->io, &cmd, &cpl);
	if (segs != NULL) {
		spdk_dma_free(segs);
	}
	if (status != 0) {
		fprintf(stderr, "KV Exec op %u failed: status=0x%x\n", op_id, status);
		spdk_dma_free(buf);
		return -EIO;
	}
	spdk_rmb();
	/* Copy only what fits, but report the TRUE length so the caller can detect
	 * and recover from a short buffer rather than truncating silently. The result
	 * lands in the payload region AFTER the in-payload key head (the forwarder's
	 * result sink is iovs[0] + value_off, value_off = sizeof(u16 key_len)+key_len),
	 * NOT at the buffer head — read it from the same offset (bead spdk-4jq). */
	n = spdk_min(cpl.cdw0, out_len);
	if (n > 0) {
		memcpy(out, (char *)buf + sizeof(klp) + key_len, n);
	}
	if (rlen != NULL) {
		*rlen = cpl.cdw0;
	}
	spdk_dma_free(buf);
	return 0;
}
