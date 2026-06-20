/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * GPU-direct NVMe-KV host shim (bead spdk-p9k.1.1, Rung 2).
 *
 * Productizes the proven Rung-1 datapath (gpu_direct_dmamap.hip) into a small,
 * reusable C ABI that the vLLM loader's Python side drives over ctypes
 * (clients/vllm-weights/src/rados_nkv_weights/_kvshim_gpu.py).
 *
 * The whole point of Rung 2: a host-side NVMe-KV Retrieve DMAs *straight into a
 * hipMalloc GPU buffer* (no host bounce, no hipMemcpy on the datapath), and the
 * loader wraps that GPU device pointer as a device-resident torch.Tensor. So
 * vLLM's model.load_weights() never does an implicit H2D copy.
 *
 * Mechanism (same as Rung 1, see gpu_direct/README.md):
 *   hipMalloc(&dptr, size)
 *   hsa_amd_portable_export_dmabuf(dptr, ...)       -> dma-buf fd + offset
 *   mmap(fd)                                         -> CPU view == iova/vaddr
 *   vfio_user_dev_dma_map_unmap(dev, &mr, true)      -> server mmaps SAME fd
 *   program mr.iova into the NVMe SGL/PRP; the Retrieve DMA lands in GPU pages.
 *
 * We use the raw vfio-user host client (nkv_vfu.h) rather than the SPDK-NVMe
 * kv_host_shim because nkv_vfu exposes the underlying `struct vfio_device *`
 * (d->dev), which is exactly what vfio_user_dev_dma_map_unmap needs to register
 * the dma-buf region directly. (kv_host_shim drives the SPDK NVMe driver layer,
 * which hides the vfio_device.) vfio_user_dev_dma_map_unmap is a defined T symbol
 * in libspdk_vfio_user.a, so it links from the static archive with no SPDK
 * source change.
 *
 * Catalog keys are raw 16-byte binary (KEY_LEN=16) and may contain embedded NUL
 * bytes, so this shim takes an explicit (key, key_len) and does NOT use
 * nkv_vfu.h's strlen()-based helpers; it builds the SQE key field directly via
 * nvfu_kv_set_key(cmd, key, key_len) (which is length-explicit) and a
 * region-bounded SGL DPTR into the GPU dma-buf region.
 */

#include <hip/hip_runtime.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

extern "C" {
#include "nkv_vfu.h"
#include "vfio_user_internal.h"
#include "spdk/log.h"
#include "spdk/env.h"
}

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/mman.h>

/*
 * nkv_vfu.h declares (not defines) nvfu_produce(): each consumer main provides
 * it. We are a library, not the dmamap prototype, so we provide the CPU producer
 * here (the host builds the SQE + rings the doorbell; only the DMA target is the
 * GPU dma-buf, which is the whole GPU-direct trick). No GPU-initiated doorbell is
 * needed for Rung 2 — the data lands in GPU memory regardless of who rings.
 */
extern "C" int
nvfu_produce(struct nvfu_dev *d, struct nvfu_queue *q,
	     const struct spdk_nvme_cmd *cmd, uint16_t slot, uint16_t new_tail)
{
	q->sq[slot] = *cmd;
	spdk_wmb();
	d->doorbells[q->sq_db] = new_tail;
	return 0;
}

/* Public opaque handle returned to Python. */
struct kvg_dev {
	struct nvfu_dev	nvfu;
	bool		owns_env;
};

/* A GPU buffer registered as a vfio-user DMA region via its dma-buf fd. */
struct kvg_gpu_buf {
	uint8_t				*dptr;		/* hipMalloc device pointer */
	void				*mmap;		/* CPU view == iova/vaddr */
	size_t				size;
	int				dmabuf_fd;
	uint64_t			dmabuf_off;
	struct vfio_memory_region	mr;
	struct kvg_dev			*owner;		/* dev whose region this is */
};

/* ---- one-time SPDK env + controller attach -------------------------------- */

