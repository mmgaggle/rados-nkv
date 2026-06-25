/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_internal/cunit.h"
#include "spdk/stdinc.h"
#include "common/lib/test_env.c"

#include "bdev_kvrados.c"

/*
 * Capture the completion of the bdev_io submitted by the module under test.
 * The module calls spdk_bdev_io_complete_nvme_status() from its handlers; we
 * record the status here so the tests can assert on it.
 */
static bool g_completed;
static uint32_t g_cpl_cdw0;
static int g_cpl_sct;
static int g_cpl_sc;

void
spdk_bdev_io_complete_nvme_status(struct spdk_bdev_io *bdev_io, uint32_t cdw0, int sct, int sc)
{
	g_completed = true;
	g_cpl_cdw0 = cdw0;
	g_cpl_sct = sct;
	g_cpl_sc = sc;
}

/*
 * spdk_bdev_io_complete carries the bdev_io status the abort path uses (SUCCESS =
 * aborted, FAILED = not aborted). A test installs g_ut_abort_complete to capture it.
 */
static void (*g_ut_abort_complete)(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status);

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	if (g_ut_abort_complete != NULL) {
		g_ut_abort_complete(bdev_io, status);
	}
}
DEFINE_STUB(spdk_bdev_register, int, (struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_unregister_by_name, int, (const char *bdev_name,
		struct spdk_bdev_module *module, spdk_bdev_unregister_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB_V(spdk_bdev_module_list_add, (struct spdk_bdev_module *bdev_module));
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), "kvrados0");
DEFINE_STUB_V(spdk_io_device_register, (void *io_device, spdk_io_channel_create_cb create_cb,
					spdk_io_channel_destroy_cb destroy_cb, uint32_t ctx_size,
					const char *name));
DEFINE_STUB_V(spdk_io_device_unregister, (void *io_device,
		spdk_io_device_unregister_cb unregister_cb));
DEFINE_STUB(spdk_get_io_channel, struct spdk_io_channel *, (void *io_device), NULL);

#ifdef NKVX_WITH_MERCURY
/* With -DNKVX_WITH_MERCURY, bdev_kvrados.c calls into the reused nkvx front bridge.
 * This UT exercises the forwarder in isolation, so stub the bridge entry points
 * (the real implementations live in module/kvdev/rados/kvdev_rados_nkvx_front.c
 * and are covered by their own na+sm loopback tests). */
DEFINE_STUB(kvdev_rados_nkvx_front_forward, int, (struct nkvx_front *front,
		const void *key, uint8_t key_len, uint32_t op_id, bool read_only,
		uint8_t runtime, const char *module_key, const char *module_ns,
		const uint8_t *sha256, bool sha256_valid, uint64_t caps,
		const void *input, uint32_t input_len, void *output_buf,
		uint32_t output_buf_len, spdk_kvdev_io_completion_cb cb_fn, void *cb_arg,
		uint64_t *out_token), 0);
DEFINE_STUB(kvdev_rados_nkvx_front_retrieve, int, (struct nkvx_front *front,
		const void *key, uint8_t key_len, bool read_only, void *output_buf,
		uint32_t output_buf_len, spdk_kvdev_io_completion_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(kvdev_rados_nkvx_front_store, int, (struct nkvx_front *front,
		const void *key, uint8_t key_len, bool read_only, uint8_t store_flags,
		const void *value, uint32_t value_len, spdk_kvdev_io_completion_cb cb_fn,
		void *cb_arg), 0);
DEFINE_STUB(kvdev_rados_nkvx_front_delete, int, (struct nkvx_front *front,
		const void *key, uint8_t key_len, bool read_only,
		spdk_kvdev_io_completion_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(kvdev_rados_nkvx_front_exist, int, (struct nkvx_front *front,
		const void *key, uint8_t key_len, bool read_only,
		spdk_kvdev_io_completion_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(kvdev_rados_nkvx_front_list, int, (struct nkvx_front *front,
		const void *start_key, uint8_t start_key_len, bool read_only,
		void *output_buf, uint32_t output_buf_len, spdk_kvdev_io_completion_cb cb_fn,
		void *cb_arg), 0);
DEFINE_STUB(kvdev_rados_nkvx_front_cancel, bool, (struct nkvx_front *front,
		uint64_t token), false);
/* The per-channel Mercury progress poller (SPDK_POLLER_REGISTER /
 * spdk_poller_unregister) is the only deps-enabled reference that would
 * otherwise pull real libspdk_thread.a into the link and collide with this UT's
 * own spdk_get_io_channel / spdk_io_device_* stubs. Stub the poller too so the
 * UT stays fully isolated; spdk_io_channel_get_ctx() is a header inline and is
 * intentionally left real (ut_make_channel() lays out its ctx accordingly). */
DEFINE_STUB(spdk_poller_register_named, struct spdk_poller *, (spdk_poller_fn fn,
		void *arg, uint64_t period_microseconds, const char *name), NULL);
DEFINE_STUB_V(spdk_poller_unregister, (struct spdk_poller **ppoller));
#endif /* NKVX_WITH_MERCURY */

/* Fake-channel helper (defined with the Exec tests below). The KV-I/O verbs also
 * fetch the channel ctx on their --with-mercury forward path, so they submit
 * through a real fake channel rather than a NULL one. */
static struct spdk_io_channel *ut_make_channel(struct kvrados_channel **kch_out);

static struct kvrados_disk *
ut_create_kvrados(uint32_t max_key, uint32_t max_value, uint32_t opt_gran, uint64_t num_keys)
{
	struct kvrados_disk *kvrados;

	kvrados = calloc(1, sizeof(*kvrados));
	SPDK_CU_ASSERT_FATAL(kvrados != NULL);

	kvrados->disk.name = strdup("kvrados0");
	kvrados->disk.ctxt = kvrados;
	kvrados->disk.nsid = 1;
	kvrados->disk.csi = SPDK_NVME_CSI_KV;
	kvrados->max_key_size = max_key;
	kvrados->max_value_size = max_value;
	kvrados->optimal_value_granularity = opt_gran;
	kvrados->num_keys = num_keys;

	return kvrados;
}

static void
ut_free_kvrados(struct kvrados_disk *kvrados)
{
	free(kvrados->disk.name);
	free(kvrados);
}

static void
ut_reset_cpl(void)
{
	g_completed = false;
	g_cpl_cdw0 = 0;
	g_cpl_sct = -1;
	g_cpl_sc = -1;
}

/*
 * Build an admin IDENTIFY bdev_io for the given CNS and submit it through the
 * module's submit_request. Admin passthru is contiguous, so .buf is set.
 */
static void
ut_submit_identify(struct kvrados_disk *kvrados, uint8_t cns, uint8_t csi,
		   uint32_t nsid, void *buf, uint32_t nbytes)
{
	struct spdk_bdev_io bdev_io = {};

	bdev_io.bdev = &kvrados->disk;
	bdev_io.type = SPDK_BDEV_IO_TYPE_NVME_ADMIN;
	bdev_io.u.nvme_passthru.cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	bdev_io.u.nvme_passthru.cmd.nsid = nsid;
	bdev_io.u.nvme_passthru.cmd.cdw10_bits.identify.cns = cns;
	bdev_io.u.nvme_passthru.cmd.cdw11_bits.identify.csi = csi;
	bdev_io.u.nvme_passthru.buf = buf;
	bdev_io.u.nvme_passthru.nbytes = nbytes;

	ut_reset_cpl();
	kvrados_submit_request(NULL, &bdev_io);
}

/* CNS 05h: KV namespace identify returns the create-time limits. */
static void
test_identify_ns_iocs(void)
{
	struct kvrados_disk *kvrados;
	struct spdk_nvme_kv_ns_data *nsdata;

	kvrados = ut_create_kvrados(16, 256 * 1024, 4096, 1000000);

	nsdata = calloc(1, sizeof(*nsdata));
	SPDK_CU_ASSERT_FATAL(nsdata != NULL);

	ut_submit_identify(kvrados, SPDK_NVME_IDENTIFY_NS_IOCS, SPDK_NVME_CSI_KV,
			   1, nsdata, sizeof(*nsdata));

	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT_EQUAL(nsdata->kvf[0].kvkml, 16);
	CU_ASSERT_EQUAL(nsdata->kvf[0].kvvml, 256 * 1024);
	CU_ASSERT_EQUAL(nsdata->novg, 4096);
	CU_ASSERT_EQUAL(nsdata->nsze, 1000000);

	free(nsdata);
	ut_free_kvrados(kvrados);
}

/* CNS 06h: KV controller identify returns the KV spec version. */
static void
test_identify_ctrlr_iocs(void)
{
	struct kvrados_disk *kvrados;
	struct spdk_nvme_kv_ctrlr_data *cdata;

	kvrados = ut_create_kvrados(16, 128 * 1024, 4096, 100);

	cdata = calloc(1, sizeof(*cdata));
	SPDK_CU_ASSERT_FATAL(cdata != NULL);

	ut_submit_identify(kvrados, SPDK_NVME_IDENTIFY_CTRLR_IOCS, SPDK_NVME_CSI_KV,
			   1, cdata, sizeof(*cdata));

	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT_EQUAL(cdata->ver, SPDK_NVME_KV_SPEC_VER);

	free(cdata);
	ut_free_kvrados(kvrados);
}

/* A non-KV CSI on the identify must be rejected as INVALID_FIELD. */
static void
test_identify_wrong_csi(void)
{
	struct kvrados_disk *kvrados;
	struct spdk_nvme_kv_ns_data *nsdata;

	kvrados = ut_create_kvrados(16, 128 * 1024, 4096, 1);

	nsdata = calloc(1, sizeof(*nsdata));
	SPDK_CU_ASSERT_FATAL(nsdata != NULL);

	ut_submit_identify(kvrados, SPDK_NVME_IDENTIFY_NS_IOCS, SPDK_NVME_CSI_NVM,
			   1, nsdata, sizeof(*nsdata));

	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_INVALID_FIELD);

	free(nsdata);
	ut_free_kvrados(kvrados);
}

/* A wrong nsid on the NS identify must be rejected. */
static void
test_identify_ns_wrong_nsid(void)
{
	struct kvrados_disk *kvrados;
	struct spdk_nvme_kv_ns_data *nsdata;

	kvrados = ut_create_kvrados(16, 128 * 1024, 4096, 1);

	nsdata = calloc(1, sizeof(*nsdata));
	SPDK_CU_ASSERT_FATAL(nsdata != NULL);

	ut_submit_identify(kvrados, SPDK_NVME_IDENTIFY_NS_IOCS, SPDK_NVME_CSI_KV,
			   2, nsdata, sizeof(*nsdata));

	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);

	free(nsdata);
	ut_free_kvrados(kvrados);
}

/* Lay out an ADR-0014 in-payload key [u16 le key_len][key] at the head of buf,
 * followed by `vlen` value bytes. Returns the total payload length. */
static uint32_t
ut_layout_inpayload(uint8_t *buf, const char *key, uint16_t key_len, uint32_t vlen)
{
	buf[0] = (uint8_t)(key_len & 0xff);
	buf[1] = (uint8_t)(key_len >> 8);
	memcpy(buf + 2, key, key_len);
	memset(buf + 2 + key_len, 0xAB, vlen);
	return 2u + key_len + vlen;
}

/*
 * ADR-0014 key parse: a short key from a single contiguous iov is parsed from the
 * in-payload [u16 key_len][key] header, NOT the CDW slots. value_off points past it.
 */
static void
test_inpayload_key_short(void)
{
	uint8_t buf[64];
	struct iovec iov;
	uint8_t key[KVRADOS_KEY_MAX_LEN];
	uint64_t value_off = 0;
	uint16_t klen;
	uint32_t payload;

	payload = ut_layout_inpayload(buf, "ABCD", 4, 16);
	iov.iov_base = buf;
	iov.iov_len = payload;

	klen = kvrados_parse_inpayload_key(&iov, 1, payload, key, &value_off);
	CU_ASSERT_EQUAL(klen, 4);
	CU_ASSERT(memcmp(key, "ABCD", 4) == 0);
	CU_ASSERT_EQUAL(value_off, 6);	/* 2 (hdr) + 4 (key) */
}

/* A long key (>16B, the ADR-0014 raison d'être) parses correctly, and the gather
 * spans multiple iovs (the header + key straddle the iov boundary). */
static void
test_inpayload_key_long_multi_iov(void)
{
	uint8_t buf[300];
	struct iovec iovs[3];
	uint8_t key[KVRADOS_KEY_MAX_LEN];
	uint64_t value_off = 0;
	uint16_t klen;
	uint32_t payload;
	const char *longkey = "thirty-two-byte-content-hash-XYZ";	/* 32 bytes */

	payload = ut_layout_inpayload(buf, longkey, 32, 64);
	/* Split into 3 iovs so the [u16][32B key] header straddles boundaries. */
	iovs[0].iov_base = buf;        iovs[0].iov_len = 3;	/* hdr + 1 key byte */
	iovs[1].iov_base = buf + 3;    iovs[1].iov_len = 40;	/* rest of key + value start */
	iovs[2].iov_base = buf + 43;   iovs[2].iov_len = payload - 43;

	klen = kvrados_parse_inpayload_key(iovs, 3, payload, key, &value_off);
	CU_ASSERT_EQUAL(klen, 32);
	CU_ASSERT(memcmp(key, longkey, 32) == 0);
	CU_ASSERT_EQUAL(value_off, 34);	/* 2 + 32 */
}

/* Bounds: key_len 0, key_len > 255 cap (encoded), and a payload too short to hold
 * the declared key are all rejected (klen 0). */
static void
test_inpayload_key_bounds(void)
{
	uint8_t buf[300];
	struct iovec iov;
	uint8_t key[KVRADOS_KEY_MAX_LEN];
	uint16_t klen;

	/* key_len == 0 */
	buf[0] = 0; buf[1] = 0;
	iov.iov_base = buf; iov.iov_len = 8;
	klen = kvrados_parse_inpayload_key(&iov, 1, 8, key, NULL);
	CU_ASSERT_EQUAL(klen, 0);

	/* key_len 300 (> 255 cap) */
	buf[0] = (uint8_t)(300 & 0xff); buf[1] = (uint8_t)(300 >> 8);
	iov.iov_len = sizeof(buf);
	klen = kvrados_parse_inpayload_key(&iov, 1, sizeof(buf), key, NULL);
	CU_ASSERT_EQUAL(klen, 0);

	/* declared key_len 100 but payload only 10 bytes -> short */
	buf[0] = 100; buf[1] = 0;
	iov.iov_len = 10;
	klen = kvrados_parse_inpayload_key(&iov, 1, 10, key, NULL);
	CU_ASSERT_EQUAL(klen, 0);

	/* payload shorter than the 2-byte header */
	iov.iov_len = 1;
	klen = kvrados_parse_inpayload_key(&iov, 1, 1, key, NULL);
	CU_ASSERT_EQUAL(klen, 0);
}

/* Stage an inline key into the command CDW slots EXACTLY as the host's
 * nvme_kv_cmd_set_key does (bytes 0..7 -> CDW2/CDW3, 8..15 -> CDW14/CDW15, length
 * -> CDW11.KL). This mirrors the rkv shim's nkvx_exist framing so the test proves
 * the host-side write and the forwarder-side read agree byte-for-byte. */
static void
ut_stage_cdw_key(struct spdk_nvme_cmd *cmd, const char *key, uint8_t key_len)
{
	memset(cmd, 0, sizeof(*cmd));
	cmd->cdw11_bits.kv.kl = key_len;
	memcpy((uint8_t *)&cmd->cdw2, key, spdk_min(key_len, (uint8_t)8));
	if (key_len > 8) {
		memcpy((uint8_t *)&cmd->cdw14, key + 8, spdk_min((uint8_t)(key_len - 8), (uint8_t)8));
	}
}

/*
 * Inline CDW key (the NO-DATA Exist/Delete path, bead spdk-qzm): the key rides the
 * command CDW slots (no DPTR is mapped for a DATA_NONE opcode), so the forwarder
 * reads it back from CDW2/3/14/15 + CDW11.KL. Verify a short key, a full 16-byte
 * key spanning all four CDWs byte-exactly, and the inline 1..16 length bounds.
 */
static void
test_cdw_key(void)
{
	struct spdk_nvme_cmd cmd;
	uint8_t key[KVRADOS_KEY_MAX_LEN];
	uint16_t klen;
	const char *k16 = "0123456789abcdef";	/* exactly 16 bytes: spans CDW2/3/14/15 */

	/* Short key (<= 8 bytes: lives wholly in CDW2/CDW3). */
	ut_stage_cdw_key(&cmd, "ABCD", 4);
	klen = kvrados_read_cdw_key(&cmd, key);
	CU_ASSERT_EQUAL(klen, 4);
	CU_ASSERT(memcmp(key, "ABCD", 4) == 0);

	/* 9-byte key: one byte spills into CDW14. */
	ut_stage_cdw_key(&cmd, "ABCDEFGHI", 9);
	klen = kvrados_read_cdw_key(&cmd, key);
	CU_ASSERT_EQUAL(klen, 9);
	CU_ASSERT(memcmp(key, "ABCDEFGHI", 9) == 0);

	/* Full 16-byte inline key: CDW2/3 hold 0..7, CDW14/15 hold 8..15. */
	ut_stage_cdw_key(&cmd, k16, 16);
	klen = kvrados_read_cdw_key(&cmd, key);
	CU_ASSERT_EQUAL(klen, 16);
	CU_ASSERT(memcmp(key, k16, 16) == 0);

	/* kl == 0 (no key) -> rejected. */
	memset(&cmd, 0, sizeof(cmd));
	cmd.cdw11_bits.kv.kl = 0;
	CU_ASSERT_EQUAL(kvrados_read_cdw_key(&cmd, key), 0);

	/* kl == 17 (> inline 16-byte max) -> rejected. */
	cmd.cdw11_bits.kv.kl = 17;
	CU_ASSERT_EQUAL(kvrados_read_cdw_key(&cmd, key), 0);
}

/*
 * Value-region sizing + scatter (bead spdk-kbh): the rkv vfio-user client splits a
 * value buffer into one region-bounded SGL block per 2 MiB DMA region, so the value
 * region spans MANY iovs. kvrados_value_region_len must sum ALL value iovs (so the
 * executor sees the true osize, not a single-region 2 MiB cap), and kvrados_scatter
 * must reassemble a value the executor PUSHed into a contiguous bounce back across
 * those region-bounded iovs byte-exact. Small region size here mirrors the 2 MiB
 * structure without large allocations.
 */
static void
test_value_region_len_and_scatter(void)
{
	enum { REGION = 8, NREG = 5 };			/* 5 region-bounded value blocks */
	uint64_t value_off = 6;				/* a 6-byte key head precedes the value */
	uint8_t head[6] = { 0 };
	uint8_t blk[NREG][REGION];
	struct iovec iovs[1 + NREG];
	uint8_t src[NREG * REGION];
	uint32_t total, n;
	int i, j;

	/* iov[0] = key head (its own region, before the value); iov[1..] = value blocks. */
	iovs[0].iov_base = head;
	iovs[0].iov_len = sizeof(head);
	for (i = 0; i < NREG; i++) {
		memset(blk[i], 0, REGION);
		iovs[1 + i].iov_base = blk[i];
		iovs[1 + i].iov_len = REGION;
	}

	/* (a) Full value-region length sums ALL value iovs, not just the first. */
	total = kvrados_value_region_len(iovs, 1 + NREG, value_off);
	CU_ASSERT_EQUAL(total, (uint32_t)(NREG * REGION));

	/* (b) Scatter a contiguous source (the bounce the executor PUSHed into) back
	 * across the region-bounded value iovs; every byte lands in the right block. */
	for (i = 0; i < (int)sizeof(src); i++) {
		src[i] = (uint8_t)(i + 1);		/* nonzero, position-encoded */
	}
	n = kvrados_scatter(iovs, 1 + NREG, value_off, src, total);
	CU_ASSERT_EQUAL(n, total);
	for (i = 0; i < NREG; i++) {
		for (j = 0; j < REGION; j++) {
			CU_ASSERT_EQUAL(blk[i][j], (uint8_t)(i * REGION + j + 1));
		}
	}
	/* The key head must be untouched by the value scatter. */
	for (i = 0; i < (int)sizeof(head); i++) {
		CU_ASSERT_EQUAL(head[i], 0);
	}

	/* (c) Single contiguous region (value fits in one iov): region_len == its span,
	 * so the handler keeps the zero-copy path (no bounce) — the <= 2 MiB case. */
	{
		uint8_t one[6 + 100];
		struct iovec siov;

		memset(one, 0, sizeof(one));
		siov.iov_base = one;
		siov.iov_len = sizeof(one);
		total = kvrados_value_region_len(&siov, 1, value_off);
		CU_ASSERT_EQUAL(total, 100u);
	}

	/* (d) Partial scatter: a delivered length shorter than the region capacity (a
	 * truncated / short value) writes only that many bytes and stops cleanly. */
	for (i = 0; i < NREG; i++) {
		memset(blk[i], 0xEE, REGION);
	}
	n = kvrados_scatter(iovs, 1 + NREG, value_off, src, REGION + 3);
	CU_ASSERT_EQUAL(n, (uint32_t)(REGION + 3));
	CU_ASSERT_EQUAL(blk[0][0], src[0]);
	CU_ASSERT_EQUAL(blk[1][2], src[REGION + 2]);
	CU_ASSERT_EQUAL(blk[1][3], 0xEE);		/* untouched past the delivered span */
}

/*
 * A KV I/O verb arrives as NVME_IOV_MD with .iovs/.iovcnt set and .buf NULL; the
 * forwarder reads the iovs (never .buf). RETRIEVE with a well-formed in-payload key
 * but no executor (the --without-mercury UT build) completes a clean device error,
 * NOT a crash on NULL .buf.
 */
static void
test_kv_retrieve_no_executor(void)
{
	struct kvrados_disk *kvrados;
	struct spdk_bdev_io bdev_io = {};
	uint8_t buf[4096];
	struct iovec iov;
	uint32_t payload;

	struct spdk_io_channel *ch = ut_make_channel(NULL);

	kvrados = ut_create_kvrados(255, 128 * 1024, 4096, 1);

	payload = ut_layout_inpayload(buf, "ABCD", 4, 64);
	iov.iov_base = buf;
	iov.iov_len = payload;

	bdev_io.bdev = &kvrados->disk;
	bdev_io.type = SPDK_BDEV_IO_TYPE_NVME_IOV_MD;
	bdev_io.u.nvme_passthru.cmd.opc = SPDK_NVME_OPC_KV_RETRIEVE;
	bdev_io.u.nvme_passthru.cmd.nsid = 1;
	bdev_io.u.nvme_passthru.iovs = &iov;
	bdev_io.u.nvme_passthru.iovcnt = 1;
	bdev_io.u.nvme_passthru.buf = NULL;	/* must not be dereferenced */

	ut_reset_cpl();
	kvrados_submit_request(ch, &bdev_io);

	CU_ASSERT(g_completed);
	/* Without --with-mercury the forwarder has no transport -> INVALID_OPCODE;
	 * with mercury but no endpoint (front == NULL) it is INTERNAL_DEVICE_ERROR.
	 * Either is a clean generic-status completion. */
	CU_ASSERT_EQUAL(g_cpl_sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(g_cpl_sc == SPDK_NVME_SC_INVALID_OPCODE ||
		  g_cpl_sc == SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);

	free(ch);
	ut_free_kvrados(kvrados);
}

/* RETRIEVE with a malformed in-payload key header is rejected INVALID_FIELD. */
static void
test_kv_retrieve_bad_key(void)
{
	struct kvrados_disk *kvrados;
	struct spdk_bdev_io bdev_io = {};
	uint8_t buf[16] = {0};	/* key_len == 0 */
	struct iovec iov;

	kvrados = ut_create_kvrados(255, 128 * 1024, 4096, 1);

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);

	bdev_io.bdev = &kvrados->disk;
	bdev_io.type = SPDK_BDEV_IO_TYPE_NVME_IOV_MD;
	bdev_io.u.nvme_passthru.cmd.opc = SPDK_NVME_OPC_KV_RETRIEVE;
	bdev_io.u.nvme_passthru.cmd.nsid = 1;
	bdev_io.u.nvme_passthru.iovs = &iov;
	bdev_io.u.nvme_passthru.iovcnt = 1;

	ut_reset_cpl();
	kvrados_submit_request(NULL, &bdev_io);

	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_INVALID_FIELD);

	ut_free_kvrados(kvrados);
}

/* An unknown KV opcode on the I/O path must be rejected as INVALID_OPCODE. */
static void
test_kv_io_bad_opcode(void)
{
	struct kvrados_disk *kvrados;
	struct spdk_bdev_io bdev_io = {};
	struct iovec iov;
	uint8_t buf[16];

	kvrados = ut_create_kvrados(16, 128 * 1024, 4096, 1);

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);

	bdev_io.bdev = &kvrados->disk;
	bdev_io.type = SPDK_BDEV_IO_TYPE_NVME_IOV_MD;
	bdev_io.u.nvme_passthru.cmd.opc = 0x55; /* not a KV opcode */
	bdev_io.u.nvme_passthru.cmd.nsid = 1;
	bdev_io.u.nvme_passthru.iovs = &iov;
	bdev_io.u.nvme_passthru.iovcnt = 1;

	ut_reset_cpl();
	kvrados_submit_request(NULL, &bdev_io);

	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_INVALID_OPCODE);

	ut_free_kvrados(kvrados);
}

/* Submit one KV I/O verb (NVME_IOV_MD) with the given opcode + a single iov.
 * Submits through a fake channel (front == NULL) so the --with-mercury forward
 * path resolves the channel ctx instead of dereferencing a NULL channel; with a
 * NULL front it completes the clean "no executor" device error. --without-mercury
 * the channel is inert (the forward block is compiled out). */
static void
ut_submit_kv_io(struct kvrados_disk *kvrados, uint8_t opc, struct iovec *iov, int iovcnt)
{
	struct spdk_bdev_io bdev_io = {};
	struct spdk_io_channel *ch = ut_make_channel(NULL);

	bdev_io.bdev = &kvrados->disk;
	bdev_io.type = SPDK_BDEV_IO_TYPE_NVME_IOV_MD;
	bdev_io.u.nvme_passthru.cmd.opc = opc;
	bdev_io.u.nvme_passthru.cmd.nsid = 1;
	bdev_io.u.nvme_passthru.iovs = iov;
	bdev_io.u.nvme_passthru.iovcnt = iovcnt;
	bdev_io.u.nvme_passthru.buf = NULL;

	ut_reset_cpl();
	kvrados_submit_request(ch, &bdev_io);
	free(ch);
}

/*
 * Option-C FRONT fast-path read-only gate (ADR-0008 D2/D5): on a read-only
 * namespace, STORE and DELETE are rejected with "Attempted Write to Read Only
 * Range" BEFORE any forward — at the tenant edge, regardless of executor config
 * (this is the advisory fast-path; the executor enforces authoritatively too).
 */
static void
test_kv_readonly_gate_store(void)
{
	struct kvrados_disk *kvrados;
	uint8_t buf[64];
	struct iovec iov;
	uint32_t payload;

	kvrados = ut_create_kvrados(255, 128 * 1024, 4096, 1);
	kvrados->read_only = true;

	payload = ut_layout_inpayload(buf, "ABCD", 4, 16);
	iov.iov_base = buf;
	iov.iov_len = payload;

	ut_submit_kv_io(kvrados, SPDK_NVME_OPC_KV_STORE, &iov, 1);

	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sct, SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE);

	ut_free_kvrados(kvrados);
}

static void
test_kv_readonly_gate_delete(void)
{
	struct kvrados_disk *kvrados;
	uint8_t buf[64];
	struct iovec iov;
	uint32_t payload;

	kvrados = ut_create_kvrados(255, 128 * 1024, 4096, 1);
	kvrados->read_only = true;

	payload = ut_layout_inpayload(buf, "ABCD", 4, 0);
	iov.iov_base = buf;
	iov.iov_len = payload;

	ut_submit_kv_io(kvrados, SPDK_NVME_OPC_KV_DELETE, &iov, 1);

	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sct, SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE);

	ut_free_kvrados(kvrados);
}

