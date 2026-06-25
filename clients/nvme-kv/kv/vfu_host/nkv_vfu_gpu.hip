/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Phase 2 / Milestone 2d (epic spdk-jhk, bead spdk-jhk.3): Option A --
 * GPU-initiated host NVMe-KV. The GPU builds the NVMe SQE and rings the
 * doorbell directly; SPDK is purely the target; no QEMU, no lib/nvme.
 *
 * Shares the entire driver (attach, admin queue, controller enable, IO queue
 * creation, completion polling) with the CPU variant via nkv_vfu.h. The ONLY
 * difference is nvfu_produce(): here a HIP kernel writes the 64-byte SQE into
 * the IO SQ ring (in host memory mapped to the GPU) and writes the SQ doorbell
 * (the vfio-user BAR0 page mapped to the GPU). This is exactly the rocm-xio
 * nvme-ep direct nvmeBar0Gpu path, minus the bridge.
 *
 * The CPU still performs one-time controller bring-up (like the guest kernel
 * did in the QEMU testbed); the GPU rings an already-initialized IO queue.
 *
 * Usage: nkv_vfu_gpu <traddr-dir-containing-cntrl>
 *
 * The target NVMe namespace defaults to KV_NSID (1); set the NKVX_NSID env var
 * to address a different namespace (e.g. NKVX_NSID=2). The positional CLI is
 * unchanged; nsid is threaded through every datapath op below (bead spdk-jhk.10).
 */

#include <hip/hip_runtime.h>

extern "C" {
#include "nkv_vfu.h"
#include "spdk/log.h"
}

/* GPU-visible aliases of the IO SQ and the BAR0 doorbell page. */
static struct spdk_nvme_cmd	*g_sq_dev;
static volatile uint32_t	*g_db_dev;
static int			g_use_gpu;

/*
 * One GPU thread builds the submission: copy the 64-byte SQE into the SQ slot,
 * fence to system scope so the controller sees it, then ring the doorbell and
 * fence again. __threadfence_system() is the GPU analogue of the producer
 * coherence the QEMU path could never guarantee across the memfd boundary.
 */
__global__ void
nvfu_ring_kernel(struct spdk_nvme_cmd *sq, uint16_t slot, struct spdk_nvme_cmd sqe,
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

extern "C" int
nvfu_produce(struct nvfu_dev *d, struct nvfu_queue *q,
	     const struct spdk_nvme_cmd *cmd, uint16_t slot, uint16_t new_tail)
{
	hipError_t e;

	/* Admin / bring-up commands always go via the CPU. Only the IO KV op,
	 * once g_use_gpu is set, is GPU-initiated. */
	if (!g_use_gpu || q->qid != IO_QID) {
		q->sq[slot] = *cmd;
		spdk_wmb();
		d->doorbells[q->sq_db] = new_tail;
		return 0;
	}

	nvfu_ring_kernel<<<1, 1>>>(g_sq_dev, slot, *cmd, g_db_dev, q->sq_db, new_tail);
	e = hipDeviceSynchronize();
	if (e != hipSuccess) {
		fprintf(stderr, "GPU ring kernel failed: %s\n", hipGetErrorString(e));
		return -EIO;
	}
	return 0;
}

/* Map the IO SQ and the doorbell page into the GPU's address space. */
static int
gpu_map(struct nvfu_dev *d)
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
	printf("GPU mapped: IO SQ + BAR0 doorbell page (sq_dev=%p db_dev=%p)\n",
	       (void *)g_sq_dev, (void *)g_db_dev);
	return 0;
}

/*
 * Wavefront batch: each lane stages one 64-byte SQE into its SQ slot; after a
 * block-wide barrier + system fence (all SQEs globally visible), lane 0 rings
 * the SQ doorbell ONCE for the whole batch. With n<=32 this is exactly one
 * gfx1151 wavefront (warpSize=32); n up to the IO queue depth uses ceil(n/32)
 * wavefronts in one block, still one doorbell ring (rocm-xio batchSize > 1).
 */
__global__ void
ring_wavefront(struct spdk_nvme_cmd *sq, uint16_t base, uint16_t depth,
	       const struct spdk_nvme_cmd *cmds, uint32_t n,
	       volatile uint32_t *db, uint32_t db_idx, uint16_t new_tail)
{
	uint32_t i = threadIdx.x;

	if (i < n) {
		sq[(base + i) % depth] = cmds[i];
	}
	__threadfence_system();		/* this lane's SQE visible system-wide */
	__syncthreads();		/* all lanes have staged + fenced */
	if (i == 0) {
		db[db_idx] = new_tail;	/* one doorbell for the whole batch */
		__threadfence_system();
	}
}

/*
 * Ring n pre-built SQEs in one wavefront + one doorbell, then reap n completions.
 * Returns the number that completed SUCCESS, or -1 on a GPU/timeout error.
 * Callers fill cmds[i] (incl. cid) and any DMA buffers before calling.
 */
static int
gpu_batch_ring_reap(struct nvfu_dev *d, struct spdk_nvme_cmd *cmds, uint32_t n)
{
	struct nvfu_queue *q = &d->io;
	struct spdk_nvme_cmd *cmds_dev = NULL;
	uint16_t base = q->sq_tail;
	uint16_t new_tail = (uint16_t)((base + n) % q->depth);
	uint32_t reaped, ok = 0;
	hipError_t e;

	e = hipHostRegister(cmds, n * sizeof(*cmds), hipHostRegisterMapped);
	if (e != hipSuccess) {
		fprintf(stderr, "hipHostRegister(cmds): %s\n", hipGetErrorString(e));
		return -1;
	}
	(void)hipHostGetDevicePointer((void **)&cmds_dev, cmds, 0);

	ring_wavefront<<<1, n>>>(g_sq_dev, base, q->depth, cmds_dev, n,
				 g_db_dev, q->sq_db, new_tail);
	e = hipDeviceSynchronize();
	hipHostUnregister(cmds);
	if (e != hipSuccess) {
		fprintf(stderr, "wavefront kernel failed: %s\n", hipGetErrorString(e));
		return -1;
	}
	q->sq_tail = new_tail;

	for (reaped = 0; reaped < n; reaped++) {
		volatile struct spdk_nvme_cpl *cqe = &q->cq[q->cq_head];
		uint64_t deadline = spdk_get_ticks() + 5 * spdk_get_ticks_hz();

		while (cqe->status.p != q->cq_phase) {
			if (spdk_get_ticks() > deadline) {
				fprintf(stderr, "batch reap timeout %u/%u\n", reaped, n);
				return (int)ok;
			}
			spdk_pause();
		}
		spdk_rmb();
		if (cqe->status.sc == 0 && cqe->status.sct == 0) {
			ok++;
		}
		q->cq_head = (q->cq_head + 1) % q->depth;
		if (q->cq_head == 0) {
			q->cq_phase ^= 1;
		}
		d->doorbells[q->cq_db] = q->cq_head;
	}
	return (int)ok;
}

