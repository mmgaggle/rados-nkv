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
 * nvfu_submit_poll() (and thus nvfu_kv_xfer_sgl / the Exec submit) returns the raw
 * NVMe completion status encoded as `sc | (sct << 8)`, and ALWAYS fills *out_cpl
 * (cdw0 included) first. A KV Retrieve/Exec whose value exceeds the host buffer
 * completes BUFFER_TOO_SMALL — the forwarder maps SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL
 * to {SCT_COMMAND_SPECIFIC, SC_CAPACITY_EXCEEDED} (target/bdev_kvrados.c) and reports
 * the TRUE value length in cdw0 (ADR-0014 truncation). This is NOT a transport error:
 * the caller must read cdw0 and re-issue with a right-sized buffer. Classify it so the
 * shim surfaces the recoverable short read (return 0, *got = cdw0) instead of swallowing
 * it as -EIO — which left the datapath.rs size-probe unable to learn the true length and
 * so never re-reading values > RETRIEVE_HINT (bead spdk-kbh).
 */
#define NVFU_STATUS_BUFFER_TOO_SMALL \
	((int)(SPDK_NVME_SC_CAPACITY_EXCEEDED | (SPDK_NVME_SCT_COMMAND_SPECIFIC << 8)))

static inline bool
nvfu_status_is_buffer_too_small(int status)
{
	return status == NVFU_STATUS_BUFFER_TOO_SMALL;
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
 * length comes back in cpl.cdw0; the target reports it even when the host buffer is
 * too small — that case completes BUFFER_TOO_SMALL ({SCT_COMMAND_SPECIFIC,
 * SC_CAPACITY_EXCEEDED}), NOT SUCCESS, with the full length in cdw0 (ADR-0014
 * truncation; see kvrados_kvdev_status_to_nvme in target/bdev_kvrados.c). We copy at
 * most out_len bytes into the caller's buffer but report the TRUE length in *got and
 * return success so the caller can detect a short buffer (*got > out_len) and re-read
 * with a right-sized buffer instead of silently truncating (bead spdk-jhk.7.9). Any
 * OTHER nonzero status (transport error, negative errno, or a different NVMe error) is
 * a hard failure (-EIO). Treating BUFFER_TOO_SMALL as -EIO is what broke the > 4 MiB
 * size-probe re-read (bead spdk-kbh).
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
	if (rc != 0 && !nvfu_status_is_buffer_too_small(rc)) {
		/* Real failure (transport / unexpected NVMe error). BUFFER_TOO_SMALL falls
		 * through: it is a recoverable short read whose cdw0 carries the true len. */
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
 * KV Exist (opcode 0x14): presence probe carrying NO value body. EXIST is a
 * NO-DATA opcode (0x14 & 3 == 0 -> SPDK_NVME_DATA_NONE), so nvmf/vfio-user maps
 * NO DPTR for it (lib/nvmf/vfio_user.c map_io_cmd_req returns early on
 * DATA_NONE). The in-payload [u16 key_len][key] head that Store/Retrieve/Exec use
 * therefore can never reach the target for EXIST -- it would arrive as key_len==0
 * and the forwarder would reject it INVALID_FIELD (bead spdk-qzm root cause).
 *
 * So we frame the key INLINE in the command CDW slots (CDW2/3 = key[0..7],
 * CDW14/15 = key[8..15], length in CDW11.KL) -- the canonical short-key framing
 * (mirrors lib/nvme nvme_kv_cmd_set_key); the matching forwarder read is
 * kvrados_read_cdw_key in target/bdev_kvrados.c. The CDWs are always present
 * regardless of data direction, so no DPTR/SGL/DMA buffer is needed. This caps
 * the EXIST key at the inline 16-byte spec max (SPDK_NVME_KV_KEY_MAX_LEN); a
 * longer key is rejected here rather than silently truncated.
 *
 * Completion status decides presence: nvfu_submit_poll returns sc | (sct << 8).
 *   - 0x000 (SUCCESS, generic)                         -> present, *present=1
 *   - 0x187 (KV Key Does Not Exist, command-specific)  -> absent,  *present=0
 *   - anything else / negative                         -> transport/target error
 * On a present key cpl.cdw0 carries the FULL stored value length (CQE DW0,
 * ADR-0014 truncation semantics -- see kvrados_retrieve_done), surfaced in *len.
 * Returns 0 on a definitive present/absent answer (status surfaced via *present),
 * negative errno on a setup/transport error or an out-of-range key.
 */
int
nkvx_exist(nkvx_session *s, uint32_t nsid, const char *key,
	   int *present, uint32_t *len)
{
	struct nvfu_dev *d;
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_cpl cpl;
	size_t key_len;
	int status;

	if (s == NULL || key == NULL) {
		return -EINVAL;
	}
	d = &s->dev;
	key_len = strlen(key);
	/* EXIST has no DPTR to carry a long key; the inline CDW slots hold <= 16 B. */
	if (key_len < SPDK_NVME_KV_KEY_MIN_LEN || key_len > SPDK_NVME_KV_KEY_MAX_LEN) {
		fprintf(stderr,
			"KV Exist: key length %zu out of inline range [%d,%d]; EXIST keys "
			"ride the command CDW slots (no DPTR), so they cap at %d bytes\n",
			key_len, SPDK_NVME_KV_KEY_MIN_LEN, SPDK_NVME_KV_KEY_MAX_LEN,
			SPDK_NVME_KV_KEY_MAX_LEN);
		return -EINVAL;
	}

	memset(&cmd, 0, sizeof(cmd));
	memset(&cpl, 0, sizeof(cpl));

	cmd.opc = SPDK_NVME_OPC_KV_EXIST;
	cmd.nsid = nsid;
	cmd.cdw10_bits.kv.vsize = 0;		/* no value transfer */
	cmd.cdw11_bits.kv.kl = (uint8_t)key_len;	/* inline key length */
	/* Inline key: bytes 0..7 in CDW2/CDW3, bytes 8..15 in CDW14/CDW15. */
	memcpy(&cmd.cdw2, key, spdk_min(key_len, (size_t)8));
	if (key_len > 8) {
		memcpy(&cmd.cdw14, key + 8, key_len - 8);
	}

	/* No DPTR: EXIST transfers no data, so submit the bare command. */
	status = nvfu_submit_poll(d, &d->io, &cmd, &cpl);
	if (status < 0) {
		return status;	/* transport/setup error (timeout, etc.) */
	}
	if (status == 0) {
		if (present != NULL) {
			*present = 1;
		}
		if (len != NULL) {
			*len = cpl.cdw0;
		}
		return 0;
	}
	if (status == ((SPDK_NVME_SCT_COMMAND_SPECIFIC << 8) |
		       SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST)) {
		if (present != NULL) {
			*present = 0;
		}
		if (len != NULL) {
			*len = 0;
		}
		return 0;
	}
	/* Any other NVMe status is an unexpected error, not a clean present/absent. */
	fprintf(stderr, "KV Exist nsid=%u failed: status=0x%x\n", nsid, status);
	return -EIO;
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
	void *head, *val;
	uint64_t head_iova = 0, val_iova = 0;
	uint8_t key_len;
	uint16_t klp;
	uint32_t head_len, val_len, n;
	int status, err = 0;

	if (s == NULL || key == NULL) {
		return -EINVAL;
	}
	d = &s->dev;
	key_len = (uint8_t)strlen(key);
	klp = key_len;
	head_len = (uint32_t)sizeof(uint16_t) + key_len;

	/*
	 * Mirror the WORKING retrieve/store path (nvfu_kv_xfer_sgl): stage the
	 * [u16 key_len][key] head in its OWN small DMA region and the value/result buffer
	 * SEPARATELY, described by nvfu_sgl_set_dptr_kv as [head block][value blocks]. The
	 * forwarder then sees value_off == head_len at the head of the SECOND SGL block,
	 * so the value region (input in, result out) starts at val offset 0 — NOT at a
	 * head_len offset inside one combined buffer. The old single-buffer nvfu_sgl_set_dptr
	 * staging put the result at buf+head_len while sizing the buffer/osize as out_len,
	 * so osize over-promised by head_len, the result sink was short, and the read ran
	 * head_len bytes past the buffer (large exec returned 0 / wrong length, bead spdk-4i7).
	 *
	 * The value region carries the INPUT on the way in and the RESULT on the way out;
	 * the forwarder maps max(vsize, osize) of it. Size the buffer to hold both. The
	 * SGL VALUE size is osize (out_len) so the result-sink capacity the executor sees
	 * is exactly osize (cdw12), matching what we report back.
	 */
	memset(&cmd, 0, sizeof(cmd));
	memset(&cpl, 0, sizeof(cpl));

	val_len = spdk_max(in_len, out_len);
	val_len = spdk_max(val_len, 1u);	/* nvfu_dma_alloc rejects size 0 */

	head = nvfu_dma_alloc(head_len, &head_iova);
	if (head == NULL) {
		return -ENOMEM;
	}
	memcpy(head, &klp, sizeof(klp));
	memcpy((char *)head + sizeof(klp), key, key_len);

	val = nvfu_dma_alloc(val_len, &val_iova);
	if (val == NULL) {
		spdk_dma_free(head);
		return -ENOMEM;
	}
	/* Stage the input at the head of the value region (the forwarder reads input from
	 * value_off; result is scattered back over the same region). Zero any tail beyond
	 * the input so a short input + larger osize sink starts clean. */
	if (in_len > 0) {
		memcpy(val, in, in_len);
	}
	if (val_len > in_len) {
		memset((char *)val + in_len, 0, val_len - in_len);
	}
	spdk_wmb();

	cmd.opc = SPDK_NVME_OPC_KV_EXEC;
	cmd.nsid = nsid;
	cmd.cdw10_bits.kv.vsize = in_len;	/* VALUE (input) size only, not the key head */
	cmd.cdw11_bits.kv.kl = 0;		/* in-payload long-key signal (ADR-0014) */
	/* KV Exec osize/op_id were named bitfields (full-width :32) in the old SPDK
	 * fork; the out-of-tree base SPDK (Gerrit 28298) has no kv_exec cdw members,
	 * so write the raw CDW words — byte-identical wire output. (See the same fix
	 * in clients/nvme-kv/kv/vfu_host/nkv_vfu.h.) */
	cmd.cdw12 = out_len;		/* KV Exec output buffer size = osize (vendor ext) */
	cmd.cdw13 = op_id;		/* KV Exec op_id (vendor ext) */

	/* [head block][value blocks]: the value region (max(in_len,out_len)) is described
	 * region-bounded so a > 2 MiB input/result scatters correctly. */
	segs = nvfu_sgl_set_dptr_kv(&cmd, head_iova, head_len, val_iova, val_len, &err);
	if (err != 0) {
		spdk_dma_free(val);
		spdk_dma_free(head);
		return err;
	}

	status = nvfu_submit_poll(d, &d->io, &cmd, &cpl);
	if (segs != NULL) {
		spdk_dma_free(segs);
	}
	if (status != 0 && !nvfu_status_is_buffer_too_small(status)) {
		fprintf(stderr, "KV Exec op %u failed: status=0x%x\n", op_id, status);
		spdk_dma_free(val);
		spdk_dma_free(head);
		return -EIO;
	}
	/* BUFFER_TOO_SMALL is a recoverable short result (cdw0 = true length): fall
	 * through and report it in *rlen so the datapath.rs size-probe re-runs into a
	 * right-sized buffer, exactly like nkvx_retrieve (bead spdk-kbh). */
	spdk_rmb();
	/* Copy only what fits (min(true_len, our sink capacity)), but report the TRUE
	 * length in *rlen so the caller can size-probe + re-read. The result lands at the
	 * head of the SEPARATE value region (offset 0), exactly like nkvx_retrieve reads
	 * from buf[0] — there is no key-head offset inside the value buffer. */
	n = spdk_min(cpl.cdw0, out_len);
	if (n > 0) {
		memcpy(out, val, n);
	}
	if (rlen != NULL) {
		*rlen = cpl.cdw0;
	}
	spdk_dma_free(val);
	spdk_dma_free(head);
	return 0;
}
