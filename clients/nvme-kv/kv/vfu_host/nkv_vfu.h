/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Shared raw vfio-user NVMe-KV driver for the Option-A host client
 * (epic spdk-jhk, bead spdk-jhk.3). Used by both:
 * nkv_vfu_host.c -- CPU builds the SQE + rings the doorbell (2c)
 * nkv_vfu_gpu.hip -- GPU builds the SQE + rings the doorbell (2d)
 *
 * The only difference between the two is nvfu_produce(): the producer step that
 * writes the SQE into the SQ ring and rings the SQ doorbell. Each main provides
 * its own (CPU or GPU). Everything else -- attach, admin queue, controller
 * enable, IO queue creation, completion polling -- is shared and identical.
 */

#ifndef NKV_VFU_H
#define NKV_VFU_H

#include "spdk/stdinc.h"
#include "spdk/barrier.h"
#include "spdk/env.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvme_kv.h"
#include "spdk/vfio_user_pci.h"

#include <linux/vfio.h>		/* VFIO_PCI_*_REGION_INDEX */

/*
 * KV vendor extensions (Exec osize/op_id in CDW12/13, Store TTL in CDW12) were
 * carried as named bitfields in the OLD in-tree SPDK fork's nvme_spec.h. The
 * out-of-tree base SPDK (Gerrit 28298 KV series) ships only the ratified KV
 * opcodes and has no cdw12_bits.kv_exec / cdw12_bits.kv_store / TTL_VALID, so
 * those references are written via the raw CDW words here (the named fields were
 * full-width :32, so cmd.cdwNN == cmd.cdwNN_bits.<field>) and the TTL Store
 * Option bit is provided as a fallback. This keeps nkv_vfu.h building against
 * BOTH the old fork and the unmodified base, with byte-identical wire output.
 */
#ifndef SPDK_NVME_KV_STORE_OPT_TTL_VALID
#define SPDK_NVME_KV_STORE_OPT_TTL_VALID (1u << 3)
#endif
/* KV Exec is a vendor opcode (ADR-0005); 0x83 in the old fork. Absent from the
 * ratified base spec, so define it for the out-of-tree client build. */
#ifndef SPDK_NVME_OPC_KV_EXEC
#define SPDK_NVME_OPC_KV_EXEC 0x83
#endif

#define ADMIN_Q_ENTRIES	16
#define IO_Q_ENTRIES	128
#define IO_QID		1
#define NVME_DB_OFFSET	0x1000
#define KV_NSID		1

struct nvfu_queue {
	struct spdk_nvme_cmd	*sq;
	struct spdk_nvme_cpl	*cq;
	uint64_t		sq_iova;
	uint64_t		cq_iova;
	uint16_t		qid;
	uint16_t		depth;
	uint16_t		sq_tail;
	uint16_t		cq_head;
	uint8_t			cq_phase;
	uint16_t		cid;
	uint32_t		sq_db;
	uint32_t		cq_db;
};

struct nvfu_dev {
	struct vfio_device	*dev;
	volatile uint32_t	*doorbells;
	uint32_t		db_stride_u32;
	struct nvfu_queue	admin;
	struct nvfu_queue	io;
};

/*
 * Producer step (write SQE into SQ slot, ring SQ doorbell). Provided by each
 * main: CPU stores in nkv_vfu_host.c, a GPU kernel in nkv_vfu_gpu.hip. `slot`
 * is the SQ index to write; `new_tail` is the doorbell value to ring.
 */
int nvfu_produce(struct nvfu_dev *d, struct nvfu_queue *q,
		 const struct spdk_nvme_cmd *cmd, uint16_t slot, uint16_t new_tail);

static inline int
nvfu_bar(struct nvfu_dev *d, uint32_t region, uint32_t off, size_t len,
	 void *buf, bool is_write)
{
	return spdk_vfio_user_pci_bar_access(d->dev, region, off, len, buf, is_write);
}

static inline int nvfu_reg_get4(struct nvfu_dev *d, uint32_t off, uint32_t *v)
{ return nvfu_bar(d, VFIO_PCI_BAR0_REGION_INDEX, off, 4, v, false); }
static inline int nvfu_reg_get8(struct nvfu_dev *d, uint32_t off, uint64_t *v)
{ return nvfu_bar(d, VFIO_PCI_BAR0_REGION_INDEX, off, 8, v, false); }
static inline int nvfu_reg_set4(struct nvfu_dev *d, uint32_t off, uint32_t v)
{ return nvfu_bar(d, VFIO_PCI_BAR0_REGION_INDEX, off, 4, &v, true); }
static inline int nvfu_reg_set8(struct nvfu_dev *d, uint32_t off, uint64_t v)
{ return nvfu_bar(d, VFIO_PCI_BAR0_REGION_INDEX, off, 8, &v, true); }