/*
 * The read-only gate is verb-based: non-mutating reads (RETRIEVE/EXIST/LIST) are
 * NEVER rejected by it on a read-only namespace. In the --without-mercury UT build
 * the verb then falls through to a clean INVALID_OPCODE (no transport) — the point
 * is that it is NOT the read-only rejection, proving the gate let the read pass.
 */
static void
test_kv_readonly_gate_allows_reads(void)
{
	struct kvrados_disk *kvrados;
	uint8_t buf[64];
	struct iovec iov;
	uint32_t payload;

	kvrados = ut_create_kvrados(255, 128 * 1024, 4096, 1);
	kvrados->read_only = true;

	payload = ut_layout_inpayload(buf, "ABCD", 4, 0);
	iov.iov_base = buf;
	iov.iov_len = payload;

	/* EXIST on a read-only ns: not gate-rejected. */
	ut_submit_kv_io(kvrados, SPDK_NVME_OPC_KV_EXIST, &iov, 1);
	CU_ASSERT(g_completed);
	CU_ASSERT(g_cpl_sc != SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE);

	/* LIST on a read-only ns: not gate-rejected. */
	ut_submit_kv_io(kvrados, SPDK_NVME_OPC_KV_LIST, &iov, 1);
	CU_ASSERT(g_completed);
	CU_ASSERT(g_cpl_sc != SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE);

	/* RETRIEVE on a read-only ns: not gate-rejected. */
	ut_submit_kv_io(kvrados, SPDK_NVME_OPC_KV_RETRIEVE, &iov, 1);
	CU_ASSERT(g_completed);
	CU_ASSERT(g_cpl_sc != SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE);

	ut_free_kvrados(kvrados);
}