extern "C" struct kvg_dev *
kvg_open(const char *vfu_addr)
{
	struct kvg_dev *g;
	struct spdk_env_opts opts;
	int ndev = 0;

	if (vfu_addr == NULL) {
		fprintf(stderr, "kvg_open: NULL vfu_addr\n");
		return NULL;
	}
	if (hipGetDeviceCount(&ndev) != hipSuccess || ndev < 1) {
		fprintf(stderr, "kvg_open: no HIP device available\n");
		return NULL;
	}

	g = (struct kvg_dev *)calloc(1, sizeof(*g));
	if (g == NULL) {
		return NULL;
	}

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "kvg_gpu";
	/* IOVA-as-VA: the dma-buf mmap vaddr is the IOVA we register/program. */
	opts.env_context = (void *)"--iova-mode=va";
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "kvg_open: spdk_env_init failed\n");
		free(g);
		return NULL;
	}
	g->owns_env = true;

	if (nvfu_open(&g->nvfu, vfu_addr) != 0) {
		fprintf(stderr, "kvg_open: nvfu_open(%s) failed\n", vfu_addr);
		spdk_env_fini();
		free(g);
		return NULL;
	}
	return g;
}

extern "C" void
kvg_close(struct kvg_dev *g)
{
	if (g == NULL) {
		return;
	}
	nvfu_close(&g->nvfu);
	if (g->owns_env) {
		spdk_env_fini();
	}
	free(g);
}

/* ---- GPU dma-buf region alloc / free -------------------------------------- */

/*
 * Allocate a `size`-byte hipMalloc GPU buffer and register it with the vfio-user
 * controller as a DMA region backed by its dma-buf fd (iova == the CPU mmap
 * vaddr; IOVA-as-VA). Returns an opaque kvg_gpu_buf* (NULL on failure). The
 * caller reads the GPU device pointer with kvg_buf_dptr().
 */
extern "C" struct kvg_gpu_buf *
kvg_buf_alloc(struct kvg_dev *g, size_t size)
{
	struct kvg_gpu_buf *b;
	hsa_status_t hs;

	if (g == NULL || size == 0) {
		return NULL;
	}
	b = (struct kvg_gpu_buf *)calloc(1, sizeof(*b));
	if (b == NULL) {
		return NULL;
	}
	b->size = size;
	b->dmabuf_fd = -1;
	b->owner = g;

	if (hipMalloc((void **)&b->dptr, size) != hipSuccess) {
		fprintf(stderr, "kvg_buf_alloc: hipMalloc(%zu) failed\n", size);
		free(b);
		return NULL;
	}

	hs = hsa_amd_portable_export_dmabuf(b->dptr, size, &b->dmabuf_fd, &b->dmabuf_off);
	if (hs != HSA_STATUS_SUCCESS) {
		const char *msg = NULL; hsa_status_string(hs, &msg);
		fprintf(stderr, "kvg_buf_alloc: export_dmabuf failed: %s\n", msg ? msg : "?");
		hipFree(b->dptr);
		free(b);
		return NULL;
	}

	b->mmap = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       b->dmabuf_fd, (off_t)b->dmabuf_off);
	if (b->mmap == MAP_FAILED) {
		fprintf(stderr, "kvg_buf_alloc: mmap(dma-buf) failed: %s\n", strerror(errno));
		close(b->dmabuf_fd);
		hipFree(b->dptr);
		free(b);
		return NULL;
	}

	/* Register the dma-buf fd as a single contiguous vfio-user DMA region.
	 * iova == vaddr == the CPU mmap address; the server mmaps the SAME fd at
	 * `offset` and serves DMA at [iova, iova+size). */
	b->mr.iova   = (uint64_t)(uintptr_t)b->mmap;
	b->mr.vaddr  = (uint64_t)(uintptr_t)b->mmap;
	b->mr.size   = size;
	b->mr.offset = b->dmabuf_off;
	b->mr.fd     = b->dmabuf_fd;

	if (vfio_user_dev_dma_map_unmap(g->nvfu.dev, &b->mr, true) != 0) {
		fprintf(stderr, "kvg_buf_alloc: vfio_user_dev_dma_map_unmap(map) FAILED\n");
		munmap(b->mmap, size);
		close(b->dmabuf_fd);
		hipFree(b->dptr);
		free(b);
		return NULL;
	}
	return b;
}