#define REG_OFF(field) ((uint32_t)offsetof(struct spdk_nvme_registers, field))

/* iova == vaddr for vfio-user (IOVA-as-VA); see nkv_vfu_host.c notes. */
static inline void *
nvfu_dma_alloc(size_t size, uint64_t *iova)
{
	void *p = spdk_dma_zmalloc(size, 0x1000, NULL);

	if (p && iova) {
		*iova = (uint64_t)(uintptr_t)p;
	}
	return p;
}

static inline void
nvfu_queue_init_db(struct nvfu_dev *d, struct nvfu_queue *q)
{
	q->sq_db = (2u * q->qid) * d->db_stride_u32;
	q->cq_db = (2u * q->qid + 1u) * d->db_stride_u32;
	q->sq_tail = q->cq_head = 0;
	q->cq_phase = 1;
	q->cid = 0;
}

static inline int
nvfu_submit_poll(struct nvfu_dev *d, struct nvfu_queue *q,
		 struct spdk_nvme_cmd *cmd, struct spdk_nvme_cpl *out_cpl)
{
	volatile struct spdk_nvme_cpl *cqe;
	uint64_t deadline;
	uint16_t slot, new_tail;

	cmd->cid = q->cid++;
	slot = q->sq_tail;
	new_tail = (q->sq_tail + 1) % q->depth;

	nvfu_produce(d, q, cmd, slot, new_tail);	/* CPU or GPU */
	q->sq_tail = new_tail;

	cqe = &q->cq[q->cq_head];
	deadline = spdk_get_ticks() + 5 * spdk_get_ticks_hz();
	while (cqe->status.p != q->cq_phase) {
		if (spdk_get_ticks() > deadline) {
			fprintf(stderr, "qid:%u opc=0x%x TIMEOUT\n", q->qid, cmd->opc);
			return -ETIMEDOUT;
		}
		spdk_pause();
	}
	spdk_rmb();
	if (out_cpl) {
		*out_cpl = *(struct spdk_nvme_cpl *)(uintptr_t)cqe;
	}
	q->cq_head = (q->cq_head + 1) % q->depth;
	if (q->cq_head == 0) {
		q->cq_phase ^= 1;
	}
	d->doorbells[q->cq_db] = q->cq_head;
	return cqe->status.sc | (cqe->status.sct << 8);
}

static inline int
nvfu_pci_enable_dma(struct nvfu_dev *d)
{
	uint16_t cmd_reg = 0;

	if (nvfu_bar(d, VFIO_PCI_CONFIG_REGION_INDEX, 4, 2, &cmd_reg, false)) {
		return -EIO;
	}
	cmd_reg |= 0x404;	/* bus master + INTx disable */
	return nvfu_bar(d, VFIO_PCI_CONFIG_REGION_INDEX, 4, 2, &cmd_reg, true);
}

static inline int
nvfu_controller_enable(struct nvfu_dev *d)
{
	union spdk_nvme_cap_register cap;
	union spdk_nvme_cc_register cc;
	union spdk_nvme_csts_register csts;
	union spdk_nvme_aqa_register aqa;
	struct nvfu_queue *q = &d->admin;
	uint64_t to_us, deadline;

	if (nvfu_reg_get8(d, REG_OFF(cap), &cap.raw)) {
		return -EIO;
	}
	d->db_stride_u32 = 1u << cap.bits.dstrd;

	q->qid = 0;
	q->depth = ADMIN_Q_ENTRIES;
	q->sq = (struct spdk_nvme_cmd *)nvfu_dma_alloc(q->depth * sizeof(*q->sq), &q->sq_iova);
	q->cq = (struct spdk_nvme_cpl *)nvfu_dma_alloc(q->depth * sizeof(*q->cq), &q->cq_iova);
	if (!q->sq || !q->cq) {
		fprintf(stderr, "admin queue DMA alloc failed\n");
		return -ENOMEM;
	}
	nvfu_queue_init_db(d, q);

	aqa.raw = 0;
	aqa.bits.asqs = q->depth - 1;
	aqa.bits.acqs = q->depth - 1;
	if (nvfu_reg_set4(d, REG_OFF(aqa.raw), aqa.raw) ||
	    nvfu_reg_set8(d, REG_OFF(asq), q->sq_iova) ||
	    nvfu_reg_set8(d, REG_OFF(acq), q->cq_iova)) {
		return -EIO;
	}

	cc.raw = 0;
	cc.bits.en = 1;
	cc.bits.css = SPDK_NVME_CC_CSS_IOCS;
	cc.bits.iosqes = 6;
	cc.bits.iocqes = 4;
	if (nvfu_reg_set4(d, REG_OFF(cc.raw), cc.raw)) {
		return -EIO;
	}

	to_us = (uint64_t)cap.bits.to * 500ULL * 1000ULL;
	deadline = spdk_get_ticks() + (to_us * spdk_get_ticks_hz()) / 1000000ULL;
	do {
		if (nvfu_reg_get4(d, REG_OFF(csts.raw), &csts.raw)) {
			return -EIO;
		}
		if (csts.bits.cfs) {
			fprintf(stderr, "controller fatal status during enable\n");
			return -EIO;
		}
		if (csts.bits.rdy) {
			return 0;
		}
		usleep(1000);
	} while (spdk_get_ticks() < deadline);

	fprintf(stderr, "timeout waiting for CSTS.RDY\n");
	return -ETIMEDOUT;
}