static const char *
wf_desc(uint32_t n)	/* "1 wavefront" / "3 wavefronts" */
{
	static char b[24];
	uint32_t w = (n + 31) / 32;

	snprintf(b, sizeof(b), "%u wavefront%s", w, (w > 1) ? "s" : "");
	return b;
}

/* n GPU-initiated KV Stores in one wavefront + one doorbell. */
static int
gpu_store_batch(struct nvfu_dev *d, uint32_t nsid, const char *prefix, uint32_t n)
{
	struct nvfu_queue *q = &d->io;
	const uint32_t SLOT = 4096;	/* one page per value -> single PRP */
	struct spdk_nvme_cmd *cmds = NULL;
	char *vbuf = NULL;
	uint64_t vbuf_iova = 0;
	uint32_t i;
	int ok, rc = -EIO;

	/* An NVMe SQ holds depth-1 entries (tail==head means empty), and the kernel
	 * launches n threads (<=1024 per block), so cap at min(depth-1, 1024). */
	if (n == 0 || n >= q->depth || n > 1024) {
		fprintf(stderr, "batch n must be 1..%u\n",
			(q->depth - 1u < 1024u) ? q->depth - 1u : 1024u);
		return -EINVAL;
	}
	cmds = (struct spdk_nvme_cmd *)nvfu_dma_alloc(n * sizeof(*cmds), NULL);
	vbuf = (char *)nvfu_dma_alloc((size_t)n * SLOT, &vbuf_iova);
	if (!cmds || !vbuf) {
		fprintf(stderr, "batch alloc failed\n");
		goto done;
	}
	for (i = 0; i < n; i++) {
		char key[32], val[64];
		int klen = snprintf(key, sizeof(key), "%s%u", prefix, i);
		int vlen = snprintf(val, sizeof(val), "wavefront-value-%u", i);
		struct spdk_nvme_cmd *c = &cmds[i];

		memcpy(vbuf + (size_t)i * SLOT, val, vlen);
		memset(c, 0, sizeof(*c));
		c->opc = SPDK_NVME_OPC_KV_STORE;
		c->nsid = nsid;
		c->dptr.prp.prp1 = vbuf_iova + (uint64_t)i * SLOT;
		c->cdw10_bits.kv.vsize = vlen;
		nvfu_kv_set_key(c, key, (uint8_t)klen);
		c->cid = q->cid++;
	}
	spdk_wmb();

	ok = gpu_batch_ring_reap(d, cmds, n);
	if (ok < 0) {
		goto done;
	}
	printf("WAVEFRONT STORE-BATCH ok: %d/%u GPU-initiated stores in ONE doorbell "
	       "(%u-lane = %s), keys %s0..%s%u\n",
	       ok, n, n, wf_desc(n), prefix, prefix, n - 1);
	rc = ((uint32_t)ok == n) ? 0 : -EIO;
done:
	spdk_dma_free(cmds);
	spdk_dma_free(vbuf);
	return rc;
}

/*
 * n GPU-initiated KV Execs (op_id) against keys <prefix>0..<prefix>(n-1) in one
 * wavefront + one doorbell. Each Exec routes through rados-nkv to the two-tier
 * rados-nkvx executor; results land back in per-Exec buffers (in-place).
 */
static int
gpu_exec_batch(struct nvfu_dev *d, uint32_t nsid, const char *prefix, uint32_t op_id, uint32_t n)
{
	struct nvfu_queue *q = &d->io;
	const uint32_t SLOT = 4096;
	struct spdk_nvme_cmd *cmds = NULL;
	char *buf = NULL;
	uint64_t buf_iova = 0;
	uint32_t i;
	int ok, rc = -EIO;

	/* An NVMe SQ holds depth-1 entries (tail==head means empty), and the kernel
	 * launches n threads (<=1024 per block), so cap at min(depth-1, 1024). */
	if (n == 0 || n >= q->depth || n > 1024) {
		fprintf(stderr, "batch n must be 1..%u\n",
			(q->depth - 1u < 1024u) ? q->depth - 1u : 1024u);
		return -EINVAL;
	}
	cmds = (struct spdk_nvme_cmd *)nvfu_dma_alloc(n * sizeof(*cmds), NULL);
	buf = (char *)nvfu_dma_alloc((size_t)n * SLOT, &buf_iova);
	if (!cmds || !buf) {
		fprintf(stderr, "batch alloc failed\n");
		goto done;
	}
	for (i = 0; i < n; i++) {
		char key[32];
		uint8_t klen = (uint8_t)snprintf(key, sizeof(key), "%s%u", prefix, i);
		uint16_t klp = klen;
		char *slot = buf + (size_t)i * SLOT;
		struct spdk_nvme_cmd *c = &cmds[i];
		uint32_t payload_len = (uint32_t)sizeof(uint16_t) + klen;	/* input_len 0 */

		/* ADR-0014 request head: [u16 key_len][key] */
		memcpy(slot, &klp, sizeof(klp));
		memcpy(slot + sizeof(klp), key, klen);
		memset(c, 0, sizeof(*c));
		c->opc = SPDK_NVME_OPC_KV_EXEC;
		c->nsid = nsid;
		c->dptr.prp.prp1 = buf_iova + (uint64_t)i * SLOT;
		c->cdw10_bits.kv.vsize = payload_len;
		/* KV Exec osize/op_id are vendor extensions absent from the ratified
		 * base SPDK spec (no cdw12_bits.kv_exec / cdw13_bits.kv_exec). The named
		 * fields were full-width :32, so write the raw CDW words instead — same
		 * convention and byte-identical wire output as nvfu_kv_exec() in
		 * nkv_vfu.h. (osize == KV Exec output-buffer size; op_id == executor op.) */
		c->cdw12 = SLOT;
		c->cdw13 = op_id;
		c->cid = q->cid++;
	}
	spdk_wmb();

	ok = gpu_batch_ring_reap(d, cmds, n);
	if (ok < 0) {
		goto done;
	}
	spdk_rmb();
	printf("WAVEFRONT EXEC-BATCH ok: %d/%u GPU-initiated Execs (op %u) in ONE doorbell "
	       "(%u-lane = %s) -> two-tier executor, keys %s0..%s%u\n",
	       ok, n, op_id, n, wf_desc(n), prefix, prefix, n - 1);
	if (op_id == 10) {	/* nkvx:bytecount -> LE u64 per Exec, in its own buffer */
		uint64_t c0 = 0, cl = 0;

		memcpy(&c0, buf, 8);
		memcpy(&cl, buf + (size_t)(n - 1) * SLOT, 8);
		printf("  sample bytecount: %s0=%llu  %s%u=%llu\n",
		       prefix, (unsigned long long)c0, prefix, n - 1, (unsigned long long)cl);
	}
	rc = ((uint32_t)ok == n) ? 0 : -EIO;
done:
	spdk_dma_free(cmds);
	spdk_dma_free(buf);
	return rc;
}