/*
 * On a writable (non-read-only) namespace the gate does NOT fire for STORE/DELETE;
 * the verb proceeds to the forward (which, --without-mercury, is INVALID_OPCODE).
 */
static void
test_kv_writable_no_gate(void)
{
	struct kvrados_disk *kvrados;
	uint8_t buf[64];
	struct iovec iov;
	uint32_t payload;

	kvrados = ut_create_kvrados(255, 128 * 1024, 4096, 1);
	kvrados->read_only = false;

	payload = ut_layout_inpayload(buf, "ABCD", 4, 16);
	iov.iov_base = buf;
	iov.iov_len = payload;

	ut_submit_kv_io(kvrados, SPDK_NVME_OPC_KV_STORE, &iov, 1);
	CU_ASSERT(g_completed);
	CU_ASSERT(g_cpl_sc != SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE);

	ut_free_kvrados(kvrados);
}

/* STORE/DELETE/EXIST with a malformed (key_len 0) in-payload header are rejected
 * INVALID_FIELD before any forward. */
static void
test_kv_mutating_bad_key(void)
{
	struct kvrados_disk *kvrados;
	uint8_t buf[16] = {0};	/* key_len == 0 */
	struct iovec iov;

	kvrados = ut_create_kvrados(255, 128 * 1024, 4096, 1);
	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);

	ut_submit_kv_io(kvrados, SPDK_NVME_OPC_KV_STORE, &iov, 1);
	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_INVALID_FIELD);

	ut_submit_kv_io(kvrados, SPDK_NVME_OPC_KV_DELETE, &iov, 1);
	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_INVALID_FIELD);

	ut_submit_kv_io(kvrados, SPDK_NVME_OPC_KV_EXIST, &iov, 1);
	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_INVALID_FIELD);

	ut_free_kvrados(kvrados);
}