static inline int
nvfu_identify_controller(struct nvfu_dev *d)
{
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_ctrlr_data *cdata;
	uint64_t iova;
	int status;

	memset(&cmd, 0, sizeof(cmd));
	cdata = (struct spdk_nvme_ctrlr_data *)nvfu_dma_alloc(4096, &iova);
	if (!cdata) {
		return -ENOMEM;
	}
	cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	cmd.dptr.prp.prp1 = iova;
	cmd.cdw10 = 0x01;
	status = nvfu_submit_poll(d, &d->admin, &cmd, NULL);
	if (status != 0) {
		fprintf(stderr, "Identify Controller failed: status=0x%x\n", status);
		spdk_dma_free(cdata);
		return -EIO;
	}
	printf("Identify Controller OK: Model '%.40s' NN=%u\n", cdata->mn, cdata->nn);
	spdk_dma_free(cdata);
	return 0;
}

static inline int
nvfu_create_io_queue(struct nvfu_dev *d)
{
	struct nvfu_queue *q = &d->io;
	struct spdk_nvme_cmd cmd;
	int status;

	q->qid = IO_QID;
	q->depth = IO_Q_ENTRIES;
	q->sq = (struct spdk_nvme_cmd *)nvfu_dma_alloc(q->depth * sizeof(*q->sq), &q->sq_iova);
	q->cq = (struct spdk_nvme_cpl *)nvfu_dma_alloc(q->depth * sizeof(*q->cq), &q->cq_iova);
	if (!q->sq || !q->cq) {
		fprintf(stderr, "IO queue DMA alloc failed\n");
		return -ENOMEM;
	}
	nvfu_queue_init_db(d, q);

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_CREATE_IO_CQ;
	cmd.dptr.prp.prp1 = q->cq_iova;
	cmd.cdw10 = ((uint32_t)(q->depth - 1) << 16) | q->qid;
	cmd.cdw11 = 0x1;	/* PC=1, IEN=0 */
	status = nvfu_submit_poll(d, &d->admin, &cmd, NULL);
	if (status != 0) {
		fprintf(stderr, "Create IO CQ failed: status=0x%x\n", status);
		return -EIO;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_NVME_OPC_CREATE_IO_SQ;
	cmd.dptr.prp.prp1 = q->sq_iova;
	cmd.cdw10 = ((uint32_t)(q->depth - 1) << 16) | q->qid;
	cmd.cdw11 = ((uint32_t)q->qid << 16) | 0x1;
	status = nvfu_submit_poll(d, &d->admin, &cmd, NULL);
	if (status != 0) {
		fprintf(stderr, "Create IO SQ failed: status=0x%x\n", status);
		return -EIO;
	}
	printf("IO queue qid:%u created (depth=%u, SQ doorbell idx=%u).\n",
	       q->qid, q->depth, q->sq_db);
	return 0;
}

static inline void
nvfu_kv_set_key(struct spdk_nvme_cmd *cmd, const char *key, uint8_t key_len)
{
	cmd->cdw11_bits.kv.kl = key_len;
	memcpy((uint8_t *)&cmd->cdw2, key, spdk_min(key_len, 8));
	if (key_len > 8) {
		memcpy((uint8_t *)&cmd->cdw14, key + 8, (size_t)(key_len - 8));
	}
}

static inline int nvfu_kv_store_lk(struct nvfu_dev *d, uint32_t nsid, const char *key,
				   uint8_t key_len, const void *value, uint32_t value_len);
static inline int nvfu_kv_retrieve_lk(struct nvfu_dev *d, uint32_t nsid, const char *key,
				      uint8_t key_len, void *out, uint32_t out_len,
				      uint32_t *got_len);

/*
 * KV Store for keys <= 16 bytes. The out-of-tree bdev_kvrados forwarder (and the
 * rados-nkvx executor it forwards to) parse the key from the IN-PAYLOAD
 * [u16 key_len][key][value] prefix at the head of the DPTR — the legacy CDW key
 * slots (CDW2/3/14/15) are NOT consulted for our command set. So this short-key
 * helper routes through the same in-payload encoding as the long-key path; the
 * encoding is uniform for any key 1..255 B (ADR-0014). (It previously staged the
 * key in CDW slots, which the in-tree kvdev read but the forwarder ignores.)
 */
static inline int
nvfu_kv_store(struct nvfu_dev *d, uint32_t nsid, const char *key, const void *value,
	      uint32_t value_len)
{
	return nvfu_kv_store_lk(d, nsid, key, (uint8_t)strlen(key), value, value_len);
}

/* One vfio-user DMA region == one DPDK hugepage (2 MiB). The target maps each
 * region independently (max_nr_sgs=1 per descriptor), so a single SGL data-block
 * descriptor must not cross a region boundary. nvfu_sgl_set_dptr (defined below)
 * builds a region-bounded DPTR; forward-declared here for the helpers above it. */
#define NVFU_DMA_REGION	(2ULL * 1024 * 1024)
static inline struct spdk_nvme_sgl_descriptor *
nvfu_sgl_set_dptr(struct spdk_nvme_cmd *cmd, uint64_t buf_iova, uint32_t len, int *err);

/*
 * KV Retrieve for keys <= 16 bytes — routes through the in-payload long-key path
 * for the same reason as nvfu_kv_store above (the forwarder reads the key from
 * the [u16 key_len][key] DPTR prefix, not the CDW key slots).
 */
static inline int
nvfu_kv_retrieve(struct nvfu_dev *d, uint32_t nsid, const char *key, void *out,
		 uint32_t out_len, uint32_t *got_len)
{
	return nvfu_kv_retrieve_lk(d, nsid, key, (uint8_t)strlen(key), out, out_len, got_len);
}

/*
 * (docs/wire-format.md "Keys of 17 to 255 bytes — in the payload"):
 * long-key KV Store. A key longer than the 16-byte inline cap rides
 * length-prefixed at the HEAD of the DPTR payload, exactly as KV Exec does:
 * [u16 key_len][key_len key bytes][value ...]
 * The inline Key Length (CDW11 bits 7:0) is left 0 to signal the long-key path,
 * and CDW10 (vsize) carries the VALUE size only (not the key prefix). This works
 * for any key 1..255 B; callers use it specifically for keys > 16 B.
 */
static inline int
nvfu_kv_store_lk(struct nvfu_dev *d, uint32_t nsid, const char *key, uint8_t key_len,
		 const void *value, uint32_t value_len)
{
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_sgl_descriptor *segs;
	void *buf;
	uint64_t iova;
	uint16_t klp = key_len;
	uint32_t payload_len = (uint32_t)sizeof(uint16_t) + key_len + value_len;
	uint32_t bufsz = spdk_max(payload_len, 4096);
	int status, err = 0;

	memset(&cmd, 0, sizeof(cmd));
	buf = nvfu_dma_alloc(bufsz, &iova);
	if (!buf) {
		return -ENOMEM;
	}
	/* Stage [u16 key_len][key][value] at the buffer head. */
	memcpy(buf, &klp, sizeof(klp));
	memcpy((char *)buf + sizeof(klp), key, key_len);
	if (value_len) {
		memcpy((char *)buf + sizeof(klp) + key_len, value, value_len);
	}
	spdk_wmb();

	cmd.opc = SPDK_NVME_OPC_KV_STORE;
	cmd.nsid = nsid;
	cmd.cdw11_bits.kv.kl = 0;		/* long-key signal: real length is the u16 prefix */
	cmd.cdw10_bits.kv.vsize = value_len;	/* VALUE size only (not the key prefix) */

	/* The target gathers payload_len bytes (key prefix + value) on the way in. */
	segs = nvfu_sgl_set_dptr(&cmd, iova, payload_len, &err);
	if (err != 0) {
		spdk_dma_free(buf);
		return err;
	}
	status = nvfu_submit_poll(d, &d->io, &cmd, NULL);
	if (segs != NULL) {
		spdk_dma_free(segs);
	}
	spdk_dma_free(buf);
	if (status != 0) {
		fprintf(stderr, "KV Store (long key) failed: status=0x%x\n", status);
		return -EIO;
	}
	return 0;
}

/*
 * long-key KV Retrieve. The DPTR carries [u16 key_len][key] at its head
 * (host->device); per the ratified wire format (docs/wire-format.md), the value
 * is returned into the segment FOLLOWING the key head (device->host), i.e. at
 * offset value_off = sizeof(u16)+key_len — NOT at offset 0. cpl.cdw0 reports the
 * true stored length. CDW10 (vsize) is the host buffer size for the value. The
 * buffer must hold the [u16 key_len][key] head AND the returned value after it.
 */
static inline int
nvfu_kv_retrieve_lk(struct nvfu_dev *d, uint32_t nsid, const char *key, uint8_t key_len,
		    void *out, uint32_t out_len, uint32_t *got_len)
{
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_cpl cpl;
	struct spdk_nvme_sgl_descriptor *segs;
	void *buf;
	uint64_t iova;
	uint16_t klp = key_len;
	uint32_t head_len = (uint32_t)sizeof(uint16_t) + key_len;
	/* The DPTR region carries the key head IN (host->device) followed by the
	 * value OUT (device->host) at offset head_len — the value is returned into
	 * the segment FOLLOWING the key head (docs/wire-format.md), so the mapped
	 * region must span head_len + out_len. */
	uint32_t xfer_len = head_len + out_len;
	uint32_t bufsz = spdk_max(xfer_len, 4096);
	int status, err = 0;

	memset(&cmd, 0, sizeof(cmd));
	memset(&cpl, 0, sizeof(cpl));
	buf = nvfu_dma_alloc(bufsz, &iova);
	if (!buf) {
		return -ENOMEM;
	}
	/* Stage [u16 key_len][key] at the buffer head (host->device). */
	memcpy(buf, &klp, sizeof(klp));
	memcpy((char *)buf + sizeof(klp), key, key_len);
	spdk_wmb();

	cmd.opc = SPDK_NVME_OPC_KV_RETRIEVE;
	cmd.nsid = nsid;
	cmd.cdw11_bits.kv.kl = 0;		/* long-key signal: real length is the u16 prefix */
	cmd.cdw10_bits.kv.vsize = out_len;	/* host buffer size for the value */

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
		fprintf(stderr, "KV Retrieve (long key) failed: status=0x%x\n", status);
		spdk_dma_free(buf);
		return -EIO;
	}
	spdk_rmb();
	*got_len = spdk_min(cpl.cdw0, out_len);
	/* The value was returned at offset head_len (after the [u16 key_len][key]
	 * head), per the ratified wire format. */
	memcpy(out, (char *)buf + head_len, *got_len);
	spdk_dma_free(buf);
	return 0;
}