/*
 * Wavefront batch RETRIEVE for an EXPLICIT key list (bead spdk-jhk.11). Unlike
 * gpu_store_batch/gpu_exec_batch (which synthesise keys from a prefix+count),
 * this takes the caller's literal keys and returns their values, length-framed
 * in INPUT ORDER on stdout, so a parent process (rados-nkv) can demux. Each
 * chunk of up to (depth-1) keys is one wavefront + ONE SQ doorbell ring (via
 * gpu_batch_ring_reap); chunks are only used when N exceeds the IO queue depth,
 * and the chunk count is logged to stderr (never a silent cap).
 *
 * Output framing (stdout, binary): for each key in order, a 4-byte LE length
 * followed by that many value bytes. A not-found / failed key is framed with
 * the sentinel length 0xFFFFFFFF and no value bytes, so the caller can tell a
 * missing key from a zero-length value.
 *
 * spdk-jhk.12: each lane carries a region-bounded SGL DPTR (not a 4 KiB PRP), so
 * a value up to the controller max (64 MiB) is returned in FULL, framed at its
 * TRUE length. The larger per-lane buffers shrink the chunk size (memory-bounded,
 * see NKVX_RETR_LANE_BUF / NKVX_RETR_BATCH_BUDGET) but each chunk is still one
 * wavefront + ONE doorbell.
 */
#define NKVX_NOTFOUND_LEN	0xFFFFFFFFu

/*
 * retrieve-batch emits a binary length-framed stream that MUST be the only thing
 * on stdout, but the shared driver (nvfu_open/gpu_map/identify) prints human
 * bring-up chatter via printf(stdout). So before bring-up we dup the real stdout
 * to g_frame_out and point fd 1 at stderr; all chatter then lands on stderr and
 * the frames go to g_frame_out (the original stdout the caller reads).
 */
static FILE *g_frame_out;	/* NULL until retrieve-batch redirects; then real stdout */

static int
nkvx_redirect_stdout_to_stderr(void)
{
	int saved = dup(STDOUT_FILENO);

	if (saved < 0) {
		fprintf(stderr, "retrieve-batch: dup(stdout) failed\n");
		return -EIO;
	}
	fflush(stdout);
	g_frame_out = fdopen(saved, "wb");
	if (g_frame_out == NULL) {
		fprintf(stderr, "retrieve-batch: fdopen(saved stdout) failed\n");
		close(saved);
		return -EIO;
	}
	if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
		fprintf(stderr, "retrieve-batch: dup2(stderr->stdout) failed\n");
		fclose(g_frame_out);
		g_frame_out = NULL;
		return -EIO;
	}
	return 0;
}

static int
emit_frame(uint32_t len, const void *val)
{
	FILE *out = g_frame_out ? g_frame_out : stdout;
	unsigned char hdr[4];

	hdr[0] = (unsigned char)(len & 0xff);
	hdr[1] = (unsigned char)((len >> 8) & 0xff);
	hdr[2] = (unsigned char)((len >> 16) & 0xff);
	hdr[3] = (unsigned char)((len >> 24) & 0xff);
	if (fwrite(hdr, 1, sizeof(hdr), out) != sizeof(hdr)) {
		return -EIO;
	}
	if (len != NKVX_NOTFOUND_LEN && len > 0) {
		if (fwrite(val, 1, len, out) != len) {
			return -EIO;
		}
	}
	return 0;
}

/*
 * Per-lane retrieve buffer (bead spdk-jhk.12). spdk-jhk.11 gave each lane a fixed
 * 4096-byte PRP slot, so any value >4 KiB was truncated to 4 KiB. We now size each
 * lane to the controller max_io_size (64 MiB) and point the lane's DPTR at a
 * region-bounded SGL (nvfu_sgl_set_dptr) instead of a single PRP, so a lane
 * returns its FULL value. The length framed to the caller is the controller's
 * reported value length (cqe->cdw0, clamped only to the lane buffer), not the
 * slot size. */
#define NKVX_RETR_LANE_BUF	(64ULL * 1024 * 1024)	/* controller max_io_size */
/*
 * Per-batch DMA budget. Larger lane buffers mean fewer lanes fit in one
 * wavefront/doorbell, so we cap a chunk so the total per-batch DMA stays bounded:
 * chunk_max = min(depth-1, BUDGET / LANE_BUF). At 64 MiB/lane and a 256 MiB
 * budget that is 4 lanes/chunk; a batch of N keys is split into ceil(N/4) chunks,
 * each still a single wavefront + ONE doorbell. This is logged (never a silent
 * cap). Raising the budget trades host DMA memory for fewer doorbells. */
#define NKVX_RETR_BATCH_BUDGET	(256ULL * 1024 * 1024)

/*
 * Retrieve one chunk (<= chunk_max keys) in a single wavefront + doorbell, then
 * frame each value to stdout in order. keys[]/klens[] index the chunk slice. Each
 * lane gets a region-bounded SGL DPTR over its own NKVX_RETR_LANE_BUF slice, so a
 * value up to the controller max comes back in full. Returns 0 on success (all
 * CQEs reaped), -1 on a GPU/timeout/alloc error.
 */