/* io_type_supported must accept the passthru types + ABORT and reject block ops. */
static void
test_io_type_supported(void)
{
	CU_ASSERT(kvrados_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_NVME_IO));
	CU_ASSERT(kvrados_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_NVME_IOV_MD));
	CU_ASSERT(kvrados_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_NVME_ADMIN));
	CU_ASSERT(kvrados_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_ABORT));
	CU_ASSERT_FALSE(kvrados_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_READ));
	CU_ASSERT_FALSE(kvrados_io_type_supported(NULL, SPDK_BDEV_IO_TYPE_WRITE));
}

/* ---- KV Exec (0x83): allowlist, native built-in, abort (S5a) ----------------- */

/*
 * A fake io_channel whose context buffer (at SPDK_IO_CHANNEL_STRUCT_SIZE) holds a
 * struct kvrados_channel. The Exec/abort paths derive the channel ctx via
 * spdk_io_channel_get_ctx(ch). exec_inflight is initialized; front is absent (the
 * UT builds --without-mercury, so kch->front does not exist as a field).
 */
static struct spdk_io_channel *
ut_make_channel(struct kvrados_channel **kch_out)
{
	uint8_t *buf = calloc(1, SPDK_IO_CHANNEL_STRUCT_SIZE + sizeof(struct kvrados_channel));
	struct kvrados_channel *kch;

	SPDK_CU_ASSERT_FATAL(buf != NULL);
	kch = (struct kvrados_channel *)(buf + SPDK_IO_CHANNEL_STRUCT_SIZE);
	TAILQ_INIT(&kch->exec_inflight);
	if (kch_out != NULL) {
		*kch_out = kch;
	}
	return (struct spdk_io_channel *)buf;
}