extern "C" void
kvg_buf_free(struct kvg_gpu_buf *b)
{
	if (b == NULL) {
		return;
	}
	if (b->dmabuf_fd >= 0) {
		vfio_user_dev_dma_map_unmap(b->owner->nvfu.dev, &b->mr, false);
		munmap(b->mmap, b->size);
		close(b->dmabuf_fd);
		hipFree(b->dptr);
		b->dmabuf_fd = -1;
	}
	free(b);
}

/* The GPU (hipMalloc) device pointer backing this region. */
extern "C" void *
kvg_buf_dptr(struct kvg_gpu_buf *b)
{
	return b ? (void *)b->dptr : NULL;
}

/* ---- GPU-direct KV Retrieve (binary key) ---------------------------------- */

/*
 * KV Retrieve the value for the raw `key` (`key_len` bytes, may contain NULs)
 * directly INTO the GPU dma-buf region `b`, up to `b->size` bytes. The DMA writes
 * the GPU pages; no host bounce. On success returns 0 and writes the device's
 * TRUE value length (cpl.cdw0) to *got_len; a positive NVMe status code or
 * negated errno otherwise.
 *
 * Uses a length-explicit key (NOT nkv_vfu.h's strlen() helpers) and a
 * region-bounded SGL DPTR over the GPU region so a value up to the controller
 * max (64 MiB) scatters correctly across the single contiguous dma-buf region.
 */
extern "C" int
kvg_retrieve_gpu(struct kvg_dev *g, uint32_t nsid, const void *key, uint8_t key_len,
		 struct kvg_gpu_buf *b, uint32_t *got_len)
{
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_cpl cpl;
	struct spdk_nvme_sgl_descriptor *segs;
	int rc, err = 0;

	if (g == NULL || b == NULL || key == NULL) {
		return -EINVAL;
	}
	if (nsid == 0) {
		nsid = KV_NSID;
	}

	memset(&cmd, 0, sizeof(cmd));
	memset(&cpl, 0, sizeof(cpl));
	cmd.opc = SPDK_NVME_OPC_KV_RETRIEVE;
	cmd.nsid = nsid;
	cmd.cdw10_bits.kv.vsize = (uint32_t)b->size;
	/* Length-explicit key (binary, may contain NUL). */
	nvfu_kv_set_key(&cmd, (const char *)key, key_len);

	/* Region-bounded SGL DPTR into the GPU dma-buf region (iova == b->mr.iova). */
	segs = nvfu_sgl_set_dptr(&cmd, b->mr.iova, (uint32_t)b->size, &err);
	if (err != 0) {
		return err;
	}

	rc = nvfu_submit_poll(&g->nvfu, &g->nvfu.io, &cmd, &cpl);
	if (segs != NULL) {
		spdk_dma_free(segs);
	}
	if (rc != 0) {
		/* nvfu_submit_poll returns sc | (sct << 8); a non-generic sct (high
		 * byte set) is reported as -EIO, otherwise the bare sc. */
		if (rc < 0) {
			return rc;	/* -ETIMEDOUT etc. */
		}
		if (rc & 0xff00) {
			return -EIO;
		}
		return rc;		/* generic NVMe status code, e.g. 0x87 not-found */
	}
	if (got_len != NULL) {
		*got_len = cpl.cdw0;
	}
	return 0;
}

/*
 * Convenience: copy `n` bytes out of the GPU region to a host buffer (D2H). Only
 * used by the self-test / verification path (the datapath itself never bounces);
 * the loader instead wraps b's device pointer as a torch device tensor.
 */
extern "C" int
kvg_buf_copy_to_host(struct kvg_gpu_buf *b, void *host, size_t n)
{
	if (b == NULL || host == NULL || n > b->size) {
		return -EINVAL;
	}
	return (hipMemcpy(host, b->dptr, n, hipMemcpyDeviceToHost) == hipSuccess) ? 0 : -EIO;
}

/* Max value length advertised by the bound KV namespace (kvvml). nkv_vfu does
 * not cache it, so callers that need it size to the controller max_io_size; this
 * returns 0 as "unknown" and the Python side falls back to the catalog cap. */
extern "C" uint32_t
kvg_max_value_len(struct kvg_dev *g)
{
	(void)g;
	return 0;
}