static int
gpu_retrieve_chunk(struct nvfu_dev *d, uint32_t nsid,
		   char keys[][256], const uint8_t *klens, uint32_t n,
		   uint32_t chunk_idx, uint32_t nchunks)
{
	struct nvfu_queue *q = &d->io;
	const uint64_t SLOT = NKVX_RETR_LANE_BUF;	/* region-bounded SGL per lane */
	struct spdk_nvme_cmd *cmds = NULL;
	struct spdk_nvme_sgl_descriptor **segs = NULL;	/* per-lane SGL seg list (or NULL) */
	char *buf = NULL;
	uint64_t buf_iova = 0;
	uint16_t base = q->sq_tail;
	uint32_t reaped, i;
	int rc = -1;

	cmds = (struct spdk_nvme_cmd *)nvfu_dma_alloc(n * sizeof(*cmds), NULL);
	segs = (struct spdk_nvme_sgl_descriptor **)calloc(n, sizeof(*segs));
	buf = (char *)nvfu_dma_alloc((size_t)n * SLOT, &buf_iova);
	if (!cmds || !segs || !buf) {
		fprintf(stderr, "retrieve-batch chunk alloc failed (%u lanes x %llu bytes)\n",
			n, (unsigned long long)SLOT);
		goto done;
	}
	for (i = 0; i < n; i++) {
		struct spdk_nvme_cmd *c = &cmds[i];
		char *lane = buf + (size_t)i * SLOT;
		uint64_t lane_iova = buf_iova + (uint64_t)i * SLOT;
		uint16_t klp = klens[i];
		uint32_t head_len = (uint32_t)sizeof(uint16_t) + klens[i];
		uint32_t out_len = (uint32_t)SLOT - head_len;	/* value buffer after the head */
		int err = 0;

		/*
		 * KV Retrieve carries the key as the in-payload [u16 key_len][key] head
		 * (host->device), NOT the inline CDW key slots — the forwarder reads the
		 * key from the DPTR prefix (docs/wire-format.md; same encoding the CPU
		 * nvfu_kv_retrieve_lk and the GPU store use). Staging the key only in
		 * CDW2/3/14/15 (nvfu_kv_set_key) left the in-payload head zeroed, so the
		 * forwarder read key_len=0 and returned KEY_NOT_EXIST before the executor
		 * ever ran the lookup. Stage the head here and signal long-key via
		 * cdw11.kl=0; the value is returned at offset head_len (after the head).
		 */
		memcpy(lane, &klp, sizeof(klp));
		memcpy(lane + sizeof(klp), keys[i], klens[i]);

		memset(c, 0, sizeof(*c));
		c->opc = SPDK_NVME_OPC_KV_RETRIEVE;
		c->nsid = nsid;
		c->cdw11_bits.kv.kl = 0;		/* long-key: real length is the u16 prefix */
		c->cdw10_bits.kv.vsize = out_len;	/* host buffer size for the value */
		c->cid = q->cid++;
		/* Region-bounded SGL DPTR over the whole lane ([head][value]); a single
		 * data block cannot cross a 2 MiB DMA region, so a >2 MiB value scatters
		 * correctly into the lane. */
		segs[i] = nvfu_sgl_set_dptr(c, lane_iova, (uint32_t)SLOT, &err);
		if (err != 0) {
			fprintf(stderr, "retrieve-batch SGL build failed (lane %u): %d\n", i, err);
			goto done;
		}
	}
	spdk_wmb();

	/* One wavefront + ONE doorbell for the whole chunk. gpu_batch_ring_reap
	 * advances cq_head and counts SUCCESS CQEs, but for retrieve we need each
	 * CQE's cdw0 (value length) and not-found status, so we reap inline here
	 * after the single GPU ring. */
	{
		struct spdk_nvme_cmd *cmds_dev = NULL;
		uint16_t new_tail = (uint16_t)((base + n) % q->depth);
		hipError_t e;

		e = hipHostRegister(cmds, n * sizeof(*cmds), hipHostRegisterMapped);
		if (e != hipSuccess) {
			fprintf(stderr, "hipHostRegister(cmds): %s\n", hipGetErrorString(e));
			goto done;
		}
		(void)hipHostGetDevicePointer((void **)&cmds_dev, cmds, 0);
		ring_wavefront<<<1, n>>>(g_sq_dev, base, q->depth, cmds_dev, n,
					 g_db_dev, q->sq_db, new_tail);
		e = hipDeviceSynchronize();
		hipHostUnregister(cmds);
		if (e != hipSuccess) {
			fprintf(stderr, "wavefront kernel failed: %s\n", hipGetErrorString(e));
			goto done;
		}
		q->sq_tail = new_tail;
		fprintf(stderr,
			"WAVEFRONT RETRIEVE-BATCH chunk %u/%u: %u-lane = %s, ONE doorbell ring "
			"(%llu MiB/lane SGL)\n",
			chunk_idx + 1, nchunks, n, wf_desc(n),
			(unsigned long long)(SLOT >> 20));
	}

	for (reaped = 0; reaped < n; reaped++) {
		volatile struct spdk_nvme_cpl *cqe = &q->cq[q->cq_head];
		uint64_t deadline = spdk_get_ticks() + 5 * spdk_get_ticks_hz();
		/* The value is returned at offset head_len within the lane (after the
		 * [u16 key_len][key] head), per the ratified wire format — NOT at offset
		 * 0. CQEs complete in submission == lane order on this single in-order IO
		 * queue, so lane index == reaped. */
		uint32_t head_len = (uint32_t)sizeof(uint16_t) + klens[reaped];
		uint32_t out_len = (uint32_t)SLOT - head_len;
		uint32_t vlen;

		while (cqe->status.p != q->cq_phase) {
			if (spdk_get_ticks() > deadline) {
				fprintf(stderr, "retrieve-batch reap timeout %u/%u\n", reaped, n);
				goto done;
			}
			spdk_pause();
		}
		spdk_rmb();
		if (cqe->status.sc == 0 && cqe->status.sct == 0) {
			/* TRUE value length (clamped to the lane's value region), so a
			 * >4 KiB value is framed at its actual size, not the slot. */
			vlen = spdk_min(cqe->cdw0, out_len);
			if (emit_frame(vlen, buf + (size_t)reaped * SLOT + head_len) != 0) {
				goto done;
			}
		} else {
			/* Not found / error: emit the sentinel so the caller demuxes. */
			if (emit_frame(NKVX_NOTFOUND_LEN, NULL) != 0) {
				goto done;
			}
		}
		q->cq_head = (q->cq_head + 1) % q->depth;
		if (q->cq_head == 0) {
			q->cq_phase ^= 1;
		}
		d->doorbells[q->cq_db] = q->cq_head;
	}
	rc = 0;
done:
	if (segs != NULL) {
		for (i = 0; i < n; i++) {
			if (segs[i] != NULL) {
				spdk_dma_free(segs[i]);
			}
		}
		free(segs);
	}
	spdk_dma_free(cmds);
	spdk_dma_free(buf);
	return rc;
}

/*
 * Read keys one-per-line from `path` ("-" or NULL = stdin), then retrieve them
 * all (chunked at depth-1) and length-frame the values to stdout in order.
 */
