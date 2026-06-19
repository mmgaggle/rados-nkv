/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Native HIP GPU-initiated datapath for the rados-nkv CLI (bead spdk-jhk.7.15).
 *
 * This file is compiled and linked ONLY when the crate is built with the
 * `gpu-native` cargo feature (`cargo build --release --features gpu-native`).
 * The default build never compiles it, so the default `rados-nkv` binary stays
 * ROCm-free and `--gpu` continues to delegate to the separate `nkv_vfu_gpu`
 * subprocess (see src/gpu.rs).
 *
 * When the feature IS on, `--gpu` routes through this in-process native path:
 * the GPU builds the 64-byte NVMe SQE into the IO SQ ring and rings the SQ
 * doorbell directly from a device kernel -- exactly mirroring nkv_vfu_gpu.hip's
 * nvfu_produce() / nvfu_ring_kernel() -- instead of spawning a subprocess.
 *
 * It reuses the SAME driver (nkv_vfu.h: attach, admin queue, controller enable,
 * IO queue creation, completion polling, the region-bounded SGL transfer path)
 * as both the CPU shim and the standalone GPU binary. The ONLY GPU-specific
 * piece is nvfu_produce(): when g_use_gpu is set and the op is on the IO queue,
 * a HIP kernel stages the SQE + fences + rings the doorbell; otherwise it falls
 * back to the CPU producer (used for one-time controller bring-up).
 *
 * Because nkv_vfu.h declares exactly one non-static `nvfu_produce`, this TU
 * provides it for the whole binary. The CPU shim (nkvx_shim.c) compiles its own
 * nvfu_produce out under -DNKVX_GPU_NATIVE so there is no duplicate symbol; the
 * CPU datapath then shares this gated producer (with g_use_gpu == 0).
 */

#include <hip/hip_runtime.h>

extern "C" {
#include "../../kv/vfu_host/nkv_vfu.h"
#include "spdk/log.h"
#include "nkvx_gpu.h"
}

/* GPU-visible aliases of the IO SQ and the BAR0 doorbell page, plus the gate. */
static struct spdk_nvme_cmd	*g_sq_dev;
static volatile uint32_t		*g_db_dev;
static int			g_use_gpu;

/*
 * One GPU thread builds the submission: copy the 64-byte SQE into the SQ slot,
 * fence to system scope so the controller sees it, then ring the doorbell and
 * fence again. Identical to nkv_vfu_gpu.hip's nvfu_ring_kernel.
 */
__global__ void
nkvx_ring_kernel(struct spdk_nvme_cmd *sq, uint16_t slot, struct spdk_nvme_cmd sqe,
		 volatile uint32_t *db, uint32_t db_idx, uint16_t new_tail)
{
	if (threadIdx.x != 0 || blockIdx.x != 0) {
		return;
	}
	sq[slot] = sqe;
	__threadfence_system();
	db[db_idx] = new_tail;
	__threadfence_system();
}

/*
 * The single producer for this binary. Admin / bring-up commands and the CPU
 * datapath (g_use_gpu == 0) go through the plain CPU store+fence+ring. Only the
 * IO KV op, once g_use_gpu is set, is GPU-initiated via the device kernel.
 * Mirrors nkv_vfu_gpu.hip's nvfu_produce exactly.
 */
extern "C" int
nvfu_produce(struct nvfu_dev *d, struct nvfu_queue *q,
	     const struct spdk_nvme_cmd *cmd, uint16_t slot, uint16_t new_tail)
{
	hipError_t e;

	if (!g_use_gpu || q->qid != IO_QID) {
		q->sq[slot] = *cmd;
		spdk_wmb();
		d->doorbells[q->sq_db] = new_tail;
		return 0;
	}

	nkvx_ring_kernel<<<1, 1>>>(g_sq_dev, slot, *cmd, g_db_dev, q->sq_db, new_tail);
	e = hipDeviceSynchronize();
	if (e != hipSuccess) {
		fprintf(stderr, "GPU ring kernel failed: %s\n", hipGetErrorString(e));
		return -EIO;
	}
	return 0;
}

