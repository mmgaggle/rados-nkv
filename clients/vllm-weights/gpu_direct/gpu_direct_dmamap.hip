/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * TRUE host-side GPU-direct NVMe-KV Retrieve (bead spdk-p9k.1, Rung 1).
 *
 * Strategy B realized: a host-side NVMe-KV Retrieve DMAs straight into a
 * hipMalloc GPU buffer -- NO host bounce, NO spdk_dma_zmalloc staging.
 *
 * The probe gpu_direct_probe.hip established (on gfx1151 UMA):
 *   - hsa_amd_portable_export_dmabuf(hipMalloc ptr) succeeds.
 *   - the resulting dma-buf is CPU-mmap'able AND coherent in both directions
 *     (GPU write <-> CPU mmap read, CPU mmap write -> GPU read).
 *   - the mmap vaddr is NOT a DPDK memseg, so SPDK's implicit region
 *     registration (vfio_mr_map_notify, keyed on spdk_mem_get_fd_and_offset)
 *     cannot map it.
 *
 * This program closes that gap by registering the dma-buf as a vfio-user DMA
 * region DIRECTLY: it hands the dma-buf fd + offset + the mmap vaddr (used as
 * iova == vaddr, IOVA-as-VA per ADR-0016) to vfio_user_dev_dma_map_unmap().
 * The vfio-user server then mmap()s the SAME dma-buf fd on its side, so an NVMe
 * Store/Retrieve DMA lands in the exact physical pages the GPU sees.
 *
 * Because a single hipMalloc dma-buf is ONE fd with contiguous offsets (unlike
 * a multi-hugepage spdk_dma_zmalloc buffer, whose 2 MiB segments have DISTINCT
 * fds), it maps as a single contiguous region -- so this also avoids the 2 MiB
 * store cliff that the spdk_dma_zmalloc path hits.
 *
 * Usage: gpu_direct_dmamap <traddr-dir-containing-cntrl> [maxsize_bytes]
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
#include <sys/mman.h>

extern "C" int
nvfu_produce(struct nvfu_dev *d, struct nvfu_queue *q,
	     const struct spdk_nvme_cmd *cmd, uint16_t slot, uint16_t new_tail)
{
	q->sq[slot] = *cmd;
	spdk_wmb();
	d->doorbells[q->sq_db] = new_tail;
	return 0;
}

static inline uint8_t pat(uint64_t i) { return (uint8_t)((i * 131u + 7u) & 0xffu); }

__global__ void
fill_pattern(uint8_t *buf, uint32_t len)
{
	uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
	if (i < len) buf[i] = (uint8_t)((i * 131u + 7u) & 0xffu);
}

__global__ void
verify_pattern(const uint8_t *buf, uint32_t len, uint32_t *bad)
{
	uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
	if (i < len) {
		uint8_t want = (uint8_t)((i * 131u + 7u) & 0xffu);
		if (buf[i] != want) atomicAdd(bad, 1u);
	}
}

/* A GPU buffer registered as a vfio-user DMA region via its dma-buf fd. */
struct gpu_dma_buf {
	uint8_t			*dptr;		/* hipMalloc device pointer (==GPU addr) */
	void			*mmap;		/* CPU view of the same pages (==iova/vaddr) */
	size_t			size;
	int			dmabuf_fd;
	uint64_t		dmabuf_off;
	struct vfio_memory_region mr;
};

/*
 * Allocate a GPU buffer and register it with the vfio-user controller as a DMA
 * region backed by the buffer's dma-buf fd. iova == the CPU mmap vaddr (so the
 * SGL/PRP address we program is a valid IOVA the server resolves to the SAME
 * dma-buf pages).
 */
static int
gpu_dma_buf_alloc(struct nvfu_dev *d, size_t size, struct gpu_dma_buf *g)
{
	memset(g, 0, sizeof(*g));
	g->size = size;
	g->dmabuf_fd = -1;

	if (hipMalloc((void **)&g->dptr, size) != hipSuccess) {
		fprintf(stderr, "hipMalloc(%zu) failed\n", size);
		return -1;
	}

	hsa_status_t hs = hsa_amd_portable_export_dmabuf(g->dptr, size, &g->dmabuf_fd, &g->dmabuf_off);
	if (hs != HSA_STATUS_SUCCESS) {
		const char *msg = NULL; hsa_status_string(hs, &msg);
		fprintf(stderr, "export_dmabuf failed: %s\n", msg ? msg : "?");
		hipFree(g->dptr);
		return -1;
	}

	/* CPU view of the SAME pages: the iova/vaddr we register and program. */
	g->mmap = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, g->dmabuf_fd, (off_t)g->dmabuf_off);
	if (g->mmap == MAP_FAILED) {
		fprintf(stderr, "mmap(dma-buf) failed: %s\n", strerror(errno));
		close(g->dmabuf_fd);
		hipFree(g->dptr);
		return -1;
	}

	/* Register the dma-buf fd as a single contiguous vfio-user DMA region.
	 * iova == vaddr == the CPU mmap address (IOVA-as-VA). The server mmaps the
	 * fd at `offset` and serves DMA at [iova, iova+size). */
	g->mr.iova   = (uint64_t)(uintptr_t)g->mmap;
	g->mr.vaddr  = (uint64_t)(uintptr_t)g->mmap;
	g->mr.size   = size;
	g->mr.offset = g->dmabuf_off;
	g->mr.fd     = g->dmabuf_fd;

	if (vfio_user_dev_dma_map_unmap(d->dev, &g->mr, true) != 0) {
		fprintf(stderr, "vfio_user_dev_dma_map_unmap(map) FAILED for dma-buf region\n");
		munmap(g->mmap, size);
		close(g->dmabuf_fd);
		hipFree(g->dptr);
		return -1;
	}
	return 0;
}

