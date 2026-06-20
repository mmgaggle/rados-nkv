/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Host-side GPU-direct NVMe-KV Retrieve probe (bead spdk-p9k.1, Rung 1).
 *
 * Goal: prove a host-side NVMe-KV Retrieve lands byte-exact in a GPU-visible
 * buffer with NO host bounce, on the gfx1151 UMA iGPU, NO QEMU guest.
 *
 * The host vfio-user client (nkv_vfu.h) registers DMA regions out of SPDK env
 * memory and programs the buffer's *virtual address* as the IOVA (IOVA-as-VA;
 * ADR-0016). For "GPU-direct" we need ONE buffer that is simultaneously:
 *   (a) a valid SPDK DMA target (a registered vfio-user DMA region), and
 *   (b) a valid GPU device pointer the kernel/torch can read.
 *
 * On a UMA APU (VRAM == GTT == system RAM) a single physical allocation can be
 * both. This probe tests the candidate strategies and reports which holds.
 *
 *   STRATEGY A (host-buffer-as-GPU): allocate an SPDK-DMA buffer
 *     (spdk_dma_zmalloc, a DPDK memseg that registers cleanly + has a backing
 *     fd), then hipHostRegister(..., Mapped) it so the GPU gets a device
 *     pointer into the SAME physical pages. SPDK Retrieves into the host vaddr;
 *     a GPU kernel reads the device pointer. This is the path the proven GPU
 *     client (nkv_vfu_gpu.hip) already uses for its data buffers.
 *
 *   STRATEGY B (device-buffer-as-DMA-target): hipMalloc a device buffer, export
 *     it as a dma-buf (hsa_amd_portable_export_dmabuf), and try to make SPDK
 *     DMA into it. Tests spdk-avu open-Q1: is the UMA dma-buf CPU-mappable (the
 *     simple vaddr path) or must SPDK accept an fd-only region?
 *
 * Usage: gpu_direct_probe <traddr-dir-containing-cntrl> [maxsize_bytes]
 */

#include <hip/hip_runtime.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

extern "C" {
#include "nkv_vfu.h"
#include "spdk/log.h"
#include "spdk/env.h"
}

#include <vector>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>

#define CHECK_HIP(call) do { hipError_t _e = (call); if (_e != hipSuccess) { \
	fprintf(stderr, "HIP error %s:%d: %s\n", __FILE__, __LINE__, hipGetErrorString(_e)); \
	return -1; } } while (0)

/* CPU producer used by nkv_vfu.h's submit/poll (we drive the doorbell on the CPU;
 * the GPU is the data CONSUMER here, not the SQE producer). */
extern "C" int
nvfu_produce(struct nvfu_dev *d, struct nvfu_queue *q,
	     const struct spdk_nvme_cmd *cmd, uint16_t slot, uint16_t new_tail)
{
	q->sq[slot] = *cmd;
	spdk_wmb();
	d->doorbells[q->sq_db] = new_tail;
	return 0;
}

/* Deterministic test pattern. */
static __host__ __device__ inline uint8_t pat(uint64_t i) { return (uint8_t)((i * 131u + 7u) & 0xffu); }

/* GPU kernel: read the buffer the SPDK Retrieve wrote, recompute the expected
 * pattern on-device, and count mismatches. This proves the bytes are visible to
 * the GPU at the device pointer (NOT just on the host). */
__global__ void
verify_pattern(const uint8_t *buf, uint32_t len, uint32_t *bad)
{
	uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
	if (i < len) {
		uint8_t want = (uint8_t)((i * 131u + 7u) & 0xffu);
		if (buf[i] != want) {
			atomicAdd(bad, 1u);
		}
	}
}

/* Fill a device buffer with the pattern, on the GPU. */
__global__ void
fill_pattern(uint8_t *buf, uint32_t len)
{
	uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
	if (i < len) {
		buf[i] = (uint8_t)((i * 131u + 7u) & 0xffu);
	}
}

static int g_size_ladder[] = {
	4096, 16384, 65536, 262144, 1048576, 2097152, 4194304, 8388608, 16777216
};