/*
 * (docs/wire-format.md): long-key KV Delete (0x10) and Exist (0x14).
 * These carry NO value, but a long key (> 16 B) still does not fit the inline
 * CDW2/3/14/15 slots, so it rides length-prefixed at the HEAD of the DPTR
 * payload exactly like Store/Retrieve -- just the [u16 key_len][key] head,
 * host->device, no value. The inline Key Length (CDW11 bits 7:0) is left 0 to
 * select the long-key path; CDW10 is 0 (no value transfer).
 *
 * Returns the raw NVMe status (sc | sct<<8) from the completion so the caller
 * can distinguish SUCCESS (0x00) from "key does not exist" (sc 0x87): Exist of
 * a present key and Delete of a present key complete 0x00; either on an absent
 * key reports 0x87. Negative returns are transport/setup errors.
 */
static inline int
nvfu_kv_op_lk(struct nvfu_dev *d, uint32_t nsid, uint8_t opc, const char *key,
	      uint8_t key_len)
{
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_sgl_descriptor *segs;
	void *buf;
	uint64_t iova;
	uint16_t klp = key_len;
	uint32_t head_len = (uint32_t)sizeof(uint16_t) + key_len;
	uint32_t bufsz = spdk_max(head_len, 4096);
	int status, err = 0;

	memset(&cmd, 0, sizeof(cmd));
	buf = nvfu_dma_alloc(bufsz, &iova);
	if (!buf) {
		return -ENOMEM;
	}
	/* Stage [u16 key_len][key] at the buffer head (host->device, no value). */
	memcpy(buf, &klp, sizeof(klp));
	memcpy((char *)buf + sizeof(klp), key, key_len);
	spdk_wmb();

	cmd.opc = opc;
	cmd.nsid = nsid;
	cmd.cdw11_bits.kv.kl = 0;		/* long-key signal: real length is the u16 prefix */
	cmd.cdw10_bits.kv.vsize = 0;		/* no value transfer */

	/* The target gathers head_len bytes (the key prefix) on the way in. */
	segs = nvfu_sgl_set_dptr(&cmd, iova, head_len, &err);
	if (err != 0) {
		spdk_dma_free(buf);
		return err;
	}
	status = nvfu_submit_poll(d, &d->io, &cmd, NULL);
	if (segs != NULL) {
		spdk_dma_free(segs);
	}
	spdk_dma_free(buf);
	return status;
}