/* Build + submit a KV Exec (0x83): in-payload key, op_id in CDW13, osize in CDW12. */
static void
ut_submit_exec(struct kvrados_disk *kvrados, struct spdk_io_channel *ch,
	       uint32_t op_id, uint32_t osize, struct iovec *iov, int iovcnt)
{
	struct spdk_bdev_io bdev_io = {};

	bdev_io.bdev = &kvrados->disk;
	bdev_io.type = SPDK_BDEV_IO_TYPE_NVME_IOV_MD;
	bdev_io.u.nvme_passthru.cmd.opc = SPDK_NVME_OPC_KV_EXEC;
	bdev_io.u.nvme_passthru.cmd.nsid = 1;
	/* Decode is from the RAW dwords (spdk_kv_exec_decode): CDW12 = osize, CDW13 =
	 * op_id. We set the raw uint32 dwords, not cmd.cdw1x_bits.kv_exec.* (those
	 * union members are our vendor additions, absent in unmodified SPDK). */
	bdev_io.u.nvme_passthru.cmd.cdw12 = osize;
	bdev_io.u.nvme_passthru.cmd.cdw13 = op_id;
	bdev_io.u.nvme_passthru.iovs = iov;
	bdev_io.u.nvme_passthru.iovcnt = iovcnt;
	bdev_io.u.nvme_passthru.buf = NULL;

	ut_reset_cpl();
	kvrados_submit_request(ch, &bdev_io);
}