static int
gpu_retrieve_batch(struct nvfu_dev *d, uint32_t nsid, const char *path)
{
	struct nvfu_queue *q = &d->io;
	/* The larger per-lane SGL buffers (NKVX_RETR_LANE_BUF) cap how many lanes
	 * fit in one chunk: the smaller of the IO queue depth-1 and the per-batch
	 * DMA budget / lane buffer. So chunking is driven by memory, not just depth;
	 * each chunk is still a single wavefront + ONE doorbell. */
	const uint32_t depth_cap = (q->depth > 1) ? (q->depth - 1u) : 1u;
	uint32_t budget_cap = (uint32_t)(NKVX_RETR_BATCH_BUDGET / NKVX_RETR_LANE_BUF);
	uint32_t chunk_max;
	FILE *f = NULL;
	char (*keys)[256] = NULL;
	uint8_t *klens = NULL;
	uint32_t cap = 64, n = 0, off, nchunks, ci;
	char line[512];
	int rc = -EIO;

	if (path == NULL || !strcmp(path, "-")) {
		f = stdin;
	} else {
		f = fopen(path, "r");
		if (f == NULL) {
			fprintf(stderr, "retrieve-batch: cannot open keyfile '%s'\n", path);
			return -ENOENT;
		}
	}

	keys = (char (*)[256])malloc((size_t)cap * sizeof(keys[0]));
	klens = (uint8_t *)malloc((size_t)cap);
	if (!keys || !klens) {
		fprintf(stderr, "retrieve-batch: key buffer alloc failed\n");
		goto done;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		size_t L = strlen(line);

		while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r')) {
			line[--L] = '\0';
		}
		if (L == 0) {
			continue;	/* skip blank lines */
		}
		if (L > 16) {
			fprintf(stderr, "retrieve-batch: key '%s' is %zu bytes; KV keys are 1-16 bytes\n",
				line, L);
			goto done;
		}
		if (n == cap) {
			uint32_t ncap = cap * 2;
			char (*nk)[256] = (char (*)[256])realloc(keys, (size_t)ncap * sizeof(keys[0]));
			uint8_t *nl = (uint8_t *)realloc(klens, (size_t)ncap);

			if (!nk || !nl) {
				free(nk ? nk : keys);
				free(nl ? nl : klens);
				keys = NULL;
				klens = NULL;
				fprintf(stderr, "retrieve-batch: key buffer grow failed\n");
				goto done;
			}
			keys = nk;
			klens = nl;
			cap = ncap;
		}
		memcpy(keys[n], line, L);
		keys[n][L] = '\0';
		klens[n] = (uint8_t)L;
		n++;
	}
	if (n == 0) {
		fprintf(stderr, "retrieve-batch: no keys on input\n");
		goto done;
	}

	if (budget_cap == 0) {
		budget_cap = 1;	/* always retrieve at least one key per chunk */
	}
	chunk_max = (depth_cap < budget_cap) ? depth_cap : budget_cap;
	nchunks = (n + chunk_max - 1) / chunk_max;
	fprintf(stderr,
		"RETRIEVE-BATCH: %u keys, IO queue depth=%u, %llu MiB/lane, %llu MiB batch "
		"budget -> chunk size %u -> %u chunk%s (ONE doorbell per chunk)\n",
		n, q->depth,
		(unsigned long long)(NKVX_RETR_LANE_BUF >> 20),
		(unsigned long long)(NKVX_RETR_BATCH_BUDGET >> 20),
		chunk_max, nchunks, (nchunks > 1) ? "s" : "");

	for (ci = 0, off = 0; off < n; ci++, off += chunk_max) {
		uint32_t this_n = (n - off < chunk_max) ? (n - off) : chunk_max;

		if (gpu_retrieve_chunk(d, nsid, keys + off, klens + off, this_n, ci, nchunks) != 0) {
			goto done;
		}
	}
	if (fflush(g_frame_out ? g_frame_out : stdout) != 0) {
		fprintf(stderr, "retrieve-batch: frame stream flush failed\n");
		goto done;
	}
	rc = 0;
done:
	free(keys);
	free(klens);
	if (f != NULL && f != stdin) {
		fclose(f);
	}
	return rc;
}

/* GPU produces the bulk value: each lane writes its bytes of a verifiable
 * pattern, then a system fence makes the whole buffer visible to the controller. */
__global__ void
fill_pattern(uint8_t *buf, uint32_t size)
{
	uint32_t stride = blockDim.x * gridDim.x;
	uint32_t i;

	for (i = blockIdx.x * blockDim.x + threadIdx.x; i < size; i += stride) {
		buf[i] = (uint8_t)((i * 131u + 7u) & 0xffu);
	}
	__threadfence_system();
}

/*
 * Size-ladder / spdk-5co coherence test: the GPU PRODUCES a large value (kernel
 * fills the pattern), then GPU-initiated Store; CPU retrieves and verifies
 * byte-exact. This is the faithful spdk-5co scenario (GPU bulk production + the
 * doorbell) on bare metal. Uses a single contiguous SGL descriptor (no PRP
 * list / 512-page boundary), so it scales to the controller max (64 MiB).
 */
static int
gpu_store_big(struct nvfu_dev *d, uint32_t nsid, const char *key, uint32_t size)
{
	uint8_t *vbuf = NULL, *rbuf = NULL, *vbuf_dev = NULL;
	uint64_t vbuf_iova = 0, rbuf_iova = 0;
	struct spdk_nvme_cpl cpl;
	uint32_t got = 0, i, bad = 0, threads = 256, blocks;
	hipError_t e;
	int rc = -EIO;

	/* Up to the controller's max_io_size (64 MiB). Transfers spanning more than
	 * one 2 MiB vfio-user DMA region use a region-bounded SGL segment list
	 * (nvfu_kv_xfer_sgl), since a single data-block descriptor cannot cross a
	 * region boundary (spdk-rcz / spdk-cvi). */
	if (size == 0 || size > 64u * 1024u * 1024u) {
		fprintf(stderr, "store-big size must be 1..%u bytes (controller max_io_size)\n",
			64u * 1024u * 1024u);
		return -EINVAL;
	}
	vbuf = (uint8_t *)nvfu_dma_alloc(size, &vbuf_iova);
	rbuf = (uint8_t *)nvfu_dma_alloc(size, &rbuf_iova);
	if (!vbuf || !rbuf) {
		fprintf(stderr, "store-big alloc failed\n");
		goto done;
	}

	/* GPU produces the bulk value into the host-registered buffer. */
	e = hipHostRegister(vbuf, size, hipHostRegisterMapped);
	if (e != hipSuccess) {
		fprintf(stderr, "hipHostRegister(vbuf): %s\n", hipGetErrorString(e));
		goto done;
	}
	(void)hipHostGetDevicePointer((void **)&vbuf_dev, vbuf, 0);
	blocks = (size + threads - 1) / threads;
	if (blocks > 65535) {
		blocks = 65535;
	}
	fill_pattern<<<blocks, threads>>>(vbuf_dev, size);
	e = hipDeviceSynchronize();
	if (e != hipSuccess) {
		fprintf(stderr, "fill kernel: %s\n", hipGetErrorString(e));
		hipHostUnregister(vbuf);
		goto done;
	}

	/* GPU-initiated Store of the GPU-produced bulk (single contiguous SGL). */
	g_use_gpu = 1;
	rc = nvfu_kv_xfer_sgl(d, nsid, SPDK_NVME_OPC_KV_STORE, key, size, vbuf_iova, 0, 0, NULL);
	hipHostUnregister(vbuf);
	if (rc != 0) {
		goto done;
	}

	/* CPU retrieve + byte-exact verify of the GPU-produced pattern. */
	g_use_gpu = 0;
	memset(&cpl, 0, sizeof(cpl));
	if (nvfu_kv_xfer_sgl(d, nsid, SPDK_NVME_OPC_KV_RETRIEVE, key, size, rbuf_iova, 0, 0, &cpl) != 0) {
		rc = -EIO;
		goto done;
	}
	got = cpl.cdw0;
	for (i = 0; i < size; i++) {
		if (rbuf[i] != (uint8_t)((i * 131u + 7u) & 0xffu)) {
			if (bad < 3) {
				fprintf(stderr, "  mismatch @%u: got 0x%02x want 0x%02x\n",
					i, rbuf[i], (uint8_t)((i * 131u + 7u) & 0xffu));
			}
			bad++;
		}
	}
	printf("STORE-BIG %s: GPU produced+stored %u bytes (%u pages, SGL), retrieved %u -> %s "
	       "(%u mismatches)\n", key, size, (size + 4095) / 4096, got,
	       (got == size && bad == 0) ? "BYTE-EXACT" : "MISMATCH", bad);
	rc = (got == size && bad == 0) ? 0 : -EIO;
done:
	spdk_dma_free(vbuf);
	spdk_dma_free(rbuf);
	return rc;
}