/* long-key KV Delete. See nvfu_kv_op_lk for the return convention. */
static inline int
nvfu_kv_delete_lk(struct nvfu_dev *d, uint32_t nsid, const char *key, uint8_t key_len)
{
	return nvfu_kv_op_lk(d, nsid, SPDK_NVME_OPC_KV_DELETE, key, key_len);
}

/* long-key KV Exist. See nvfu_kv_op_lk for the return convention. */
static inline int
nvfu_kv_exist_lk(struct nvfu_dev *d, uint32_t nsid, const char *key, uint8_t key_len)
{
	return nvfu_kv_op_lk(d, nsid, SPDK_NVME_OPC_KV_EXIST, key, key_len);
}

/*
 * KV Exec (ADR-0014, opcode 0x83): near-data compute. The op runs an
 * allowlisted module (selected by op_id) against the value stored under `key`,
 * read-only. The request rides the DPTR payload head as [u16 key_len][key]
 * [input]; the result comes back in the same buffer, its length in cpl.cdw0.
 * The key does NOT go in the inline CDW slots.
 */
static inline int
nvfu_kv_exec(struct nvfu_dev *d, uint32_t nsid, const char *key, uint32_t op_id,
	     const void *input, uint32_t input_len,
	     void *out, uint32_t out_len, uint32_t *result_len)
{
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_cpl cpl;
	struct spdk_nvme_sgl_descriptor *segs;
	void *buf;
	uint64_t iova;
	uint8_t key_len = (uint8_t)strlen(key);
	uint16_t klp = key_len;
	uint32_t payload_len = (uint32_t)sizeof(uint16_t) + key_len + input_len;
	uint32_t bufsz = spdk_max(out_len, 4096);
	/* The target maps max(vsize, osize) bytes of the single DPTR buffer (input
	 * gathered in, result scattered back), so the SGL must describe exactly that. */
	uint32_t xfer_len = spdk_max(payload_len, out_len);
	int status, err = 0;

	memset(&cmd, 0, sizeof(cmd));
	memset(&cpl, 0, sizeof(cpl));
	if (payload_len > bufsz) {
		return -EINVAL;
	}
	buf = nvfu_dma_alloc(bufsz, &iova);
	if (!buf) {
		return -ENOMEM;
	}
	/* Stage [u16 key_len][key][input] at the buffer head. */
	memcpy(buf, &klp, sizeof(klp));
	memcpy((char *)buf + sizeof(klp), key, key_len);
	if (input_len) {
		memcpy((char *)buf + sizeof(klp) + key_len, input, input_len);
	}
	spdk_wmb();

	cmd.opc = SPDK_NVME_OPC_KV_EXEC;
	cmd.nsid = nsid;
	cmd.cdw10_bits.kv.vsize = payload_len;		/* request payload length */
	cmd.cdw12 = out_len;		/* KV Exec output buffer size (vendor ext) */
	cmd.cdw13 = op_id;		/* KV Exec op_id (vendor ext) */

	/* Region-bounded SGL so a >2 MiB result scatters correctly (a single
	 * data-block descriptor cannot cross a 2 MiB DMA region). */
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
	*result_len = spdk_min(cpl.cdw0, out_len);
	memcpy(out, buf, *result_len);
	spdk_dma_free(buf);
	return 0;
}