/* Map the IO SQ and the doorbell page into the GPU's address space. Mirrors
 * nkv_vfu_gpu.hip's gpu_map(). */
static int
nkvx_gpu_map(struct nvfu_dev *d)
{
	size_t sq_bytes = (size_t)d->io.depth * sizeof(struct spdk_nvme_cmd);
	hipError_t e;

	e = hipHostRegister(d->io.sq, sq_bytes, hipHostRegisterMapped);
	if (e != hipSuccess) {
		fprintf(stderr, "hipHostRegister(IO SQ) failed: %s\n", hipGetErrorString(e));
		return -EIO;
	}
	e = hipHostGetDevicePointer((void **)&g_sq_dev, d->io.sq, 0);
	if (e != hipSuccess) {
		fprintf(stderr, "GetDevicePointer(SQ): %s\n", hipGetErrorString(e));
		return -EIO;
	}

	/* The doorbell page is a vfio-user shared mapping; try normal mapped
	 * registration first, then I/O-memory registration as a fallback. */
	e = hipHostRegister((void *)d->doorbells, 0x1000, hipHostRegisterMapped);
	if (e != hipSuccess) {
		e = hipHostRegister((void *)d->doorbells, 0x1000, hipHostRegisterIoMemory);
	}
	if (e != hipSuccess) {
		fprintf(stderr, "hipHostRegister(doorbell page) failed: %s\n",
			hipGetErrorString(e));
		return -EIO;
	}
	e = hipHostGetDevicePointer((void **)&g_db_dev, (void *)d->doorbells, 0);
	if (e != hipSuccess) {
		fprintf(stderr, "GetDevicePointer(doorbell): %s\n", hipGetErrorString(e));
		return -EIO;
	}
	return 0;
}

/* Session wrapper: the same opaque-handle shape as the CPU shim, but with the
 * GPU SQ/doorbell mapping done at open and the GPU producer armed for IO ops. */
struct nkvx_gpu_session {
	struct nvfu_dev dev;
};

/* EAL is process-global; spdk_env_init() must run exactly once per process. */
static int g_env_inited;

static int
nkvx_gpu_env_init_once(void)
{
	struct spdk_env_opts opts;

	if (g_env_inited) {
		return 0;
	}
	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "rados-nkv";
	opts.env_context = (void *)"--iova-mode=va";
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return -EIO;
	}
	g_env_inited = 1;
	return 0;
}

extern "C" nkvx_gpu_session *
nkvx_gpu_open(const char *traddr_dir)
{
	struct nkvx_gpu_session *s;
	int ndev = 0;

	if (hipGetDeviceCount(&ndev) != hipSuccess || ndev < 1) {
		fprintf(stderr, "no HIP device available\n");
		return NULL;
	}
	if (nkvx_gpu_env_init_once() != 0) {
		return NULL;
	}
	s = (struct nkvx_gpu_session *)calloc(1, sizeof(*s));
	if (s == NULL) {
		return NULL;
	}
	/* CPU bring-up (g_use_gpu == 0): admin queue + IO queue creation use the
	 * CPU branch of nvfu_produce. */
	if (nvfu_open(&s->dev, traddr_dir) != 0) {
		free(s);
		return NULL;
	}
	if (nkvx_gpu_map(&s->dev) != 0) {
		nvfu_close(&s->dev);
		free(s);
		return NULL;
	}
	g_use_gpu = 1;	/* IO KV ops below are GPU-initiated */
	return s;
}

extern "C" void
nkvx_gpu_close(nkvx_gpu_session *s)
{
	if (s == NULL) {
		return;
	}
	g_use_gpu = 0;
	if (g_sq_dev) {
		hipHostUnregister(s->dev.io.sq);
		g_sq_dev = NULL;
	}
	nvfu_close(&s->dev);
	free(s);
}