/*
 * Install a one-entry native-built-in allowlist on the disk, using S7's string
 * binding form: runtime "nkvx", module_namespace "nkvx", module_key <module>.
 */
static void
ut_set_native_allow(struct kvrados_disk *kvrados, uint32_t op_id, const char *module)
{
	struct kvrados_exec_binding *b = calloc(1, sizeof(*b));

	SPDK_CU_ASSERT_FATAL(b != NULL);
	b->op_id = op_id;
	b->runtime = strdup("nkvx");
	b->module_namespace = strdup("nkvx");
	b->module_key = strdup(module);
	b->caps = 0;
	kvrados->exec_allowlist = b;
	kvrados->exec_allowlist_count = 1;
}

static void
ut_free_allow(struct kvrados_disk *kvrados)
{
	kvrados_free_exec_allowlist(kvrados->exec_allowlist, kvrados->exec_allowlist_count);
	kvrados->exec_allowlist = NULL;
	kvrados->exec_allowlist_count = 0;
}

/*
 * Exec deny-by-default: an op_id absent from the (empty) allowlist is rejected with
 * Invalid Opcode BEFORE any executor runs (ADR-0008 D4).
 */
static void
test_exec_allowlist_deny(void)
{
	struct kvrados_disk *kvrados;
	struct kvrados_channel *kch;
	struct spdk_io_channel *ch;
	uint8_t buf[64];
	struct iovec iov;
	uint32_t payload;

	kvrados = ut_create_kvrados(255, 128 * 1024, 4096, 1);
	ch = ut_make_channel(&kch);

	payload = ut_layout_inpayload(buf, "ABCD", 4, 4);
	iov.iov_base = buf;
	iov.iov_len = payload;

	ut_submit_exec(kvrados, ch, 7, 64, &iov, 1);	/* empty allowlist -> op_id 7 denied */

	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_INVALID_OPCODE);
	CU_ASSERT(TAILQ_EMPTY(&kch->exec_inflight));

	free(ch);
	ut_free_kvrados(kvrados);
}