/*
 * Point cmd's DPTR at [buf_iova, buf_iova + len) as an SGL. A buffer that fits
 * within a single 2 MiB DMA region uses one contiguous data-block descriptor; a
 * larger buffer is described by a segment list with one region-bounded data
 * block per chunk, so each descriptor maps to exactly one region on the target
 * (which then gathers/scatters across the resulting iovecs). Bounded by
 * NVMF_REQ_MAX_BUFFERS (33) descriptors -> 64 MiB max_io_size.
 *
 * Returns the descriptor-list DMA buffer that the caller must spdk_dma_free()
 * after the command completes (NULL for the single-descriptor case). On OOM,
 * sets *err and returns NULL.
 */
static inline struct spdk_nvme_sgl_descriptor *
nvfu_sgl_set_dptr(struct spdk_nvme_cmd *cmd, uint64_t buf_iova, uint32_t len, int *err)
{
	struct spdk_nvme_sgl_descriptor *segs;
	uint64_t segs_iova = 0, off;
	/* Bytes from buf_iova to the next region boundary. */
	uint64_t first_chunk = NVFU_DMA_REGION - (buf_iova & (NVFU_DMA_REGION - 1));
	uint32_t nseg, i;

	*err = 0;

	if (len <= first_chunk) {
		/* Single region: one contiguous data block. */
		cmd->psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
		cmd->dptr.sgl1.address = buf_iova;
		cmd->dptr.sgl1.unkeyed.length = len;
		cmd->dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		return NULL;
	}

	/* Multi-region: build a region-bounded data-block list. */
	nseg = 1 + (uint32_t)((len - first_chunk + NVFU_DMA_REGION - 1) / NVFU_DMA_REGION);
	segs = (struct spdk_nvme_sgl_descriptor *)nvfu_dma_alloc(nseg * sizeof(*segs), &segs_iova);
	if (segs == NULL) {
		*err = -ENOMEM;
		return NULL;
	}

	off = 0;
	for (i = 0; i < nseg; i++) {
		uint64_t chunk = (i == 0) ? first_chunk : NVFU_DMA_REGION;

		if (off + chunk > len) {
			chunk = len - off;
		}
		memset(&segs[i], 0, sizeof(segs[i]));
		segs[i].address = buf_iova + off;
		segs[i].unkeyed.length = (uint32_t)chunk;
		segs[i].unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		off += chunk;
	}

	cmd->psdt = SPDK_NVME_PSDT_SGL_MPTR_SGL;
	cmd->dptr.sgl1.address = segs_iova;
	cmd->dptr.sgl1.unkeyed.length = nseg * sizeof(*segs);
	cmd->dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
	return segs;
}