/*
 * GPU-initiated KV Store via the region-bounded SGL path. The value is staged
 * into a DMA buffer; nvfu_kv_xfer_sgl builds the SGL DPTR + SQE; the GPU rings
 * the doorbell (via nvfu_produce with g_use_gpu set). Same SGL path the CPU shim
 * uses, so it scales to the controller max_io_size (64 MiB).
 */
extern "C" int
nkvx_gpu_store(nkvx_gpu_session *s, uint32_t nsid, const char *key,
	       const void *val, uint32_t len)
{
	void *buf;
	uint64_t iova;
	int rc;

	if (s == NULL || key == NULL) {
		return -EINVAL;
	}
	buf = nvfu_dma_alloc(spdk_max(len, 1), &iova);
	if (buf == NULL) {
		return -ENOMEM;
	}
	if (len > 0) {
		memcpy(buf, val, len);
	}
	spdk_wmb();
	rc = nvfu_kv_xfer_sgl(&s->dev, nsid, SPDK_NVME_OPC_KV_STORE, key, len, iova, 0, 0, NULL);
	spdk_dma_free(buf);
	return (rc != 0) ? -EIO : 0;
}

/*
 * GPU-initiated KV Retrieve via the region-bounded SGL path. Reports the TRUE
 * value length in *got (unclamped cpl.cdw0) so the Rust caller can size-probe +
 * re-read instead of truncating -- same short-buffer contract as the CPU shim's
 * nkvx_retrieve (bead spdk-jhk.7.9).
 */
extern "C" int
nkvx_gpu_retrieve(nkvx_gpu_session *s, uint32_t nsid, const char *key,
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
 * GPU-initiated KV Exec. Stages [u16 key_len][key][input] at the buffer head,
 * builds the SGL DPTR, and submits via the GPU producer. Reports the TRUE result
 * length in *rlen (unclamped) -- same short-buffer contract as the CPU shim's
 * nkvx_exec (bead spdk-jhk.7.10). Inlined here (not nvfu_kv_exec) to keep the
 * unclamped length, mirroring nkvx_shim.c::nkvx_exec.
 */
extern "C" int
nkvx_gpu_exec(nkvx_gpu_session *s, uint32_t nsid, const char *key, uint32_t op_id,
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
	memcpy(buf, &klp, sizeof(klp));
	memcpy((char *)buf + sizeof(klp), key, key_len);
	if (in_len) {
		memcpy((char *)buf + sizeof(klp) + key_len, in, in_len);
	}
	spdk_wmb();

	cmd.opc = SPDK_NVME_OPC_KV_EXEC;
	cmd.nsid = nsid;
	cmd.cdw10_bits.kv.vsize = payload_len;
	cmd.cdw12_bits.kv_exec.osize = out_len;
	cmd.cdw13_bits.kv_exec.op_id = op_id;

	segs = nvfu_sgl_set_dptr(&cmd, iova, xfer_len, &err);
	if (err != 0) {
		spdk_dma_free(buf);
		return err;
	}

	/* nvfu_submit_poll -> nvfu_produce (GPU-initiated, g_use_gpu set). */
	status = nvfu_submit_poll(d, &d->io, &cmd, &cpl);
	if (segs != NULL) {
		spdk_dma_free(segs);
	}
	if (status != 0) {
		fprintf(stderr, "GPU KV Exec op %u failed: status=0x%x\n", op_id, status);
		spdk_dma_free(buf);
		return -EIO;
	}
	spdk_rmb();
	n = spdk_min(cpl.cdw0, out_len);
	if (n > 0) {
		memcpy(out, buf, n);
	}
	if (rlen != NULL) {
		*rlen = cpl.cdw0;
	}
	spdk_dma_free(buf);
	return 0;
}