/*
 * STRATEGY A: SPDK-DMA host buffer made GPU-visible via hipHostRegister.
 *
 * Store a CPU-built pattern, Retrieve it back into the SAME GPU-mapped buffer,
 * then run a GPU kernel against the GPU device pointer to confirm the bytes are
 * visible on-device (no host bounce: the kernel reads exactly the pages SPDK
 * DMA'd into). Returns 0 on full byte-exact pass across the ladder.
 */
static int
strategy_a(struct nvfu_dev *d, uint32_t maxsize)
{
	printf("\n=== STRATEGY A: hipHostRegister(SPDK-DMA buffer) -> GPU device ptr ===\n");
	uint32_t *bad_dev = NULL;
	CHECK_HIP(hipMalloc((void **)&bad_dev, sizeof(uint32_t)));
	int overall = 0;

	for (size_t s = 0; s < sizeof(g_size_ladder) / sizeof(g_size_ladder[0]); s++) {
		uint32_t size = g_size_ladder[s];
		if (size > maxsize) {
			break;
		}
		char key[32];
		snprintf(key, sizeof(key), "A_%u", size);

		uint64_t iova = 0;
		uint8_t *hbuf = (uint8_t *)nvfu_dma_alloc(size, &iova);   /* SPDK-DMA, DPDK memseg */
		if (!hbuf) {
			fprintf(stderr, "  [%u] nvfu_dma_alloc failed\n", size);
			overall = -1;
			break;
		}

		/* CPU produces the pattern + Stores it. */
		for (uint32_t i = 0; i < size; i++) {
			hbuf[i] = pat(i);
		}
		spdk_wmb();
		struct spdk_nvme_cpl cpl;
		memset(&cpl, 0, sizeof(cpl));
		int rc = nvfu_kv_xfer_sgl(d, KV_NSID, SPDK_NVME_OPC_KV_STORE, key, size, iova, 0, 0, NULL);
		if (rc != 0) {
			fprintf(stderr, "  [%u] Store failed rc=%d\n", size, rc);
			spdk_dma_free(hbuf);
			overall = -1;
			continue;
		}

		/* Zero the buffer, then Retrieve INTO it, then verify ON THE GPU. */
		memset(hbuf, 0, size);
		spdk_wmb();
		memset(&cpl, 0, sizeof(cpl));
		rc = nvfu_kv_xfer_sgl(d, KV_NSID, SPDK_NVME_OPC_KV_RETRIEVE, key, size, iova, 0, 0, &cpl);
		if (rc != 0) {
			fprintf(stderr, "  [%u] Retrieve failed rc=%d\n", size, rc);
			spdk_dma_free(hbuf);
			overall = -1;
			continue;
		}
		uint32_t got = cpl.cdw0;

		/* Make the host buffer GPU-visible (same physical pages on UMA). */
		hipError_t e = hipHostRegister(hbuf, size, hipHostRegisterMapped);
		if (e != hipSuccess) {
			fprintf(stderr, "  [%u] hipHostRegister: %s\n", size, hipGetErrorString(e));
			spdk_dma_free(hbuf);
			overall = -1;
			continue;
		}
		uint8_t *dbuf = NULL;
		(void)hipHostGetDevicePointer((void **)&dbuf, hbuf, 0);

		uint32_t zero = 0;
		(void)hipMemcpy(bad_dev, &zero, sizeof(zero), hipMemcpyHostToDevice);
		uint32_t threads = 256, blocks = (size + threads - 1) / threads;
		if (blocks > 65535) blocks = 65535;
		hipLaunchKernelGGL(verify_pattern, dim3(blocks), dim3(threads), 0, 0, dbuf, size, bad_dev);
		(void)hipDeviceSynchronize();
		uint32_t bad = 0;
		(void)hipMemcpy(&bad, bad_dev, sizeof(bad), hipMemcpyDeviceToHost);

		hipHostUnregister(hbuf);
		spdk_dma_free(hbuf);

		printf("  [%8u] Retrieve got=%u  GPU-verify -> %s (%u mismatches)\n",
		       size, got, (got == size && bad == 0) ? "BYTE-EXACT" : "MISMATCH", bad);
		if (!(got == size && bad == 0)) {
			overall = -1;
		}
	}
	hipFree(bad_dev);
	return overall;
}