/*
 * Like nvfu_sgl_set_dptr, but the SGL's FIRST data block is a separate in-payload
 * key head [u16 key_len][key] (head_iova/head_len, its own small DMA region),
 * followed by the value buffer's region-bounded data blocks (val_iova/val_len).
 * The target gathers [key head][value] as one logical payload while the VALUE
 * buffer stays a SEPARATE, untouched region — so it can originate from VRAM /
 * p2pdma and (once the store value-bulk slice lands) be RDMA'd zero-copy. The head
 * is <= 257 B (one region); the value is split one data block per 2 MiB region.
 * Always emits a segment list (>= 1 head block, plus 0..N value blocks). Returns
 * the descriptor-list DMA buffer the caller must spdk_dma_free(); NULL + *err OOM.
 */
static inline struct spdk_nvme_sgl_descriptor *
nvfu_sgl_set_dptr_kv(struct spdk_nvme_cmd *cmd, uint64_t head_iova, uint32_t head_len,
		     uint64_t val_iova, uint32_t val_len, int *err)
{
	struct spdk_nvme_sgl_descriptor *segs;
	uint64_t segs_iova = 0, off;
	uint64_t first_chunk = NVFU_DMA_REGION - (val_iova & (NVFU_DMA_REGION - 1));
	uint32_t val_nseg, nseg, i;

	*err = 0;

	if (val_len == 0) {
		val_nseg = 0;
	} else if (val_len <= first_chunk) {
		val_nseg = 1;
	} else {
		val_nseg = 1 + (uint32_t)((val_len - first_chunk + NVFU_DMA_REGION - 1) / NVFU_DMA_REGION);
	}
	nseg = 1 + val_nseg;	/* key head + value blocks */

	segs = (struct spdk_nvme_sgl_descriptor *)nvfu_dma_alloc(nseg * sizeof(*segs), &segs_iova);
	if (segs == NULL) {
		*err = -ENOMEM;
		return NULL;
	}

	/* seg[0]: the in-payload key head (its own region). */
	memset(&segs[0], 0, sizeof(segs[0]));
	segs[0].address = head_iova;
	segs[0].unkeyed.length = head_len;
	segs[0].unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;

	/* seg[1..]: the value buffer, one region-bounded data block per 2 MiB region. */
	off = 0;
	for (i = 0; i < val_nseg; i++) {
		uint64_t chunk = (i == 0) ? first_chunk : NVFU_DMA_REGION;

		if (off + chunk > val_len) {
			chunk = val_len - off;
		}
		memset(&segs[1 + i], 0, sizeof(segs[1 + i]));
		segs[1 + i].address = val_iova + off;
		segs[1 + i].unkeyed.length = (uint32_t)chunk;
		segs[1 + i].unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		off += chunk;
	}

	cmd->psdt = SPDK_NVME_PSDT_SGL_MPTR_SGL;
	cmd->dptr.sgl1.address = segs_iova;
	cmd->dptr.sgl1.unkeyed.length = nseg * sizeof(*segs);
	cmd->dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
	return segs;
}