/*
 * Native built-in Exec round-trip with NO Ceph and NO Mercury: "inputlen" returns
 * the input length as an 8-byte LE value in the output sink and SUCCESS, DW0 = 8.
 */
static void
test_exec_native_inputlen(void)
{
	struct kvrados_disk *kvrados;
	struct kvrados_channel *kch;
	struct spdk_io_channel *ch;
	uint8_t buf[256];
	struct iovec iov;
	uint32_t payload;
	uint64_t result;
	uint64_t value_off = 2u + 4u;

	kvrados = ut_create_kvrados(255, 128 * 1024, 4096, 1);
	ut_set_native_allow(kvrados, 1, "inputlen");
	ch = ut_make_channel(&kch);

	payload = ut_layout_inpayload(buf, "ABCD", 4, 40);	/* 40 bytes of input */
	iov.iov_base = buf;
	iov.iov_len = payload;

	ut_submit_exec(kvrados, ch, 1, 64, &iov, 1);

	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sct, SPDK_NVME_SCT_GENERIC);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT_EQUAL(g_cpl_cdw0, 8);	/* TRUE result length = sizeof(uint64) */

	memcpy(&result, buf + value_off, sizeof(result));
	CU_ASSERT_EQUAL(result, 40);
	CU_ASSERT(TAILQ_EMPTY(&kch->exec_inflight));	/* synchronous: nothing in flight */

	free(ch);
	ut_free_allow(kvrados);
	ut_free_kvrados(kvrados);
}

/* Native "inputecho" copies the input bytes back into the output sink. */
static void
test_exec_native_inputecho(void)
{
	struct kvrados_disk *kvrados;
	struct kvrados_channel *kch;
	struct spdk_io_channel *ch;
	uint8_t buf[256];
	struct iovec iov;
	uint32_t payload;
	uint64_t value_off = 2u + 4u;

	kvrados = ut_create_kvrados(255, 128 * 1024, 4096, 1);
	ut_set_native_allow(kvrados, 2, "inputecho");
	ch = ut_make_channel(&kch);

	payload = ut_layout_inpayload(buf, "ABCD", 4, 16);	/* input = 16 * 0xAB */
	iov.iov_base = buf;
	iov.iov_len = payload;

	ut_submit_exec(kvrados, ch, 2, 128, &iov, 1);

	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_SUCCESS);
	CU_ASSERT_EQUAL(g_cpl_cdw0, 16);
	CU_ASSERT_EQUAL(buf[value_off], 0xAB);

	free(ch);
	ut_free_allow(kvrados);
	ut_free_kvrados(kvrados);
}

/*
 * Native built-in truncation: osize smaller than the 8-byte inputlen result reports
 * CAPACITY_EXCEEDED (BUFFER_TOO_SMALL) with the TRUE length in DW0.
 */
static void
test_exec_native_truncation(void)
{
	struct kvrados_disk *kvrados;
	struct kvrados_channel *kch;
	struct spdk_io_channel *ch;
	uint8_t buf[256];
	struct iovec iov;
	uint32_t payload;

	kvrados = ut_create_kvrados(255, 128 * 1024, 4096, 1);
	ut_set_native_allow(kvrados, 1, "inputlen");
	ch = ut_make_channel(&kch);

	payload = ut_layout_inpayload(buf, "ABCD", 4, 40);
	iov.iov_base = buf;
	iov.iov_len = payload;

	ut_submit_exec(kvrados, ch, 1, 4, &iov, 1);	/* osize 4 < result 8 -> truncated */

	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sct, SPDK_NVME_SCT_COMMAND_SPECIFIC);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_CAPACITY_EXCEEDED);
	CU_ASSERT_EQUAL(g_cpl_cdw0, 8);

	free(ch);
	ut_free_allow(kvrados);
	ut_free_kvrados(kvrados);
}

/*
 * An allowed op whose module is NOT an input-only native built-in (e.g. rados-backed
 * "bytecount") needs the standalone executor. --without-mercury that is NOT_SUPPORTED
 * -> Invalid Opcode, and nothing is left in flight.
 */
static void
test_exec_native_needs_executor(void)
{
	struct kvrados_disk *kvrados;
	struct kvrados_channel *kch;
	struct spdk_io_channel *ch;
	uint8_t buf[64];
	struct iovec iov;
	uint32_t payload;

	kvrados = ut_create_kvrados(255, 128 * 1024, 4096, 1);
	ut_set_native_allow(kvrados, 5, "bytecount");	/* needs the stored object */
	ch = ut_make_channel(&kch);

	payload = ut_layout_inpayload(buf, "ABCD", 4, 4);
	iov.iov_base = buf;
	iov.iov_len = payload;

	ut_submit_exec(kvrados, ch, 5, 64, &iov, 1);

	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_INVALID_OPCODE);	/* NOT_SUPPORTED */
	CU_ASSERT(TAILQ_EMPTY(&kch->exec_inflight));

	free(ch);
	ut_free_allow(kvrados);
	ut_free_kvrados(kvrados);
}