/*
 * STRATEGY B: hipMalloc device buffer, export dma-buf, probe SPDK DMA-ability.
 *
 * This tests spdk-avu open-Q1 directly. We do NOT yet have an SPDK API to
 * DMA_MAP an externally-supplied dma-buf fd as a region (the host client derives
 * the region fd from DPDK memsegs only -- see vfio_user_pci.c vfio_mr_map_notify
 * + memory.c spdk_mem_get_fd_and_offset, which call rte_mem_virt2memseg). So
 * this strategy first characterizes whether the simple path is even possible:
 *   (B1) does hsa_amd_portable_export_dmabuf succeed on a hipMalloc ptr?
 *   (B2) is that dma-buf CPU-mmap'able (mmap the fd)? If yes, the bytes a CPU
 *        writes through the mmap must be visible at the device pointer -- which
 *        would let SPDK DMA through the mmap vaddr (simple vaddr path).
 *   (B3) does spdk_mem_get_fd_and_offset see a hipMalloc ptr (would the existing
 *        register path accept it)?  Expected: NO (not a DPDK memseg).
 */
static int
strategy_b(struct nvfu_dev *d, uint32_t maxsize)
{
	(void)d;
	printf("\n=== STRATEGY B: hipMalloc + dma-buf export, SPDK DMA-ability probe ===\n");
	uint32_t size = (maxsize < 1048576u) ? maxsize : 1048576u;

	uint8_t *dbuf = NULL;
	CHECK_HIP(hipMalloc((void **)&dbuf, size));
	printf("  hipMalloc(%u) -> device ptr %p\n", size, (void *)dbuf);

	/* Fill on-device so we have a known pattern to read back through any mapping. */
	uint32_t threads = 256, blocks = (size + threads - 1) / threads;
	if (blocks > 65535) blocks = 65535;
	hipLaunchKernelGGL(fill_pattern, dim3(blocks), dim3(threads), 0, 0, dbuf, size);
	(void)hipDeviceSynchronize();

	/* B3: does the SPDK/DPDK memseg map see a hipMalloc ptr? */
	uint64_t off = 0;
	int dpdk_fd = spdk_mem_get_fd_and_offset(dbuf, &off);
	printf("  B3 spdk_mem_get_fd_and_offset(hipMalloc ptr) -> %d (%s)\n",
	       dpdk_fd, dpdk_fd >= 0 ? "DPDK-managed (unexpected)" : "NOT a DPDK memseg (expected)");

	/* B1: export the device buffer as a dma-buf. */
	int dmabuf_fd = -1;
	uint64_t dmabuf_off = 0;
	hsa_status_t hs = hsa_amd_portable_export_dmabuf(dbuf, size, &dmabuf_fd, &dmabuf_off);
	if (hs != HSA_STATUS_SUCCESS) {
		const char *msg = NULL;
		hsa_status_string(hs, &msg);
		printf("  B1 hsa_amd_portable_export_dmabuf FAILED: %s (status=%d)\n",
		       msg ? msg : "?", (int)hs);
		hipFree(dbuf);
		return -1;
	}
	printf("  B1 hsa_amd_portable_export_dmabuf -> fd=%d offset=%lu  OK\n", dmabuf_fd, dmabuf_off);

	/* B2: is the dma-buf CPU-mmap'able? (the simple vaddr path for SPDK). */
	void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, (off_t)dmabuf_off);
	if (map == MAP_FAILED) {
		printf("  B2 mmap(dma-buf) FAILED: %s  => dma-buf is NOT CPU-mappable; "
		       "SPDK would need an fd-only DMA region (not supported by the host client today)\n",
		       strerror(errno));
		close(dmabuf_fd);
		hipFree(dbuf);
		return 1;   /* characterized: hard path required */
	}
	printf("  B2 mmap(dma-buf) -> %p  OK (dma-buf IS CPU-mappable)\n", map);

	/* Coherence check: the on-device fill pattern must be visible through the
	 * CPU mmap. If so, the mmap vaddr is a CPU+GPU-coherent window -- exactly
	 * what SPDK needs to DMA into a GPU buffer via the simple vaddr path. */
	uint32_t bad = 0;
	uint8_t *m = (uint8_t *)map;
	for (uint32_t i = 0; i < size && bad < 8; i++) {
		if (m[i] != pat(i)) {
			if (bad < 4) {
				printf("    coherence mismatch @%u: mmap=0x%02x want=0x%02x\n", i, m[i], pat(i));
			}
			bad++;
		}
	}
	printf("  B2 coherence (GPU-written pattern read via CPU mmap): %s\n",
	       bad == 0 ? "COHERENT (GPU writes visible through mmap)" : "INCOHERENT");

	/* Now write a different pattern through the mmap and read it back on the GPU,
	 * to confirm the window is bidirectionally coherent (CPU write -> GPU read).
	 * This is the direction SPDK Retrieve needs (host DMA write -> GPU read). */
	for (uint32_t i = 0; i < size; i++) {
		m[i] = (uint8_t)((i * 17u + 3u) & 0xffu);
	}
	/* read back on device */
	uint8_t *host_check = (uint8_t *)malloc(size);
	(void)hipMemcpy(host_check, dbuf, size, hipMemcpyDeviceToHost);
	uint32_t bad2 = 0;
	for (uint32_t i = 0; i < size && bad2 < 8; i++) {
		if (host_check[i] != (uint8_t)((i * 17u + 3u) & 0xffu)) {
			bad2++;
		}
	}
	printf("  B2 reverse coherence (CPU mmap write -> GPU device read): %s\n",
	       bad2 == 0 ? "COHERENT" : "INCOHERENT");
	free(host_check);

	/* B4: can SPDK register the dma-buf MMAP vaddr as a DMA region? The host
	 * client registers regions implicitly when the buffer is a DPDK memseg; an
	 * mmap'd dma-buf is NOT a memseg, so spdk_mem_register on it would fail the
	 * same fd lookup. Report the gap explicitly. */
	uint64_t off2 = 0;
	int map_fd = spdk_mem_get_fd_and_offset(map, &off2);
	printf("  B4 spdk_mem_get_fd_and_offset(dma-buf mmap vaddr) -> %d (%s)\n",
	       map_fd, map_fd >= 0 ? "has backing fd" : "no DPDK fd (needs a direct DMA_MAP path)");

	munmap(map, size);
	close(dmabuf_fd);
	hipFree(dbuf);
	return (bad == 0 && bad2 == 0) ? 0 : 2;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <traddr-dir-containing-cntrl> [maxsize_bytes]\n", argv[0]);
		return 1;
	}
	uint32_t maxsize = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 0) : (16u * 1024u * 1024u);

	/* HIP/GPU sanity. */
	int ndev = 0;
	if (hipGetDeviceCount(&ndev) != hipSuccess || ndev < 1) {
		fprintf(stderr, "no HIP device\n");
		return 1;
	}
	hipDeviceProp_t prop;
	(void)hipGetDeviceProperties(&prop, 0);
	printf("GPU: %s (gcnArch=%s)\n", prop.name, prop.gcnArchName);

	/* SPDK env: hugepages + IOVA=VA (matches the proven host client). */
	struct spdk_env_opts opts;
	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "gpu_direct_probe";
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

	int ra = strategy_a(&d, maxsize);
	int rb = strategy_b(&d, maxsize);

	nvfu_close(&d);

	printf("\n==== RESULT ====\n");
	printf("Strategy A (host-buffer-as-GPU, hipHostRegister): %s\n",
	       ra == 0 ? "PASS (byte-exact GPU-direct Retrieve)" : "FAIL");
	printf("Strategy B (device-buffer dma-buf characterization): %s\n",
	       rb == 0 ? "dma-buf CPU-mappable + coherent both directions" :
	       rb == 1 ? "dma-buf NOT CPU-mappable (fd-only path required)" :
	       rb == 2 ? "dma-buf CPU-mappable but not fully coherent" : "FAIL/error");
	return (ra == 0) ? 0 : 2;
}