/* KV Store/Retrieve of a value buffer transferred via a region-bounded SGL.
 * nsid is threaded explicitly (default-preserving: callers pass KV_NSID to keep
 * prior behaviour) so the rados-nkv CLI can address any namespace.
 *
 * IN-PAYLOAD LONG-KEY (ADR-0014): the forwarder reads the key from a
 * [u16 key_len][key] prefix at the HEAD of the DPTR, NOT the CDW key slots. We
 * stage that head in its own tiny DMA region and describe it as the first SGL
 * data block, AHEAD of the (untouched) value buffer — keeping the value a
 * separate region so it stays VRAM/p2pdma-capable. CDW11.kl is left 0 (long-key
 * signal); CDW10 (vsize) carries the VALUE size only (not the key head).
 *
 * ro carries the CDW11 Request Options byte (Store Option bits: SIKE/SINKE,
 * TTL_VALID, EPHEMERAL, TOUCH) and ttl the CDW12 TTL in seconds; both are
 * meaningful only for KV Store and ignored for Retrieve. Callers that want the
 * prior behaviour pass ro=0, ttl=0 (spdk-jhk.7.14). */
static inline int
nvfu_kv_xfer_sgl(struct nvfu_dev *d, uint32_t nsid, uint8_t opc, const char *key,
		 uint32_t size, uint64_t buf_iova, uint8_t ro, uint32_t ttl,
		 struct spdk_nvme_cpl *out_cpl)
{
	struct spdk_nvme_cmd cmd;
	struct spdk_nvme_sgl_descriptor *segs;
	void *head;
	uint64_t head_iova = 0;
	uint8_t key_len = (uint8_t)strlen(key);
	uint16_t klp = key_len;
	uint32_t head_len = (uint32_t)sizeof(uint16_t) + key_len;
	int rc, err = 0;

	/* Stage [u16 key_len][key] in its own small DMA region (the in-payload head). */
	head = nvfu_dma_alloc(head_len, &head_iova);
	if (head == NULL) {
		return -ENOMEM;
	}
	memcpy(head, &klp, sizeof(klp));
	memcpy((char *)head + sizeof(klp), key, key_len);
	spdk_wmb();

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = opc;
	cmd.nsid = nsid;
	cmd.cdw10_bits.kv.vsize = size;		/* VALUE size only (not the key head) */
	cmd.cdw11_bits.kv.kl = 0;		/* in-payload long-key signal (ADR-0014) */
	if (opc == SPDK_NVME_OPC_KV_STORE) {
		cmd.cdw11_bits.kv.ro = ro;
		if (ro & SPDK_NVME_KV_STORE_OPT_TTL_VALID) {
			cmd.cdw12 = ttl;	/* KV Store TTL (vendor ext) */
		}
	}

	segs = nvfu_sgl_set_dptr_kv(&cmd, head_iova, head_len, buf_iova, size, &err);
	if (err != 0) {
		spdk_dma_free(head);
		return err;
	}

	rc = nvfu_submit_poll(d, &d->io, &cmd, out_cpl);
	if (segs != NULL) {
		spdk_dma_free(segs);
	}
	spdk_dma_free(head);
	return rc;
}

/* Shared attach + bring-to-ready + IO queue. Returns 0 on success. */
static inline int
nvfu_open(struct nvfu_dev *d, const char *traddr_dir)
{
	char cntrl_path[PATH_MAX];
	void *db;

	snprintf(cntrl_path, sizeof(cntrl_path), "%s/cntrl", traddr_dir);
	if (access(cntrl_path, F_OK) != 0) {
		fprintf(stderr, "cntrl socket not found: %s\n", cntrl_path);
		return -ENOENT;
	}
	d->dev = spdk_vfio_user_setup(cntrl_path);
	if (d->dev == NULL) {
		fprintf(stderr, "spdk_vfio_user_setup(%s) failed\n", cntrl_path);
		return -EIO;
	}
	printf("Attached to vfio-user controller at %s (no QEMU, no lib/nvme)\n",
	       cntrl_path);

	db = spdk_vfio_user_get_bar_addr(d->dev, 0, NVME_DB_OFFSET, 0x1000);
	if (!db) {
		fprintf(stderr, "failed to map BAR0 doorbell page\n");
		return -EIO;
	}
	d->doorbells = (volatile uint32_t *)db;

	if (nvfu_pci_enable_dma(d) != 0 || nvfu_controller_enable(d) != 0) {
		return -EIO;
	}
	printf("Controller ENABLED (CSTS.RDY=1) via our own admin queue.\n");

	if (nvfu_identify_controller(d) != 0 || nvfu_create_io_queue(d) != 0) {
		return -EIO;
	}
	return 0;
}

static inline void
nvfu_close(struct nvfu_dev *d)
{
	spdk_dma_free(d->admin.sq);
	spdk_dma_free(d->admin.cq);
	spdk_dma_free(d->io.sq);
	spdk_dma_free(d->io.cq);
	if (d->dev) {
		spdk_vfio_user_release(d->dev);
	}
}

#endif /* NKV_VFU_H */