/* An out-of-range caps tier is rejected INVALID_FIELD before dispatch (ADR-0014). */
static void
test_exec_bad_caps_tier(void)
{
	struct kvrados_disk *kvrados;
	struct kvrados_channel *kch;
	struct spdk_io_channel *ch;
	uint8_t buf[64];
	struct iovec iov;
	uint32_t payload;

	kvrados = ut_create_kvrados(255, 128 * 1024, 4096, 1);
	ut_set_native_allow(kvrados, 1, "inputlen");
	kvrados->exec_allowlist[0].caps = SPDK_KV_EXEC_CAPS_TIER_MAX + 1;	/* invalid tier */
	ch = ut_make_channel(&kch);

	payload = ut_layout_inpayload(buf, "ABCD", 4, 4);
	iov.iov_base = buf;
	iov.iov_len = payload;

	ut_submit_exec(kvrados, ch, 1, 64, &iov, 1);

	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_INVALID_FIELD);

	free(ch);
	ut_free_allow(kvrados);
	ut_free_kvrados(kvrados);
}

/* A read-only native built-in (inputlen) runs even on a read-only namespace
 * (ADR-0008 D5: read-only modules run; only write-capable ones are rejected). */
static void
test_exec_readonly_native_runs(void)
{
	struct kvrados_disk *kvrados;
	struct kvrados_channel *kch;
	struct spdk_io_channel *ch;
	uint8_t buf[64];
	struct iovec iov;
	uint32_t payload;

	kvrados = ut_create_kvrados(255, 128 * 1024, 4096, 1);
	kvrados->read_only = true;
	ut_set_native_allow(kvrados, 1, "inputlen");
	ch = ut_make_channel(&kch);

	payload = ut_layout_inpayload(buf, "ABCD", 4, 8);
	iov.iov_base = buf;
	iov.iov_len = payload;

	ut_submit_exec(kvrados, ch, 1, 64, &iov, 1);

	CU_ASSERT(g_completed);
	CU_ASSERT_EQUAL(g_cpl_sc, SPDK_NVME_SC_SUCCESS);	/* not RO-rejected */

	free(ch);
	ut_free_allow(kvrados);
	ut_free_kvrados(kvrados);
}

/*
 * ABORT bookkeeping. An ABORT targeting a bdev_io NOT on the channel's exec_inflight
 * list is reported "not aborted" (bdev_io completes FAILED). An ABORT targeting an
 * entry that IS in flight finds it; --without-mercury there is no transport so the
 * cancel cannot be entered (reported not-aborted), but the lookup path is exercised.
 */
static int g_abort_status = -1;

static void
ut_capture_abort_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	g_abort_status = (int)status;
}

static void
test_exec_abort_not_found(void)
{
	struct kvrados_channel *kch;
	struct spdk_io_channel *ch = ut_make_channel(&kch);
	struct spdk_bdev_io target = {};
	struct spdk_bdev_io abort_io = {};

	abort_io.u.abort.bio_to_abort = &target;	/* not on the list */

	g_abort_status = -1;
	g_ut_abort_complete = ut_capture_abort_complete;
	kvrados_handle_abort(kch, &abort_io);
	g_ut_abort_complete = NULL;

	CU_ASSERT_EQUAL(g_abort_status, SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(TAILQ_EMPTY(&kch->exec_inflight));

	free(ch);
}

static void
test_exec_abort_found_no_transport(void)
{
	struct kvrados_channel *kch;
	struct spdk_io_channel *ch = ut_make_channel(&kch);
	struct spdk_bdev_io target = {};
	struct spdk_bdev_io abort_io = {};
	struct kvrados_kv_io_ctx kctx = {};

	kctx.bdev_io = &target;		/* a synthetic in-flight Exec for `target` */
	kctx.is_exec = true;
	kctx.kch = kch;
	kctx.cancel_token = 0;
	TAILQ_INSERT_TAIL(&kch->exec_inflight, &kctx, exec_link);

	abort_io.u.abort.bio_to_abort = &target;

	g_abort_status = -1;
	g_ut_abort_complete = ut_capture_abort_complete;
	kvrados_handle_abort(kch, &abort_io);
	g_ut_abort_complete = NULL;

	/* Found, but --without-mercury there is no transport -> not aborted. The
	 * original Exec entry is left in place to complete via its own path. */
	CU_ASSERT_EQUAL(g_abort_status, SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(!TAILQ_EMPTY(&kch->exec_inflight));

	TAILQ_REMOVE(&kch->exec_inflight, &kctx, exec_link);
	free(ch);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("bdev_kvrados", NULL, NULL);

	CU_ADD_TEST(suite, test_identify_ns_iocs);
	CU_ADD_TEST(suite, test_identify_ctrlr_iocs);
	CU_ADD_TEST(suite, test_identify_wrong_csi);
	CU_ADD_TEST(suite, test_identify_ns_wrong_nsid);
	CU_ADD_TEST(suite, test_inpayload_key_short);
	CU_ADD_TEST(suite, test_inpayload_key_long_multi_iov);
	CU_ADD_TEST(suite, test_inpayload_key_bounds);
	CU_ADD_TEST(suite, test_value_region_len_and_scatter);
	CU_ADD_TEST(suite, test_cdw_key);
	CU_ADD_TEST(suite, test_kv_retrieve_no_executor);
	CU_ADD_TEST(suite, test_kv_retrieve_bad_key);
	CU_ADD_TEST(suite, test_kv_io_bad_opcode);
	CU_ADD_TEST(suite, test_kv_readonly_gate_store);
	CU_ADD_TEST(suite, test_kv_readonly_gate_delete);
	CU_ADD_TEST(suite, test_kv_readonly_gate_allows_reads);
	CU_ADD_TEST(suite, test_kv_writable_no_gate);
	CU_ADD_TEST(suite, test_kv_mutating_bad_key);
	CU_ADD_TEST(suite, test_io_type_supported);
	CU_ADD_TEST(suite, test_exec_allowlist_deny);
	CU_ADD_TEST(suite, test_exec_native_inputlen);
	CU_ADD_TEST(suite, test_exec_native_inputecho);
	CU_ADD_TEST(suite, test_exec_native_truncation);
	CU_ADD_TEST(suite, test_exec_native_needs_executor);
	CU_ADD_TEST(suite, test_exec_bad_caps_tier);
	CU_ADD_TEST(suite, test_exec_readonly_native_runs);
	CU_ADD_TEST(suite, test_exec_abort_not_found);
	CU_ADD_TEST(suite, test_exec_abort_found_no_transport);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	return num_failures;
}