/*
 * Large-result exec verification (spdk-w2y): store a >2 MiB pattern value, then
 * KV Exec op11 (nkvx:identity) with a large output buffer, and verify the
 * scattered-back result is byte-exact. Exercises the two-tier executor's
 * large-result path and the region-bounded SGL DPTR scatter on the front.
 */
static int
gpu_exec_big(struct nvfu_dev *d, uint32_t nsid, const char *key, uint32_t op_id, uint32_t size)
{
	uint8_t *vbuf = NULL, *obuf = NULL;
	uint64_t vbuf_iova = 0;
	uint32_t got = 0, i, bad = 0;
	int rc = -EIO;

	if (size == 0 || size > 64u * 1024u * 1024u) {
		fprintf(stderr, "exec-big size must be 1..%u bytes\n", 64u * 1024u * 1024u);
		return -EINVAL;
	}
	vbuf = (uint8_t *)nvfu_dma_alloc(size, &vbuf_iova);
	obuf = (uint8_t *)malloc(size);
	if (!vbuf || !obuf) {
		fprintf(stderr, "exec-big alloc failed\n");
		goto done;
	}

	for (i = 0; i < size; i++) {
		vbuf[i] = (uint8_t)((i * 131u + 7u) & 0xffu);
	}
	spdk_wmb();

	/* Store the value via a region-bounded SGL. */
	if (nvfu_kv_xfer_sgl(d, nsid, SPDK_NVME_OPC_KV_STORE, key, size, vbuf_iova, 0, 0, NULL) != 0) {
		fprintf(stderr, "exec-big: store failed\n");
		goto done;
	}

	/* Exec identity: the executor returns the stored value, scattered back into
	 * the (multi-region) DPTR buffer. */
	if (nvfu_kv_exec(d, nsid, key, op_id, NULL, 0, obuf, size, &got) != 0) {
		goto done;
	}
	for (i = 0; i < got; i++) {
		if (obuf[i] != (uint8_t)((i * 131u + 7u) & 0xffu)) {
			if (bad < 3) {
				fprintf(stderr, "  mismatch @%u: got 0x%02x want 0x%02x\n",
					i, obuf[i], (uint8_t)((i * 131u + 7u) & 0xffu));
			}
			bad++;
		}
	}
	printf("EXEC-BIG %s: op %u over %u bytes -> result %u bytes -> %s (%u mismatches)\n",
	       key, op_id, size, got,
	       (got == size && bad == 0) ? "BYTE-EXACT" : "MISMATCH", bad);
	rc = (got == size && bad == 0) ? 0 : -EIO;
done:
	if (vbuf) {
		spdk_dma_free(vbuf);
	}
	free(obuf);
	return rc;
}

/*
 * GPU-initiated KV Store of a value read from a file (bead spdk-jhk.13). The
 * standalone `store` mode takes the value as an argv token (small UTF-8 only);
 * `store-file` instead reads the raw value bytes from `valpath` ("-" = stdin) so
 * the rados-nkv `--gpu store -i <bigfile>` path can carry arbitrary binary values
 * up to the controller max_io_size. The transfer rides a region-bounded SGL DPTR
 * (nvfu_kv_xfer_sgl), not a single 4 KiB PRP, so a value >4 KiB no longer fails
 * with KV Store status 0x6 (symmetric to the spdk-jhk.12 retrieve fix).
 */
static int
gpu_store_file(struct nvfu_dev *d, uint32_t nsid, const char *key, const char *valpath)
{
	const uint32_t MAX_VALUE = 64u * 1024u * 1024u;	/* controller max_io_size */
	uint8_t *vbuf = NULL;
	uint64_t vbuf_iova = 0;
	size_t cap = 4096, len = 0;
	FILE *f = NULL;
	int rc = -EIO;

	if (valpath == NULL || !strcmp(valpath, "-")) {
		f = stdin;
	} else {
		f = fopen(valpath, "rb");
		if (f == NULL) {
			fprintf(stderr, "store-file: cannot open value file '%s'\n", valpath);
			return -ENOENT;
		}
	}

	/* Read the whole value into a DMA buffer, growing as needed (a stream like
	 * stdin has no a-priori size). Stay within the controller max_io_size. */
	vbuf = (uint8_t *)nvfu_dma_alloc(cap, &vbuf_iova);
	if (vbuf == NULL) {
		fprintf(stderr, "store-file: DMA alloc failed\n");
		goto done;
	}
	for (;;) {
		size_t got;

		if (len == cap) {
			size_t ncap = cap * 2;
			uint8_t *nbuf;
			uint64_t niova = 0;

			if (ncap > MAX_VALUE) {
				ncap = MAX_VALUE;
			}
			if (ncap == cap) {
				fprintf(stderr, "store-file: value exceeds %u bytes (controller max)\n",
					MAX_VALUE);
				goto done;
			}
			nbuf = (uint8_t *)nvfu_dma_alloc(ncap, &niova);
			if (nbuf == NULL) {
				fprintf(stderr, "store-file: DMA grow to %zu failed\n", ncap);
				goto done;
			}
			memcpy(nbuf, vbuf, len);
			spdk_dma_free(vbuf);
			vbuf = nbuf;
			vbuf_iova = niova;
			cap = ncap;
		}
		got = fread(vbuf + len, 1, cap - len, f);
		len += got;
		if (got == 0) {
			if (ferror(f)) {
				fprintf(stderr, "store-file: read error on '%s'\n",
					valpath ? valpath : "stdin");
				goto done;
			}
			break;	/* EOF */
		}
	}
	spdk_wmb();

	/* GPU-initiated Store over a region-bounded SGL (not a 4 KiB PRP). */
	if (nvfu_kv_xfer_sgl(d, nsid, SPDK_NVME_OPC_KV_STORE, key,
			     (uint32_t)len, vbuf_iova, 0, 0, NULL) != 0) {
		fprintf(stderr, "store-file: KV Store failed\n");
		goto done;
	}
	printf("STORE-FILE ok: GPU stored key='%s' (%zu bytes, SGL)\n", key, len);
	rc = 0;
done:
	spdk_dma_free(vbuf);
	if (f != NULL && f != stdin) {
		fclose(f);
	}
	return rc;
}