static void
gpu_dma_buf_free(struct nvfu_dev *d, struct gpu_dma_buf *g)
{
	if (g->dmabuf_fd >= 0) {
		vfio_user_dev_dma_map_unmap(d->dev, &g->mr, false);
		munmap(g->mmap, g->size);
		close(g->dmabuf_fd);
		hipFree(g->dptr);
		g->dmabuf_fd = -1;
	}
}

static int g_size_ladder[] = {
	4096, 16384, 65536, 262144, 1048576, 2097152, 4194304, 8388608, 16777216, 33554432, 67108864
};

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <traddr-dir-containing-cntrl> [maxsize_bytes]\n", argv[0]);
		return 1;
	}
	uint32_t maxsize = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 0) : (16u * 1024u * 1024u);

	int ndev = 0;
	if (hipGetDeviceCount(&ndev) != hipSuccess || ndev < 1) {
		fprintf(stderr, "no HIP device\n");
		return 1;
	}
	hipDeviceProp_t prop;
	(void)hipGetDeviceProperties(&prop, 0);
	printf("GPU: %s (gcnArch=%s)\n", prop.name, prop.gcnArchName);

	struct spdk_env_opts opts;
	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "gpu_direct_dmamap";
	opts.env_context = (void *)"--iova-mode=va";
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "spdk_env_init failed\n");
		return 1;
	}

	struct nvfu_dev d;
	memset(&d, 0, sizeof(d));
	if (nvfu_open(&d, argv[1]) != 0) {
		fprintf(stderr, "nvfu_open failed\n");
		return 1;
	}

	printf("\n=== TRUE GPU-DIRECT: Store from GPU buf, Retrieve INTO GPU buf (dma-buf DMA region) ===\n");
	int overall = 0;
	uint32_t *bad_dev = NULL;
	(void)hipMalloc((void **)&bad_dev, sizeof(uint32_t));

	for (size_t s = 0; s < sizeof(g_size_ladder) / sizeof(g_size_ladder[0]); s++) {
		uint32_t size = g_size_ladder[s];
		if (size > maxsize) break;
		char key[32];
		snprintf(key, sizeof(key), "GD_%u", size);

		/* Two GPU buffers: src (store source) and dst (retrieve sink). Both are
		 * pure hipMalloc device memory registered as dma-buf DMA regions. */
		struct gpu_dma_buf src, dst;
		if (gpu_dma_buf_alloc(&d, size, &src) != 0) { overall = -1; break; }
		if (gpu_dma_buf_alloc(&d, size, &dst) != 0) { gpu_dma_buf_free(&d, &src); overall = -1; break; }

		/* GPU produces the pattern directly in src device memory. */
		uint32_t threads = 256, blocks = (size + threads - 1) / threads;
		if (blocks > 65535) blocks = 65535;
		hipLaunchKernelGGL(fill_pattern, dim3(blocks), dim3(threads), 0, 0, src.dptr, size);
		(void)hipDeviceSynchronize();

		/* Store FROM the GPU buffer: DMA reads the GPU pages via the dma-buf
		 * region. iova == src.mmap vaddr. */
		int rc = nvfu_kv_xfer_sgl(&d, KV_NSID, SPDK_NVME_OPC_KV_STORE, key, size,
					  src.mr.iova, 0, 0, NULL);
		if (rc != 0) {
			fprintf(stderr, "  [%u] Store(from GPU) failed rc=%d\n", size, rc);
			gpu_dma_buf_free(&d, &dst); gpu_dma_buf_free(&d, &src);
			overall = -1; continue;
		}

		/* Retrieve INTO the (different) GPU buffer: DMA writes the GPU pages. */
		struct spdk_nvme_cpl cpl;
		memset(&cpl, 0, sizeof(cpl));
		rc = nvfu_kv_xfer_sgl(&d, KV_NSID, SPDK_NVME_OPC_KV_RETRIEVE, key, size,
				      dst.mr.iova, 0, 0, &cpl);
		if (rc != 0) {
			fprintf(stderr, "  [%u] Retrieve(into GPU) failed rc=%d\n", size, rc);
			gpu_dma_buf_free(&d, &dst); gpu_dma_buf_free(&d, &src);
			overall = -1; continue;
		}
		uint32_t got = cpl.cdw0;

		/* Verify ON THE GPU that dst now holds the pattern. The kernel reads
		 * dst.dptr, the GPU device pointer -- proving the host Retrieve landed
		 * byte-exact in GPU-visible memory with no host bounce. */
		uint32_t zero = 0;
		(void)hipMemcpy(bad_dev, &zero, sizeof(zero), hipMemcpyHostToDevice);
		hipLaunchKernelGGL(verify_pattern, dim3(blocks), dim3(threads), 0, 0, dst.dptr, size, bad_dev);
		(void)hipDeviceSynchronize();
		uint32_t bad = 0;
		(void)hipMemcpy(&bad, bad_dev, sizeof(bad), hipMemcpyDeviceToHost);

		printf("  [%8u] Store(GPU src)+Retrieve(GPU dst) got=%u  GPU-verify dst -> %s (%u mismatches)\n",
		       size, got, (got == size && bad == 0) ? "BYTE-EXACT" : "MISMATCH", bad);
		if (!(got == size && bad == 0)) overall = -1;

		gpu_dma_buf_free(&d, &dst);
		gpu_dma_buf_free(&d, &src);
	}

	hipFree(bad_dev);
	nvfu_close(&d);
	printf("\n==== RESULT: TRUE GPU-direct host-side Retrieve %s ====\n",
	       overall == 0 ? "PASS (byte-exact into hipMalloc GPU memory)" : "FAIL");
	return overall == 0 ? 0 : 2;
}