static uint64_t
parse_size(const char *s)
{
	char *end = NULL;
	uint64_t v = strtoull(s, &end, 0);

	if (end && (*end == 'k' || *end == 'K')) {
		v *= 1024;
	} else if (end && (*end == 'm' || *end == 'M')) {
		v *= 1024u * 1024u;
	}
	return v;
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"usage:\n"
		"  %s <traddr> store      <key> <value>   GPU-initiated KV Store (value as argv)\n"
		"  %s <traddr> store-file <key> <file>    GPU-initiated KV Store (value from file or '-', SGL)\n"
		"  %s <traddr> retrieve   <key>           GPU-initiated KV Retrieve\n"
		"  %s <traddr> exec       <key> <op_id>   GPU-initiated KV Exec (10=bytecount, 11=identity)\n"
		"  %s <traddr> retrieve-batch [keyfile]  N GPU Retrieves (keys one/line from keyfile or stdin)\n"
		"                                         in ONE wavefront + ONE doorbell per chunk;\n"
		"                                         length-framed values to stdout (4-byte LE len + bytes)\n"
		"  %s <traddr> store-batch <prefix> <N>   N GPU Stores in ONE wavefront + ONE doorbell\n"
		"  %s <traddr> exec-batch <prefix> <op_id> <N>  N GPU Execs in ONE wavefront (two-tier)\n"
		"  %s <traddr> store-big  <key> <size>    GPU produces+stores a big value (e.g. 256K, 2M), verify\n"
		"  %s <traddr> exec-big   <key> <op_id> <size>  store big value + Exec, verify scattered result\n"
		"  %s <traddr> [demo]                     store+retrieve self-test\n"
		"  %s <traddr> demo-exec                  store+retrieve+exec self-test\n"
		"<traddr> is the VFIOUSER listener dir containing 'cntrl'.\n",
		prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	struct nvfu_dev d;
	char ealargs[64];
	const char *traddr, *mode, *key, *nsid_env;
	uint32_t nsid = KV_NSID;	/* NKVX_NSID overrides; default 1 (bead spdk-jhk.10) */
	int ndev = 0;
	int rc = 1;

	memset(&d, 0, sizeof(d));

	/* The target namespace defaults to KV_NSID (1); NKVX_NSID selects another. The
	 * positional CLI is unchanged, so default invocations behave exactly as before. */
	nsid_env = getenv("NKVX_NSID");
	if (nsid_env != NULL && nsid_env[0] != '\0') {
		nsid = (uint32_t)strtoul(nsid_env, NULL, 0);
		if (nsid == 0) {
			fprintf(stderr, "NKVX_NSID='%s' is not a valid nsid (>=1)\n", nsid_env);
			return 1;
		}
	}

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}
	traddr = argv[1];
	mode = (argc >= 3) ? argv[2] : "demo";

	/* Validate args per mode up front. */
	if ((!strcmp(mode, "store") && argc != 5) ||
	    (!strcmp(mode, "store-file") && argc != 5) ||
	    (!strcmp(mode, "retrieve") && argc != 4) ||
	    (!strcmp(mode, "exec") && argc != 5) ||
	    (!strcmp(mode, "retrieve-batch") && argc != 3 && argc != 4) ||
	    (!strcmp(mode, "store-batch") && argc != 5) ||
	    (!strcmp(mode, "store-big") && argc != 5) ||
	    (!strcmp(mode, "exec-big") && argc != 6) ||
	    (!strcmp(mode, "exec-batch") && argc != 6)) {
		usage(argv[0]);
		return 1;
	}

	if (hipGetDeviceCount(&ndev) != hipSuccess || ndev < 1) {
		fprintf(stderr, "no HIP device available\n");
		return 1;
	}

	/* retrieve-batch's stdout carries a binary length-framed stream only; move
	 * all bring-up chatter (SPDK env, attach, identify, GPU map) to stderr now,
	 * BEFORE any of it prints, and keep the real stdout for the frames. */
	if (!strcmp(mode, "retrieve-batch") && nkvx_redirect_stdout_to_stderr() != 0) {
		return 1;
	}

	opts.opts_size = sizeof(opts);
	spdk_env_opts_init(&opts);
	opts.name = "nkv_vfu_gpu";
	snprintf(ealargs, sizeof(ealargs), "--iova-mode=va%s",
		 getenv("NKV_EAL_DEBUG") ? " --log-level=lib.eal:8" : "");
	opts.env_context = ealargs;
	/*
	 * Single-file hugepage segments (bead spdk-6ar; mirrors the rkv CPU client
	 * fix in clients/rkv/csrc/nkvx_shim.c). The GPU retrieve/exec lanes DMA host
	 * buffers up to the controller max_io_size (NKVX_RETR_LANE_BUF = 64 MiB) that
	 * are VA-contiguous but span many 2 MiB hugepages. SPDK's vfio-user client
	 * registers that span as ONE DMA region carrying a SINGLE (fd, offset)
	 * (vfio_mr_map_notify -> spdk_mem_get_fd_and_offset of the region start), and
	 * the vfio-user target mmaps the whole region MAP_SHARED from that one fd.
	 * With the default one-file-per-page layout only the FIRST hugepage maps to
	 * the client's real memory; the controller's writes to the value/result
	 * buffer (2nd+ hugepage) land on unshared pages, so the client reads zeros.
	 * The region-bounded SGL DPTR splits the transfer correctly on the wire, but
	 * cannot fix the underlying single-fd backing — only single-file segments
	 * can. Single-file segments back the memseg list with ONE fd at contiguous
	 * offsets, so the target's single-fd mmap covers the whole region. The two
	 * single-file modes are mutually exclusive in DPDK, so unlink must be off.
	 */
	opts.hugepage_single_segments = true;
	opts.unlink_hugepage = false;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	/* CPU does one-time controller bring-up (admin queue + IO queue); the GPU
	 * then builds the SQE and rings the doorbell for each op. */
	if (nvfu_open(&d, traddr) != 0 || gpu_map(&d) != 0) {
		goto out;
	}
	g_use_gpu = 1;	/* every datapath op below is GPU-initiated */

	if (!strcmp(mode, "store")) {
		key = argv[3];
		const char *value = argv[4];
		uint32_t vlen = (uint32_t)strlen(value);
		void *vbuf;
		uint64_t vbuf_iova = 0;

		/* Store over a region-bounded SGL (not a single 4 KiB PRP) so an argv
		 * value >4 KiB no longer fails with KV Store status 0x6 (spdk-jhk.13). */
		vbuf = nvfu_dma_alloc(spdk_max(vlen, 1u), &vbuf_iova);
		if (vbuf == NULL) {
			fprintf(stderr, "store: DMA alloc failed\n");
			goto out;
		}
		if (vlen > 0) {
			memcpy(vbuf, value, vlen);
		}
		spdk_wmb();
		if (nvfu_kv_xfer_sgl(&d, nsid, SPDK_NVME_OPC_KV_STORE, key, vlen,
				     vbuf_iova, 0, 0, NULL) != 0) {
			fprintf(stderr, "store: KV Store failed\n");
			spdk_dma_free(vbuf);
			goto out;
		}
		spdk_dma_free(vbuf);
		printf("STORE ok: GPU stored key='%s' (%u bytes, SGL)\n", key, vlen);
		rc = 0;
	} else if (!strcmp(mode, "store-file")) {
		key = argv[3];
		if (gpu_store_file(&d, nsid, key, argv[4]) != 0) {
			goto out;
		}
		rc = 0;
	} else if (!strcmp(mode, "retrieve-batch")) {
		const char *keyfile = (argc == 4) ? argv[3] : NULL;	/* NULL = stdin */
		if (gpu_retrieve_batch(&d, nsid, keyfile) != 0) {
			goto out;
		}
		rc = 0;
	} else if (!strcmp(mode, "store-batch")) {
		const char *prefix = argv[3];
		uint32_t nbatch = (uint32_t)strtoul(argv[4], NULL, 0);
		if (gpu_store_batch(&d, nsid, prefix, nbatch) != 0) {
			goto out;
		}
		rc = 0;
	} else if (!strcmp(mode, "exec-batch")) {
		const char *prefix = argv[3];
		uint32_t op_id = (uint32_t)strtoul(argv[4], NULL, 0);
		uint32_t nbatch = (uint32_t)strtoul(argv[5], NULL, 0);
		if (gpu_exec_batch(&d, nsid, prefix, op_id, nbatch) != 0) {
			goto out;
		}
		rc = 0;
	} else if (!strcmp(mode, "store-big")) {
		key = argv[3];
		uint32_t size = (uint32_t)parse_size(argv[4]);
		if (gpu_store_big(&d, nsid, key, size) != 0) {
			goto out;
		}
		rc = 0;
	} else if (!strcmp(mode, "exec-big")) {
		key = argv[3];
		uint32_t op_id = (uint32_t)strtoul(argv[4], NULL, 0);
		uint32_t size = (uint32_t)parse_size(argv[5]);
		if (gpu_exec_big(&d, nsid, key, op_id, size) != 0) {
			goto out;
		}
		rc = 0;
	} else if (!strcmp(mode, "retrieve")) {
		char buf[4096];
		uint32_t got = 0;
		key = argv[3];
		if (nvfu_kv_retrieve(&d, nsid, key, buf, sizeof(buf) - 1, &got) != 0) {
			goto out;
		}
		buf[got] = '\0';
		printf("RETRIEVE ok: GPU retrieved key='%s' (%u bytes): %s\n", key, got, buf);
		rc = 0;
	} else if (!strcmp(mode, "exec")) {
		uint8_t exbuf[4096];
		uint32_t op_id = (uint32_t)strtoul(argv[4], NULL, 0);
		uint32_t exlen = 0;
		key = argv[3];
		memset(exbuf, 0, sizeof(exbuf));
		if (nvfu_kv_exec(&d, nsid, key, op_id, NULL, 0, exbuf, sizeof(exbuf), &exlen) != 0) {
			goto out;
		}
		if (op_id == 10) {	/* nkvx:bytecount -> LE u64 */
			uint64_t count = 0;
			memcpy(&count, exbuf, (exlen < sizeof(count)) ? exlen : sizeof(count));
			printf("EXEC ok: GPU exec op %u (nkvx:bytecount) key='%s' -> count=%llu\n",
			       op_id, key, (unsigned long long)count);
		} else {		/* nkvx:identity and friends -> raw bytes */
			printf("EXEC ok: GPU exec op %u key='%s' -> %u bytes: %.*s\n",
			       op_id, key, exlen, (int)exlen, (char *)exbuf);
		}
		rc = 0;
	} else {
		/* self-test: store -> retrieve (-> exec) round-trip */
		const char *k = "kvgpu01";
		const char *v = "GPU-initiated near-data KV write over vfio-user";
		char buf[256];
		uint32_t got = 0;
		int demo_exec = !strcmp(mode, "demo-exec") || !strcmp(mode, "exec");

		memset(buf, 0, sizeof(buf));
		if (nvfu_kv_store(&d, nsid, k, v, (uint32_t)strlen(v)) != 0) {
			goto out;
		}
		printf("GPU-initiated KV Store OK (key='%s', %zu bytes)\n", k, strlen(v));
		if (nvfu_kv_retrieve(&d, nsid, k, buf, sizeof(buf) - 1, &got) != 0) {
			goto out;
		}
		printf("Retrieve OK (%u bytes): '%s'\n", got, buf);
		if (got != strlen(v) || memcmp(buf, v, got) != 0) {
			fprintf(stderr, "FAIL: value mismatch\n");
			goto out;
		}
		if (demo_exec) {
			uint8_t exbuf[256];
			uint32_t exlen = 0;
			uint64_t bytecount = 0;

			if (nvfu_kv_exec(&d, nsid, k, 10, NULL, 0, exbuf, sizeof(exbuf), &exlen) != 0) {
				goto out;
			}
			memcpy(&bytecount, exbuf, (exlen < 8) ? exlen : 8);
			printf("Exec op10 nkvx:bytecount -> count=%llu (expect %zu)\n",
			       (unsigned long long)bytecount, strlen(v));
			memset(exbuf, 0, sizeof(exbuf));
			if (nvfu_kv_exec(&d, nsid, k, 11, NULL, 0, exbuf, sizeof(exbuf), &exlen) != 0) {
				goto out;
			}
			printf("Exec op11 nkvx:identity -> %u bytes: '%.*s'\n",
			       exlen, (int)exlen, (char *)exbuf);
			if (bytecount != strlen(v) || exlen != strlen(v) ||
			    memcmp(exbuf, v, exlen) != 0) {
				fprintf(stderr, "FAIL: Exec result mismatch\n");
				goto out;
			}
		}
		printf("PASS: QEMU-less GPU-initiated host IO%s complete.\n",
		       demo_exec ? " + rados-nkvx Exec" : "");
		rc = 0;
	}

out:
	if (g_sq_dev) {
		hipHostUnregister(d.io.sq);
	}
	nvfu_close(&d);
	return rc;
}
