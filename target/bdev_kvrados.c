/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "bdev_kvrados.h"

#include "spdk/stdinc.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/nvme_spec.h"
#include "spdk/bdev_module.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/kvdev.h"			/* enum spdk_kvdev_io_status, spdk_kv_exec_runtime, caps tier */

/*
 * NKVX_WITH_MERCURY gates the Mercury (inter-tier RPC) front-bridge code paths.
 * In the out-of-tree build, SPDK is UNMODIFIED and has no Mercury support, so WE
 * own this knob (formerly SPDK's generated SPDK_CONFIG_* mercury macro): it is
 * defined by passing -DNKVX_WITH_MERCURY from target/Makefile (and the umbrella
 * rados-nkvx/Makefile), not from SPDK's spdk/config.h. A build without the macro
 * is byte-identical to a stock (Mercury-less) forwarder.
 */
#ifdef NKVX_WITH_MERCURY
#include "kvdev_rados_nkvx_front.h"	/* the front Mercury bridge (reused from kvdev_rados) */
#include "nkvx_exec_rpc.h"		/* NKVX_INLINE_MAX (small/large value threshold) */
#endif

/* Default KV parameter values. */
#define KVRADOS_DEFAULT_MAX_KEY_SIZE			16
#define KVRADOS_DEFAULT_MAX_VALUE_SIZE			(128 * 1024)
#define KVRADOS_DEFAULT_OPTIMAL_VALUE_GRANULARITY	4096

/*
 * ADR-0014 in-payload key encoding: keys ride at the head of the DPTR/SGL payload
 * as [u16 key_len][key bytes][value...], NOT in the inline CDW2/3/14/15 slots. The
 * bdev parses the key from the host iovs (the forwarder owns gather/scatter of the
 * passthru SGL — S0). Max key length is 255 (SPDK_KVDEV_EXEC_KEY_MAX_LEN); the u16
 * length prefix is 2 bytes.
 */
#define KVRADOS_KEY_HDR_LEN				2u	/* sizeof(u16 key_len) */
#define KVRADOS_KEY_MAX_LEN				255u	/* == SPDK_KVDEV_EXEC_KEY_MAX_LEN */

struct kvrados_disk {
	struct spdk_bdev		disk;

	uint32_t			max_key_size;
	uint32_t			max_value_size;
	uint32_t			optimal_value_granularity;
	uint64_t			num_keys;

	char				*executor_endpoint;

	/* Per-namespace read-only bit (ADR-0008). Carried on the wire to the executor
	 * (which enforces it authoritatively); the bdev-side verb gate is S4. */
	bool				read_only;

	/* KV Exec allowlist/bindings (ADR-0005/0012/0014), copied from create opts.
	 * The Exec slice (S5a/later) consults these to gate/route KV Exec (0x83). */
	struct kvrados_exec_binding	*exec_allowlist;
	size_t				exec_allowlist_count;

	TAILQ_ENTRY(kvrados_disk)	link;
};

struct kvrados_channel {
	/*
	 * In-flight KV Exec commands on this channel, for tenant NVMe ABORT (S5a).
	 * Exec is the only long-running abortable KV op; each in-flight Exec registers
	 * an entry at submit and removes it at completion so an ABORT can find it and
	 * request a cancel. The original Exec still completes exactly once (ABORTED)
	 * from its own done-cb.
	 */
	TAILQ_HEAD(, kvrados_kv_io_ctx)	exec_inflight;
#ifdef NKVX_WITH_MERCURY
	/*
	 * Per-channel front Mercury client + non-blocking progress poller (reused from
	 * the kvdev_rados two-tier front, design §4.2). A struct nkvx_front is single-
	 * threaded, so one per reactor/channel. NULL when no executor_endpoint is
	 * configured (the bdev then runs the in-process native built-in executor only).
	 */
	struct nkvx_front		*front;
	struct spdk_poller		*front_poller;
#endif
};

/*
 * Per-KV-io forward context: completes the bdev_io from the bridge / native
 * executor done-cb. For Exec it also links onto the channel's exec_inflight list
 * (for ABORT) and retains the front cancel token + a multi-iov input bounce.
 */
struct kvrados_kv_io_ctx {
	struct spdk_bdev_io		*bdev_io;

	/* Exec-only abort plumbing (S5a). is_exec gates the inflight-list membership. */
	bool				is_exec;
	struct kvrados_channel		*kch;
	uint64_t			cancel_token;	/* KVDEV_RADOS_NKVX_TOKEN_NONE if none */
	/*
	 * A multi-segment Exec input OR Store value gathered into a temp. The forward may
	 * register it as a bulk PULL source (large input / large value), so it MUST outlive
	 * the in-flight forward — freed at completion (kvrados_exec_complete for Exec,
	 * kvrados_retrieve_done for Store), not right after submit. NULL on the zero-copy
	 * path (the value/input is read straight from the bdev_io DMA region) and for the
	 * read verbs.
	 */
	void				*input_bounce;
	TAILQ_ENTRY(kvrados_kv_io_ctx)	exec_link;
};

static TAILQ_HEAD(, kvrados_disk) g_kvrados_disks = TAILQ_HEAD_INITIALIZER(g_kvrados_disks);
static int g_kvrados_disk_count = 0;

static int bdev_kvrados_init(void);
static void bdev_kvrados_fini(void);

static struct spdk_bdev_module kvrados_if = {
	.name = "kvrados",
	.module_init = bdev_kvrados_init,
	.module_fini = bdev_kvrados_fini,
};

SPDK_BDEV_MODULE_REGISTER(kvrados, &kvrados_if)

/*
 * Gather up to `n` bytes starting at byte offset `off` from the host iovs into
 * `dst`. Returns the number of bytes actually copied (< n iff the SGL is shorter
 * than off+n). This is the only gather the forwarder needs: the small ADR-0014 key
 * header at the head of the payload — NOT the whole value (the value rides the SGL
 * verbatim to the executor as the result sink, zero-copy per S0).
 */
static uint32_t
kvrados_gather(const struct iovec *iovs, int iovcnt, uint64_t off, void *dst, uint32_t n)
{
	uint8_t *out = dst;
	uint32_t copied = 0;
	uint64_t pos = 0;
	int i;

	for (i = 0; i < iovcnt && copied < n; i++) {
		uint64_t seg = iovs[i].iov_len;

		if (off >= pos + seg) {
			pos += seg;
			continue;	/* this segment is entirely before off */
		}
		{
			uint64_t seg_off = (off > pos) ? (off - pos) : 0;
			uint64_t avail = seg - seg_off;
			uint32_t take = (uint32_t)spdk_min(avail, (uint64_t)(n - copied));

			memcpy(out + copied, (const uint8_t *)iovs[i].iov_base + seg_off, take);
			copied += take;
			pos += seg;
			off = pos;	/* subsequent segments start at their head */
		}
	}
	return copied;
}

/*
 * Parse the ADR-0014 in-payload key: [u16 key_len][key bytes] at the head of the
 * host SGL (CDW key slots are unused for our command set — migration §3). The u16
 * is little-endian on the wire. Bounds-checked: key_len must be 1..255 and the SGL
 * must actually carry the header + key bytes. On success returns the key length and
 * fills key[] (caller-sized to KVRADOS_KEY_MAX_LEN) and *value_off (the byte offset
 * of the value that follows). Returns 0 on a malformed/short payload.
 */
static uint16_t
kvrados_parse_inpayload_key(const struct iovec *iovs, int iovcnt, uint64_t payload_len,
			    uint8_t key[KVRADOS_KEY_MAX_LEN], uint64_t *value_off)
{
	uint8_t hdr[KVRADOS_KEY_HDR_LEN];
	uint16_t key_len;

	if (payload_len < KVRADOS_KEY_HDR_LEN) {
		return 0;
	}
	if (kvrados_gather(iovs, iovcnt, 0, hdr, KVRADOS_KEY_HDR_LEN) != KVRADOS_KEY_HDR_LEN) {
		return 0;
	}
	key_len = from_le16(hdr);
	if (key_len == 0 || key_len > KVRADOS_KEY_MAX_LEN) {
		return 0;
	}
	if (payload_len < (uint64_t)KVRADOS_KEY_HDR_LEN + key_len) {
		return 0;	/* SGL too short to hold the declared key */
	}
	if (kvrados_gather(iovs, iovcnt, KVRADOS_KEY_HDR_LEN, key, key_len) != key_len) {
		return 0;
	}
	if (value_off != NULL) {
		*value_off = (uint64_t)KVRADOS_KEY_HDR_LEN + key_len;
	}
	return key_len;
}

/*
 * Map an executor kvdev status to an NVMe (SCT, SC) pair for the tenant CQE. The
 * KV command set reuses the standard generic/command-specific status codes the
 * tenant edge expects (mirrors the kvdev_rados completion mapping). Shared by the
 * Mercury forward completions and the in-process native Exec executor (S5a).
 */
static void
kvrados_kvdev_status_to_nvme(enum spdk_kvdev_io_status status, int *sct, int *sc)
{
	switch (status) {
	case SPDK_KVDEV_IO_STATUS_SUCCESS:
		*sct = SPDK_NVME_SCT_GENERIC; *sc = SPDK_NVME_SC_SUCCESS;
		break;
	case SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST:
		/* KEY_DOES_NOT_EXIST (command-specific in the KV command set). Also the
		 * SIKE-conflict status (Store-If-Key-Exists on an absent key). */
		*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		*sc = SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST;
		break;
	case SPDK_KVDEV_IO_STATUS_KEY_EXIST:
		/* SINKE conflict (Store-If-No-Key-Exists on a present key): KV Key Exists
		 * (0x89, command-specific). */
		*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		*sc = SPDK_NVME_SC_KEY_EXISTS;
		break;
	case SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL:
		/* Truncation: the value did not fit osize. DW0 carries the true length so
		 * the host can resize and re-issue (ADR-0014 Retrieve-style truncation).
		 * CAPACITY_EXCEEDED (0x81) is in the command-specific status block. */
		*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		*sc = SPDK_NVME_SC_CAPACITY_EXCEEDED;
		break;
	case SPDK_KVDEV_IO_STATUS_INVALID:
		*sct = SPDK_NVME_SCT_GENERIC; *sc = SPDK_NVME_SC_INVALID_FIELD;
		break;
	case SPDK_KVDEV_IO_STATUS_NOMEM:
		*sct = SPDK_NVME_SCT_GENERIC; *sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		break;
	case SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED:
		*sct = SPDK_NVME_SCT_GENERIC; *sc = SPDK_NVME_SC_INVALID_OPCODE;
		break;
	case SPDK_KVDEV_IO_STATUS_READ_ONLY:
		/* "Attempted Write to Read Only Range" (0x82, command-specific). The
		 * verb-based gate is S4; carried here so the mapping is complete. */
		*sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		*sc = SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE;
		break;
	case SPDK_KVDEV_IO_STATUS_ABORTED:
		*sct = SPDK_NVME_SCT_GENERIC; *sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
		break;
	case SPDK_KVDEV_IO_STATUS_FAILED:
	default:
		*sct = SPDK_NVME_SCT_GENERIC; *sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		break;
	}
}

#ifdef NKVX_WITH_MERCURY
/*
 * Decode the STORE conditional options from CDW11 Request Options (ro) into
 * enum spdk_kvdev_store_flags (mirrors lib/nvmf/ctrlr_kvdev.c): the spec's
 * "Don't store if key does NOT exist" is Store-If-Key-Exists (SIKE); "Don't store
 * if key DOES exist" is Store-If-No-Key-Exists (SINKE). TTL/Ephemeral/Touch are
 * not carried on the inter-tier KV wire yet (a later slice).
 */
static uint8_t
kvrados_decode_store_flags(const struct spdk_nvme_cmd *cmd)
{
	uint8_t flags = 0;

	if (cmd->cdw11_bits.kv.ro & SPDK_NVME_KV_STORE_OPT_DONT_STORE_IF_KEY_NOT_EXISTS) {
		flags |= SPDK_KVDEV_STORE_FLAG_SIKE;
	}
	if (cmd->cdw11_bits.kv.ro & SPDK_NVME_KV_STORE_OPT_DONT_STORE_IF_KEY_EXISTS) {
		flags |= SPDK_KVDEV_STORE_FLAG_SINKE;
	}
	return flags;
}

/*
 * RETRIEVE/STORE/DELETE/EXIST/LIST forward completion (fires from the front
 * progress poller on the reactor thread). For verbs that return a body
 * (Retrieve/List) it has already landed in the request iovs — either PUSHed by the
 * executor (large) or copied into iov[0] by the bridge done trampoline (small).
 * Set CQE DW0 to the TRUE value/listing length (0 for Store/Delete; the stored
 * value length for Exist) and complete the io with the mapped NVMe status.
 */
static void
kvrados_retrieve_done(void *cb_arg, int status, uint32_t value_len)
{
	struct kvrados_kv_io_ctx * kctx = cb_arg;
	struct spdk_bdev_io *bdev_io = kctx->bdev_io;
	int sct, sc;

	kvrados_kvdev_status_to_nvme((enum spdk_kvdev_io_status)status, &sct, &sc);

	/* CQE DW0 = full value length (truncation semantics, ADR-0014): the cdw0 arg is
	 * surfaced by nvmf via spdk_bdev_io_get_nvme_status. */
	spdk_bdev_io_complete_nvme_status(bdev_io, value_len, sct, sc);
	/* Free a large-Store value bounce (registered as the value_bulk PULL source, so it
	 * had to outlive the in-flight forward). NULL for zero-copy stores and read verbs. */
	free(kctx->input_bounce);
	free(kctx);
}
#endif /* NKVX_WITH_MERCURY */

/* ---- KV Exec (0x83): allowlist enforcement + native executor + abort (S5a) -- */

/*
 * Per-namespace KV Exec op-ID allowlist lookup (ADR-0008 D4, deny-by-default).
 * Returns the matching S7 binding entry (carrying the structured/legacy binding)
 * or NULL when the op_id is not allowed for this namespace. The base read/write
 * verbs do NOT pass through this gate (D4: the allowlist is scoped to Exec); only
 * opcode 0x83 does.
 */
static const struct kvrados_exec_binding *
kvrados_exec_op_allowed(const struct kvrados_disk *kvrados, uint32_t op_id)
{
	size_t i;

	for (i = 0; i < kvrados->exec_allowlist_count; i++) {
		if (kvrados->exec_allowlist[i].op_id == op_id) {
			return &kvrados->exec_allowlist[i];
		}
	}
	return NULL;
}

/*
 * The structured binding the forwarder hands to the executor, resolved from S7's
 * string-typed struct kvrados_exec_binding. runtime is the enum the nkvx bridge
 * expects; module_ns/module_key alias the entry's strings; sha256[] is decoded
 * from the hex string when valid; caps is the tier selector. For the legacy
 * "class:method" `binding` form, class "nkvx" selects a built-in native module
 * (runtime NKVX, ns "nkvx"), any other class is a genuine object-class (CLS).
 */
struct kvrados_resolved_binding {
	enum spdk_kv_exec_runtime	runtime;
	const char			*module_ns;	/* aliases entry strings (or cls_buf) */
	const char			*module_key;
	uint8_t				sha256[SPDK_KV_EXEC_SHA256_LEN];
	bool				sha256_valid;
	uint64_t			caps;
};

/* Decode a 64-char hex sha256 into 32 bytes. Returns 0 / -EINVAL (bad length/hex). */
static int
kvrados_decode_sha256_hex(const char *hex, uint8_t out[SPDK_KV_EXEC_SHA256_LEN])
{
	size_t i;

	if (hex == NULL || strlen(hex) != SPDK_KV_EXEC_SHA256_LEN * 2) {
		return -EINVAL;
	}
	for (i = 0; i < SPDK_KV_EXEC_SHA256_LEN; i++) {
		unsigned int byte;

		if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
			return -EINVAL;
		}
		out[i] = (uint8_t)byte;
	}
	return 0;
}

/* Map a runtime name to the enum (mirrors nvmf_rpc_parse_kv_exec_runtime): "nkvx"
 * and "wasm" -> NKVX, "cls" -> CLS, NULL/unknown -> NONE. */
static enum spdk_kv_exec_runtime
kvrados_runtime_from_name(const char *name)
{
	if (name == NULL) {
		return SPDK_KV_EXEC_RUNTIME_NONE;
	}
	if (strcmp(name, "nkvx") == 0 || strcmp(name, "wasm") == 0) {
		return SPDK_KV_EXEC_RUNTIME_NKVX;
	}
	if (strcmp(name, "cls") == 0) {
		return SPDK_KV_EXEC_RUNTIME_CLS;
	}
	return SPDK_KV_EXEC_RUNTIME_NONE;
}

/*
 * Resolve S7's string binding entry into the structured form the forwarder uses.
 * `cls_buf` (caller-sized) holds the parsed legacy class so module_ns can alias it.
 * Returns 0, or -EINVAL on a malformed binding (bad sha256, bad legacy form, or
 * both legacy + structured fields set). *out is fully populated on success.
 */
static int
kvrados_resolve_binding(const struct kvrados_exec_binding *e,
			struct kvrados_resolved_binding *out,
			char *cls_buf, size_t cls_buf_len)
{
	bool has_structured = e->runtime || e->module_namespace || e->module_key ||
			      e->sha256 || e->caps;

	memset(out, 0, sizeof(*out));
	out->caps = e->caps;

	/* Legacy "class:method" migration form. */
	if (e->binding != NULL) {
		const char *colon, *method;
		size_t cls_len;

		if (has_structured) {
			SPDK_ERRLOG("KV Exec: legacy 'binding' and structured fields are mutually exclusive\n");
			return -EINVAL;
		}
		colon = strchr(e->binding, ':');
		if (colon == NULL || colon == e->binding || colon[1] == '\0') {
			SPDK_ERRLOG("KV Exec: legacy binding '%s' is not 'class:method'\n", e->binding);
			return -EINVAL;
		}
		cls_len = (size_t)(colon - e->binding);
		method = colon + 1;
		if (cls_len >= cls_buf_len) {
			SPDK_ERRLOG("KV Exec: legacy binding class too long\n");
			return -EINVAL;
		}
		memcpy(cls_buf, e->binding, cls_len);
		cls_buf[cls_len] = '\0';
		/* "nkvx:<module>" selects a BUILT-IN native module (runtime NKVX, ns
		 * "nkvx"); any other class is a genuine object-class (CLS). */
		out->runtime = (strcmp(cls_buf, "nkvx") == 0) ?
			       SPDK_KV_EXEC_RUNTIME_NKVX : SPDK_KV_EXEC_RUNTIME_CLS;
		out->module_ns = cls_buf;
		out->module_key = method;
		return 0;
	}

	/* Structured form. */
	out->runtime = kvrados_runtime_from_name(e->runtime);
	out->module_ns = e->module_namespace;
	out->module_key = e->module_key;
	if (e->sha256 != NULL) {
		if (kvrados_decode_sha256_hex(e->sha256, out->sha256) != 0) {
			SPDK_ERRLOG("KV Exec: sha256 must be 64 hex chars\n");
			return -EINVAL;
		}
		out->sha256_valid = true;
	}
	return 0;
}

/*
 * In-process NATIVE built-in Exec executor (S5a). Runs the input-observing
 * built-in modules WITHOUT a backend object store, so a native Exec round-trips
 * end-to-end with no Ceph and no Mercury — the loopback/UT path. These mirror
 * nkvx_executor.c's input-only modules exactly (bead spdk-aep):
 *   - "inputlen":  result is the input length as a little-endian uint64 (8 B).
 *   - "inputecho": result is the input bytes (truncated to osize; true len in DW0).
 * Both are READ-ONLY by construction, so they run even on a read-only namespace
 * (ADR-0008 D5). The rados-backed built-ins (bytecount/identity) and cold-fetched
 * wasm need the standalone executor over Mercury and are NOT served here.
 */
static enum spdk_kvdev_io_status
kvrados_native_builtin_exec(const char *module_key,
			    const void *input, uint32_t input_len,
			    void *out_buf, uint32_t out_len,
			    uint32_t *result_len, uint32_t *deliver_len)
{
	*result_len = 0;
	*deliver_len = 0;

	if (module_key == NULL) {
		return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
	}

	if (strcmp(module_key, "inputlen") == 0) {
		uint64_t len = input_len;
		uint32_t rlen = (uint32_t)sizeof(len);
		uint32_t deliver = (uint32_t)spdk_min((uint64_t)out_len, (uint64_t)rlen);

		*result_len = rlen;
		if (deliver > 0 && out_buf != NULL) {
			memcpy(out_buf, &len, deliver);
			*deliver_len = deliver;
		}
		return (rlen > out_len) ? SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL :
		       SPDK_KVDEV_IO_STATUS_SUCCESS;
	}

	if (strcmp(module_key, "inputecho") == 0) {
		uint32_t rlen = input_len;
		uint32_t deliver = (uint32_t)spdk_min((uint64_t)out_len, (uint64_t)rlen);

		*result_len = rlen;
		if (deliver > 0) {
			if (input == NULL || out_buf == NULL) {
				return SPDK_KVDEV_IO_STATUS_INVALID;
			}
			memcpy(out_buf, input, deliver);
			*deliver_len = deliver;
		}
		return (rlen > out_len) ? SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL :
		       SPDK_KVDEV_IO_STATUS_SUCCESS;
	}

	/* bytecount/identity need the stored object → standalone executor (Mercury). */
	return SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
}

/*
 * Complete an in-flight KV Exec exactly once: unlink it from the channel's
 * exec_inflight list (so a racing/late ABORT cannot target a freed ctx), set CQE
 * DW0 to the TRUE result length, map the status, complete the io, and free the ctx
 * (and any input bounce). Used by BOTH the native executor (synchronous) and the
 * Mercury forward done-cb.
 */
static void
kvrados_exec_complete(struct kvrados_kv_io_ctx *kctx, enum spdk_kvdev_io_status status,
		      uint32_t result_len)
{
	struct spdk_bdev_io *bdev_io = kctx->bdev_io;
	int sct, sc;

	if (kctx->is_exec) {
		TAILQ_REMOVE(&kctx->kch->exec_inflight, kctx, exec_link);
		kctx->is_exec = false;
	}

	kvrados_kvdev_status_to_nvme(status, &sct, &sc);
	spdk_bdev_io_complete_nvme_status(bdev_io, result_len, sct, sc);
	free(kctx->input_bounce);
	free(kctx);
}

#ifdef NKVX_WITH_MERCURY
/* KV Exec forward completion (fires from the front progress poller). */
static void
kvrados_exec_done(void *cb_arg, int status, uint32_t result_len)
{
	kvrados_exec_complete(cb_arg, (enum spdk_kvdev_io_status)status, result_len);
}
#endif /* NKVX_WITH_MERCURY */

/*
 * Handle KV Exec (0x83) — the full S5a dispatch (ADR-0008 D4 / ADR-0010/0012/0014).
 *
 * Command layout (ADR-0014): CDW12 = osize (host output cap), CDW13 = op_id; the
 * DPTR carries [u16 key_len][key][input ...] (key already parsed by the caller).
 * Steps:
 *   1. op-ID ALLOWLIST gate (deny-by-default) → INVALID_OPCODE if not allowed.
 *   2. Resolve S7's binding; validate the caps TIER → INVALID_FIELD.
 *   3. input = payload after the key; bound osize by the host buffer.
 *   4. Forward to the executor: native in-process built-ins (no Ceph/Mercury) or
 *      the Mercury front. Read-only-executor enforcement (D5: a write-capable
 *      module rejected on a read-only ns) is authoritative at the executor; the
 *      forwarder passes read_only through and maps READ_ONLY → write-to-RO-range.
 *   5. Register the in-flight Exec for ABORT; complete exactly once.
 */
static void
kvrados_handle_exec(struct kvrados_disk *kvrados, struct kvrados_channel *kch,
		    struct spdk_bdev_io *bdev_io, const uint8_t *key, uint8_t key_len,
		    uint64_t value_off, uint64_t payload_len,
		    struct iovec *iovs, int iovcnt)
{
	const struct spdk_nvme_cmd *cmd = &bdev_io->u.nvme_passthru.cmd;
	/*
	 * Decode KV Exec (0x83) op_id/osize from the RAW CDW12/CDW13 dwords via our
	 * out-of-tree ABI helper (spdk/kvdev.h). We do NOT read cmd->cdw1x_bits.kv_exec
	 * because those union members are our vendor additions, absent in unmodified
	 * upstream SPDK; CDW12 = osize:32 and CDW13 = op_id:32 each fill a whole dword.
	 */
	struct spdk_kv_exec_cmd exec = spdk_kv_exec_decode(cmd->cdw12, cmd->cdw13);
	uint32_t op_id = exec.op_id;
	uint32_t osize = exec.osize;
	const struct kvrados_exec_binding *e;
	struct kvrados_resolved_binding b;
	char cls_buf[64];
	const uint8_t *input = NULL;
	uint32_t input_len = 0;
	void *out_buf = NULL;
	uint32_t out_len = 0;
	uint8_t *input_bounce = NULL;
	struct kvrados_kv_io_ctx *kctx;
	uint64_t head_payload = payload_len;

	if (key_len == 0) {
		spdk_bdev_io_complete_nvme_status(bdev_io, 0, SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INVALID_KEY_SIZE);
		return;
	}

	/* TODO(memory_domain): dma-buf result sink, spdk-7sr follow-up */

	/* (1) Allowlist gate (ADR-0008 D4): deny-by-default per (namespace, op_id). */
	e = kvrados_exec_op_allowed(kvrados, op_id);
	if (e == NULL) {
		SPDK_DEBUGLOG(bdev_kvrados, "%s: KV EXEC op_id %u not in allowlist; rejecting\n",
			      kvrados->disk.name, op_id);
		spdk_bdev_io_complete_nvme_status(bdev_io, 0, SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INVALID_OPCODE);
		return;
	}

	/* (2) Resolve the binding (S7 string form → structured) + validate caps tier. */
	if (kvrados_resolve_binding(e, &b, cls_buf, sizeof(cls_buf)) != 0) {
		spdk_bdev_io_complete_nvme_status(bdev_io, 0, SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INVALID_FIELD);
		return;
	}
	if (b.caps > SPDK_KV_EXEC_CAPS_TIER_MAX) {
		SPDK_ERRLOG("%s: KV EXEC op_id %u caps tier %" PRIu64 " out of range\n",
			    kvrados->disk.name, op_id, b.caps);
		spdk_bdev_io_complete_nvme_status(bdev_io, 0, SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INVALID_FIELD);
		return;
	}

	/*
	 * (3) input = payload after the key head. The result sink is the host buffer
	 * after the input (host-VA path); osize bounds how many bytes are copied back.
	 */
	if (head_payload > value_off) {
		uint64_t ilen = head_payload - value_off;

		input_len = (uint32_t)spdk_min(ilen, (uint64_t)UINT32_MAX);
		if (iovcnt >= 1 && iovs[0].iov_len >= value_off + input_len) {
			input = (const uint8_t *)iovs[0].iov_base + value_off;
		} else if (input_len > 0) {
			input_bounce = malloc(input_len);
			if (input_bounce == NULL) {
				spdk_bdev_io_complete_nvme_status(bdev_io, 0,
								  SPDK_NVME_SCT_GENERIC,
								  SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
				return;
			}
			kvrados_gather(iovs, iovcnt, value_off, input_bounce, input_len);
			input = input_bounce;
		}
	}

	/* Host-VA result sink: the host buffer after the input, bounded by osize. */
	if (iovcnt >= 1 && iovs[0].iov_len > value_off) {
		out_buf = (uint8_t *)iovs[0].iov_base + value_off;
		out_len = (uint32_t)spdk_min(iovs[0].iov_len - value_off, (uint64_t)osize);
	}

	kctx = calloc(1, sizeof(*kctx));
	if (kctx == NULL) {
		free(input_bounce);
		spdk_bdev_io_complete_nvme_status(bdev_io, 0, SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
		return;
	}
	kctx->bdev_io = bdev_io;
	kctx->is_exec = true;
	kctx->kch = kch;
	kctx->cancel_token = 0; /* KVDEV_RADOS_NKVX_TOKEN_NONE */
	kctx->input_bounce = input_bounce;	/* owned; freed in exec_complete */
	TAILQ_INSERT_TAIL(&kch->exec_inflight, kctx, exec_link);

	SPDK_DEBUGLOG(bdev_kvrados,
		      "%s: KV EXEC op_id=%u key_len=%u input_len=%u osize=%u runtime=%d ns=%s key=%s ro=%d\n",
		      kvrados->disk.name, op_id, key_len, input_len, out_len, (int)b.runtime,
		      b.module_ns ? b.module_ns : "(none)", b.module_key ? b.module_key : "(none)",
		      kvrados->read_only);

	/*
	 * (4) Forward. A NATIVE built-in (runtime NKVX, module_namespace "nkvx") whose
	 * module is input-observing runs IN-PROCESS with no backend — the loopback path
	 * with no Ceph and no Mercury. Anything else (rados-backed built-ins, cold-fetch
	 * wasm, cls) needs the standalone executor over Mercury.
	 */
	if (b.runtime == SPDK_KV_EXEC_RUNTIME_NKVX && b.module_ns != NULL &&
	    strcmp(b.module_ns, "nkvx") == 0) {
		uint32_t result_len = 0, deliver_len = 0;
		enum spdk_kvdev_io_status st;

		/* Native built-ins write a host VA out_buf in-process (no backend). */
		st = kvrados_native_builtin_exec(b.module_key, input, input_len,
						 out_buf, out_len, &result_len, &deliver_len);
		if (st != SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED) {
			/* A native built-in ran (read-only by construction → allowed even on
			 * a read-only ns, ADR-0008 D5). exec_complete frees input_bounce. */
			kvrados_exec_complete(kctx, st, result_len);
			return;
		}
		/* Not an input-only native built-in: fall through to the Mercury path. */
	}

#ifdef NKVX_WITH_MERCURY
	{
		const uint8_t *sha = b.sha256_valid ? b.sha256 : NULL;
		int frc;

		if (kch->front == NULL) {
			SPDK_ERRLOG("%s: KV EXEC op_id %u needs an executor endpoint "
				    "(native built-in not applicable)\n", kvrados->disk.name, op_id);
			kvrados_exec_complete(kctx, SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED, 0);
			return;
		}

		/*
		 * input/input_bounce must remain valid until cb_fn fires: inline input is
		 * encoded synchronously, but large input (> NKVX_INLINE_MAX) is registered
		 * as a bulk PULL source. kctx owns input_bounce and frees it at completion.
		 *
		 * TODO(memory_domain): dma-buf result sink, spdk-7sr follow-up — the
		 * VRAM-direct variant (executor RDMA-WRITEs the result into a dma-buf via
		 * libfabric FI_MR_DMABUF instead of a host VA) is re-added via memory_domain
		 * in a separate slice. The KV Exec still forwards via the normal host-VA path.
		 */
		frc = kvdev_rados_nkvx_front_forward(kch->front, key, key_len, op_id,
						     kvrados->read_only, (uint8_t)b.runtime,
						     b.module_key, b.module_ns,
						     sha, b.sha256_valid, b.caps,
						     input, input_len, out_buf, out_len,
						     kvrados_exec_done, kctx, &kctx->cancel_token);
		if (frc != 0) {
			SPDK_ERRLOG("%s: KV EXEC forward failed: %d\n", kvrados->disk.name, frc);
			kvrados_exec_complete(kctx, SPDK_KVDEV_IO_STATUS_FAILED, 0);
		}
		return;
	}
#else
	/* Built --without-mercury and not an input-only native built-in: no transport. */
	SPDK_ERRLOG("%s: KV EXEC op_id %u needs --with-mercury or an input-only native "
		    "built-in module\n", kvrados->disk.name, op_id);
	kvrados_exec_complete(kctx, SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED, 0);
	return;
#endif
}

/*
 * KV I/O verbs arrive as SPDK_BDEV_IO_TYPE_NVME_IOV_MD with .iovs/.iovcnt set and
 * .buf == NULL (S0: nvmf routes KV via spdk_bdev_nvme_iov_passthru_md). So we
 * consume .iovs/.iovcnt here, NOT .buf. The forwarder parses the small ADR-0014
 * in-payload key header and forwards the verb to the executor; it does NOT
 * linearize the value (zero-copy host SGL, S0).
 */
static void
kvrados_handle_kv_io(struct kvrados_disk *kvrados, struct spdk_io_channel *ioch,
		     struct spdk_bdev_io *bdev_io)
{
	const struct spdk_nvme_cmd *cmd = &bdev_io->u.nvme_passthru.cmd;
	struct iovec *iovs = bdev_io->u.nvme_passthru.iovs;
	int iovcnt = bdev_io->u.nvme_passthru.iovcnt;
	uint8_t key[KVRADOS_KEY_MAX_LEN];
	uint16_t key_len;
	uint64_t payload_len = 0;
	uint64_t value_off = 0;
	int i;

	/* Read the host SGL exactly as the fabric transport laid it out (no coalesce). */
	for (i = 0; i < iovcnt; i++) {
		payload_len += iovs[i].iov_len;
	}

	/*
	 * ADR-0014: parse the in-payload key ([u16 key_len][key]) from the head of the
	 * SGL, bounds-checked. The CDW key slots are NOT used for our command set.
	 */
	key_len = kvrados_parse_inpayload_key(iovs, iovcnt, payload_len, key, &value_off);

	/*
	 * Option-C FRONT fast-path read-only gate (ADR-0008 D2/D5, ratified): on a
	 * read-only namespace, statically-mutating verbs (STORE/DELETE) are rejected
	 * HERE — at the tenant edge, before consuming an inter-tier RPC round-trip —
	 * with "Attempted Write to Read Only Range". This is advisory/fast-path only;
	 * the executor enforces authoritatively at the mutation point (the unbypassable
	 * boundary for a direct RPC client). Retrieve/Exist/List are non-mutating and
	 * always allowed. EXEC's gate is its op-ID allowlist (a later slice).
	 */
	if (kvrados->read_only &&
	    (cmd->opc == SPDK_NVME_OPC_KV_STORE || cmd->opc == SPDK_NVME_OPC_KV_DELETE)) {
		SPDK_DEBUGLOG(bdev_kvrados,
			      "%s: KV opc=0x%02x rejected (read-only ns, front fast-path)\n",
			      kvrados->disk.name, cmd->opc);
		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_COMMAND_SPECIFIC,
						  SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE);
		return;
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_KV_RETRIEVE: {
		if (key_len == 0) {
			/* Malformed/short in-payload key header. */
			spdk_bdev_io_complete_nvme_status(bdev_io, 0,
							  SPDK_NVME_SCT_GENERIC,
							  SPDK_NVME_SC_INVALID_FIELD);
			return;
		}
#ifdef NKVX_WITH_MERCURY
		{
			struct kvrados_channel *kch = spdk_io_channel_get_ctx(ioch);
			struct kvrados_kv_io_ctx *kctx;
			void *out_buf;
			uint32_t out_len;
			bool ro = kvrados->read_only;
			int frc;

			if (kch->front == NULL) {
				/* No executor configured: a forwarder cannot serve KV I/O. */
				SPDK_ERRLOG("%s: KV RETRIEVE with no executor endpoint\n",
					    kvrados->disk.name);
				spdk_bdev_io_complete_nvme_status(bdev_io, 0,
								  SPDK_NVME_SCT_GENERIC,
								  SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
				return;
			}

			/*
			 * The value buffer is the SGL region AFTER the key header, at payload
			 * offset value_off. The client may carry the key head EITHER inline in
			 * iov[0] before the value (single contiguous DPTR) OR as its own SGL
			 * segment, in which case value_off lands at the head of a later iov (the
			 * value stays a separate, VRAM/p2pdma-capable region). Walk the SGL to
			 * the iov holding value_off and hand the bridge that contiguous span as
			 * the result sink (zero-copy / PUSH for large values; a small value comes
			 * back inline and the bridge copies it in).
			 */
			out_buf = NULL;
			out_len = 0;
			{
				uint64_t io_off = 0;
				int j;

				for (j = 0; j < iovcnt; j++) {
					if (value_off < io_off + iovs[j].iov_len) {
						uint64_t in_iov = value_off - io_off;

						out_buf = (uint8_t *)iovs[j].iov_base + in_iov;
						out_len = (uint32_t)spdk_min(iovs[j].iov_len - in_iov,
									     (uint64_t)UINT32_MAX);
						break;
					}
					io_off += iovs[j].iov_len;
				}
			}

			kctx = calloc(1, sizeof(*kctx));
			if (kctx == NULL) {
				spdk_bdev_io_complete_nvme_status(bdev_io, 0,
								  SPDK_NVME_SCT_GENERIC,
								  SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
				return;
			}
			kctx->bdev_io = bdev_io;

			SPDK_DEBUGLOG(bdev_kvrados,
				      "%s: KV RETRIEVE key_len=%u iovcnt=%d payload=%" PRIu64
				      " value_off=%" PRIu64 " osize=%u ro=%d\n",
				      kvrados->disk.name, key_len, iovcnt, payload_len,
				      value_off, out_len, ro);

			frc = kvdev_rados_nkvx_front_retrieve(kch->front, key, (uint8_t)key_len,
							      ro, out_buf, out_len,
							      kvrados_retrieve_done, kctx);
			if (frc != 0) {
				SPDK_ERRLOG("%s: KV RETRIEVE forward failed: %d\n",
					    kvrados->disk.name, frc);
				free(kctx);
				spdk_bdev_io_complete_nvme_status(bdev_io, 0,
								  SPDK_NVME_SCT_GENERIC,
								  SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
			}
			return;
		}
#else
		/* Built --without-mercury: the forwarder has no transport to the executor. */
		SPDK_ERRLOG("%s: KV RETRIEVE needs --with-mercury (no inter-tier transport)\n",
			    kvrados->disk.name);
		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INVALID_OPCODE);
		return;
#endif
	}
	case SPDK_NVME_OPC_KV_STORE: {
		if (key_len == 0) {
			spdk_bdev_io_complete_nvme_status(bdev_io, 0,
							  SPDK_NVME_SCT_GENERIC,
							  SPDK_NVME_SC_INVALID_FIELD);
			return;
		}
#ifdef NKVX_WITH_MERCURY
		{
			struct kvrados_channel *kch = spdk_io_channel_get_ctx(ioch);
			struct kvrados_kv_io_ctx *kctx;
			uint8_t store_flags = kvrados_decode_store_flags(cmd);
			uint64_t value_len64 = (payload_len > value_off) ? payload_len - value_off : 0;
			uint32_t value_len = (uint32_t)spdk_min(value_len64, (uint64_t)UINT32_MAX);
			void *value = NULL;
			void *value_bounce = NULL;	/* gathered multi-segment value; outlives the RPC */
			int frc;

			if (kch->front == NULL) {
				SPDK_ERRLOG("%s: KV STORE with no executor endpoint\n",
					    kvrados->disk.name);
				spdk_bdev_io_complete_nvme_status(bdev_io, 0,
								  SPDK_NVME_SCT_GENERIC,
								  SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
				return;
			}

			/*
			 * Locate the value: the SGL region after the key header, at payload
			 * offset value_off (after A's in-payload key the value is its own SGL
			 * segment). Walk to the iov holding value_off (mirrors the RETRIEVE sink
			 * walk) to find the contiguous span.
			 *
			 * Delivery splits by size:
			 *   - SMALL (<= NKVX_INLINE_MAX): rides inline, serialized synchronously by
			 *     the forward via a CPU read — so a contiguous span is handed directly
			 *     (zero-copy); the client's vfio-user DMA region is fine to memcpy from.
			 *   - LARGE (> NKVX_INLINE_MAX): the executor RDMA-PULLs the value, so the
			 *     source must be a buffer the forwarder can ibv_reg_mr and the remote
			 *     NIC can read. A plain-VA bulk over the client's vfio-user mapping is
			 *     NOT remotely readable (the executor's RDMA-READ returns zeros — proven
			 *     in-container), so gather the value into a forwarder heap bounce, which
			 *     is registerable. The bounce backs the in-flight PULL, so it MUST
			 *     outlive the forward (kctx owns it, freed in kvrados_retrieve_done).
			 *     This mirrors the Exec large-input path, which also bounces. (True
			 *     zero-copy / p2pdma straight from the client buffer needs memory_domain
			 *     / dma-buf MR registration of the vfio-user region — a later slice.)
			 */
			if (value_len > 0) {
				uint8_t *span = NULL;
				uint64_t span_len = 0;
				uint64_t io_off = 0;
				int j;

				for (j = 0; j < iovcnt; j++) {
					if (value_off < io_off + iovs[j].iov_len) {
						uint64_t in_iov = value_off - io_off;

						span = (uint8_t *)iovs[j].iov_base + in_iov;
						span_len = iovs[j].iov_len - in_iov;
						break;
					}
					io_off += iovs[j].iov_len;
				}

				if (value_len <= NKVX_INLINE_MAX && span != NULL &&
				    span_len >= value_len) {
					value = span;			/* small + contiguous: zero-copy inline */
				} else {
					/* Large (RDMA-PULL source) or a non-contiguous small value:
					 * gather into a registerable forwarder bounce. */
					value_bounce = malloc(value_len);
					if (value_bounce == NULL) {
						spdk_bdev_io_complete_nvme_status(bdev_io, 0,
								  SPDK_NVME_SCT_GENERIC,
								  SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
						return;
					}
					kvrados_gather(iovs, iovcnt, value_off, value_bounce, value_len);
					value = value_bounce;
				}
			}

			kctx = calloc(1, sizeof(*kctx));
			if (kctx == NULL) {
				free(value_bounce);
				spdk_bdev_io_complete_nvme_status(bdev_io, 0,
								  SPDK_NVME_SCT_GENERIC,
								  SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
				return;
			}
			kctx->bdev_io = bdev_io;
			/* Own the gather bounce (if any): a large value is RDMA-PULLed by the
			 * executor AFTER the forward returns, so the source must stay valid until
			 * completion. NULL on the zero-copy path. Freed in kvrados_retrieve_done. */
			kctx->input_bounce = value_bounce;

			SPDK_DEBUGLOG(bdev_kvrados,
				      "%s: KV STORE key_len=%u value_len=%u flags=0x%x ro=%d\n",
				      kvrados->disk.name, key_len, value_len, store_flags,
				      kvrados->read_only);

			frc = kvdev_rados_nkvx_front_store(kch->front, key, (uint8_t)key_len,
							   kvrados->read_only, store_flags,
							   value, value_len,
							   kvrados_retrieve_done, kctx);
			/* On success the forward owns the value lifetime via kctx (the value
			 * bytes ride inline synchronously for a small value, or are PULLed later
			 * from the zero-copy region / bounce for a large one). On failure cb did
			 * not run, so release the bounce + kctx here. */
			if (frc != 0) {
				SPDK_ERRLOG("%s: KV STORE forward failed: %d\n",
					    kvrados->disk.name, frc);
				free(kctx->input_bounce);
				free(kctx);
				spdk_bdev_io_complete_nvme_status(bdev_io, 0,
								  SPDK_NVME_SCT_GENERIC,
								  SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
			}
			return;
		}
#else
		SPDK_ERRLOG("%s: KV STORE needs --with-mercury\n", kvrados->disk.name);
		spdk_bdev_io_complete_nvme_status(bdev_io, 0, SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INVALID_OPCODE);
		return;
#endif
	}
	case SPDK_NVME_OPC_KV_DELETE:
	case SPDK_NVME_OPC_KV_EXIST: {
		if (key_len == 0) {
			spdk_bdev_io_complete_nvme_status(bdev_io, 0,
							  SPDK_NVME_SCT_GENERIC,
							  SPDK_NVME_SC_INVALID_FIELD);
			return;
		}
#ifdef NKVX_WITH_MERCURY
		{
			struct kvrados_channel *kch = spdk_io_channel_get_ctx(ioch);
			struct kvrados_kv_io_ctx *kctx;
			bool is_delete = (cmd->opc == SPDK_NVME_OPC_KV_DELETE);
			int frc;

			if (kch->front == NULL) {
				SPDK_ERRLOG("%s: KV %s with no executor endpoint\n",
					    kvrados->disk.name, is_delete ? "DELETE" : "EXIST");
				spdk_bdev_io_complete_nvme_status(bdev_io, 0,
								  SPDK_NVME_SCT_GENERIC,
								  SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
				return;
			}

			kctx = calloc(1, sizeof(*kctx));
			if (kctx == NULL) {
				spdk_bdev_io_complete_nvme_status(bdev_io, 0,
								  SPDK_NVME_SCT_GENERIC,
								  SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
				return;
			}
			kctx->bdev_io = bdev_io;

			SPDK_DEBUGLOG(bdev_kvrados, "%s: KV %s key_len=%u ro=%d\n",
				      kvrados->disk.name, is_delete ? "DELETE" : "EXIST",
				      key_len, kvrados->read_only);

			if (is_delete) {
				frc = kvdev_rados_nkvx_front_delete(kch->front, key,
								    (uint8_t)key_len,
								    kvrados->read_only,
								    kvrados_retrieve_done, kctx);
			} else {
				frc = kvdev_rados_nkvx_front_exist(kch->front, key,
								   (uint8_t)key_len,
								   kvrados->read_only,
								   kvrados_retrieve_done, kctx);
			}
			if (frc != 0) {
				SPDK_ERRLOG("%s: KV %s forward failed: %d\n",
					    kvrados->disk.name, is_delete ? "DELETE" : "EXIST", frc);
				free(kctx);
				spdk_bdev_io_complete_nvme_status(bdev_io, 0,
								  SPDK_NVME_SCT_GENERIC,
								  SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
			}
			return;
		}
#else
		SPDK_ERRLOG("%s: KV opc=0x%02x needs --with-mercury\n",
			    kvrados->disk.name, cmd->opc);
		spdk_bdev_io_complete_nvme_status(bdev_io, 0, SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INVALID_OPCODE);
		return;
#endif
	}
	case SPDK_NVME_OPC_KV_LIST: {
#ifdef NKVX_WITH_MERCURY
		{
			struct kvrados_channel *kch = spdk_io_channel_get_ctx(ioch);
			struct kvrados_kv_io_ctx *kctx;
			void *out_buf;
			uint32_t out_len;
			uint8_t start_len = (uint8_t)spdk_min((uint64_t)key_len, (uint64_t)UINT8_MAX);
			int frc;

			if (kch->front == NULL) {
				SPDK_ERRLOG("%s: KV LIST with no executor endpoint\n",
					    kvrados->disk.name);
				spdk_bdev_io_complete_nvme_status(bdev_io, 0,
								  SPDK_NVME_SCT_GENERIC,
								  SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
				return;
			}

			/*
			 * LIST has no in-payload key header to skip — the whole DPTR is the
			 * output buffer for the NRK-prefixed listing. The start position (if
			 * any) rides in the key header like the other verbs; key_len==0 lists
			 * from the first key. The listing lands in the host buffer (iov[0]).
			 */
			if (iovcnt >= 1) {
				out_buf = iovs[0].iov_base;
				out_len = (uint32_t)spdk_min(iovs[0].iov_len, (uint64_t)UINT32_MAX);
			} else {
				out_buf = NULL;
				out_len = 0;
			}

			kctx = calloc(1, sizeof(*kctx));
			if (kctx == NULL) {
				spdk_bdev_io_complete_nvme_status(bdev_io, 0,
								  SPDK_NVME_SCT_GENERIC,
								  SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
				return;
			}
			kctx->bdev_io = bdev_io;

			SPDK_DEBUGLOG(bdev_kvrados,
				      "%s: KV LIST start_key_len=%u osize=%u\n",
				      kvrados->disk.name, start_len, out_len);

			frc = kvdev_rados_nkvx_front_list(kch->front, key, start_len,
							  kvrados->read_only, out_buf, out_len,
							  kvrados_retrieve_done, kctx);
			if (frc != 0) {
				SPDK_ERRLOG("%s: KV LIST forward failed: %d\n",
					    kvrados->disk.name, frc);
				free(kctx);
				spdk_bdev_io_complete_nvme_status(bdev_io, 0,
								  SPDK_NVME_SCT_GENERIC,
								  SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
			}
			return;
		}
#else
		SPDK_ERRLOG("%s: KV LIST needs --with-mercury\n", kvrados->disk.name);
		spdk_bdev_io_complete_nvme_status(bdev_io, 0, SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INVALID_OPCODE);
		return;
#endif
	}
	case SPDK_NVME_OPC_KV_EXEC: {
		/*
		 * KV Exec (0x83), S5a. op-ID allowlist + binding + caps + native/forward
		 * dispatch + abort registration live in kvrados_handle_exec. The channel
		 * ctx carries the per-channel front + in-flight Exec list.
		 */
		struct kvrados_channel *kch = spdk_io_channel_get_ctx(ioch);

		kvrados_handle_exec(kvrados, kch, bdev_io, key, (uint8_t)key_len,
				    value_off, payload_len, iovs, iovcnt);
		return;
	}
	default:
		SPDK_DEBUGLOG(bdev_kvrados, "%s: unknown KV opcode 0x%02x\n",
			      kvrados->disk.name, cmd->opc);
		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_INVALID_OPCODE);
		return;
	}
}

/*
 * Admin IDENTIFY for the KV command set. nvmf delivers admin passthru via
 * spdk_bdev_nvme_admin_passthru, which sets .buf (contiguous), so the identify
 * handler reads .buf — distinct from the KV I/O path which uses .iovs.
 *
 * The capabilities are filled from CREATE-TIME config (there is no front
 * librados in the forwarder model):
 *   CNS 05h (SPDK_NVME_IDENTIFY_NS_IOCS)    -> spdk_nvme_kv_ns_data
 *   CNS 06h (SPDK_NVME_IDENTIFY_CTRLR_IOCS) -> spdk_nvme_kv_ctrlr_data
 */
static void
kvrados_handle_admin_identify(struct kvrados_disk *kvrados, struct spdk_bdev_io *bdev_io)
{
	struct spdk_nvme_cmd *cmd = &bdev_io->u.nvme_passthru.cmd;
	void *buf = bdev_io->u.nvme_passthru.buf;
	uint32_t nbytes = bdev_io->u.nvme_passthru.nbytes;
	uint8_t cns = cmd->cdw10_bits.identify.cns;
	uint8_t csi = cmd->cdw11_bits.identify.csi;

	if (csi != SPDK_NVME_CSI_KV) {
		goto invalid;
	}

	switch (cns) {
	case SPDK_NVME_IDENTIFY_NS_IOCS: {
		struct spdk_nvme_kv_ns_data *nsdata;

		if (cmd->nsid != kvrados->disk.nsid) {
			spdk_bdev_io_complete_nvme_status(bdev_io, 0,
							  SPDK_NVME_SCT_GENERIC,
							  SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);
			return;
		}

		if (buf == NULL || nbytes < sizeof(*nsdata)) {
			goto invalid;
		}

		memset(buf, 0, nbytes);
		nsdata = buf;
		nsdata->nsze = kvrados->num_keys;
		nsdata->nuse = 0;
		nsdata->nkvf = 0;
		nsdata->kvfc.kvfi = 0;
		nsdata->novg = kvrados->optimal_value_granularity;
		nsdata->kvf[0].kvkml = kvrados->max_key_size;
		nsdata->kvf[0].kvvml = kvrados->max_value_size;

		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_SUCCESS);
		return;
	}
	case SPDK_NVME_IDENTIFY_CTRLR_IOCS: {
		struct spdk_nvme_kv_ctrlr_data *cdata;

		if (buf == NULL || nbytes < sizeof(*cdata)) {
			goto invalid;
		}

		memset(buf, 0, nbytes);
		cdata = buf;
		cdata->ver = SPDK_NVME_KV_SPEC_VER;

		spdk_bdev_io_complete_nvme_status(bdev_io, 0,
						  SPDK_NVME_SCT_GENERIC,
						  SPDK_NVME_SC_SUCCESS);
		return;
	}
	default:
		break;
	}

invalid:
	spdk_bdev_io_complete_nvme_status(bdev_io, 0,
					  SPDK_NVME_SCT_GENERIC,
					  SPDK_NVME_SC_INVALID_FIELD);
}

/*
 * Tenant NVMe ABORT against a KV namespace (S5a). The bdev layer hands us the
 * in-flight bdev_io to abort in bdev_io->u.abort.bio_to_abort. KV Exec is the only
 * abortable KV op; we look it up on this channel's exec_inflight list and request a
 * cancel of the in-flight forward (Mercury). The ORIGINAL Exec still completes
 * exactly once — with ABORTED — from its own done-cb; this routine MUST NOT
 * complete it. We complete the ABORT bdev_io itself via the bdev_io status the
 * abort path expects: SUCCESS ("command aborted") when the cancel was entered,
 * FAILED otherwise (already completed / not abortable / no transport). nvmf maps a
 * SUCCESS abort completion to CQE DW0 bit0 = 0.
 *
 * Note: the in-process native built-in Exec completes synchronously at submit, so
 * it is never on the inflight list when an ABORT arrives → reported "not aborted",
 * which is correct (nothing was in flight to cancel).
 */
static void
kvrados_handle_abort(struct kvrados_channel *kch, struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_io *to_abort = bdev_io->u.abort.bio_to_abort;
	struct kvrados_kv_io_ctx *kctx;
	bool aborted = false;

	TAILQ_FOREACH(kctx, &kch->exec_inflight, exec_link) {
		if (kctx->bdev_io == to_abort) {
#ifdef NKVX_WITH_MERCURY
			if (kch->front != NULL && kctx->cancel_token != KVDEV_RADOS_NKVX_TOKEN_NONE) {
				/* Enter the per-command cancel handshake; the progress poller
				 * resolves it and fires the Exec done-cb (ABORTED) once. */
				aborted = kvdev_rados_nkvx_front_cancel(kch->front,
									kctx->cancel_token);
			}
#endif
			break;
		}
	}

	spdk_bdev_io_complete(bdev_io, aborted ? SPDK_BDEV_IO_STATUS_SUCCESS :
			      SPDK_BDEV_IO_STATUS_FAILED);
}

static void
kvrados_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct kvrados_disk *kvrados = bdev_io->bdev->ctxt;
	struct spdk_nvme_cmd *cmd;

	switch (bdev_io->type) {
	/*
	 * KV I/O passthru. nvmf delivers the scatter (iov) variant, so the
	 * forwarder reads .iovs/.iovcnt. The contiguous NVME_IO variant is
	 * accepted too for completeness (single iov), but the real fabric path
	 * is NVME_IOV_MD.
	 */
	case SPDK_BDEV_IO_TYPE_NVME_IOV_MD:
	case SPDK_BDEV_IO_TYPE_NVME_IO:
		cmd = &bdev_io->u.nvme_passthru.cmd;

		if (cmd->nsid != kvrados->disk.nsid) {
			spdk_bdev_io_complete_nvme_status(bdev_io, 0,
							  SPDK_NVME_SCT_GENERIC,
							  SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT);
			return;
		}

		kvrados_handle_kv_io(kvrados, ch, bdev_io);
		return;
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
		cmd = &bdev_io->u.nvme_passthru.cmd;
		if (cmd->opc == SPDK_NVME_OPC_IDENTIFY) {
			kvrados_handle_admin_identify(kvrados, bdev_io);
			return;
		}
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		/* Tenant NVMe ABORT: cancel an in-flight KV Exec (S5a). */
		kvrados_handle_abort(spdk_io_channel_get_ctx(ch), bdev_io);
		return;
	default:
		break;
	}

	spdk_bdev_io_complete_nvme_status(bdev_io, 0,
					  SPDK_NVME_SCT_GENERIC,
					  SPDK_NVME_SC_INVALID_OPCODE);
}

static bool
kvrados_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IOV_MD:
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
	/* ABORT lets the nvmf Abort path reach an in-flight KV Exec (S5a). */
	case SPDK_BDEV_IO_TYPE_ABORT:
		return true;
	default:
		return false;
	}
}

static struct spdk_io_channel *
kvrados_get_io_channel(void *ctx)
{
	/* Per-disk io_device: a channel's create cb sees THIS disk and can stand up a
	 * front to its executor_endpoint (the endpoint is per-disk). */
	struct kvrados_disk *kvrados = ctx;

	return spdk_get_io_channel(kvrados);
}

static void
kvrados_free_exec_allowlist(struct kvrados_exec_binding *allowlist, size_t count)
{
	size_t i;

	if (allowlist == NULL) {
		return;
	}
	for (i = 0; i < count; i++) {
		free(allowlist[i].binding);
		free(allowlist[i].runtime);
		free(allowlist[i].module_namespace);
		free(allowlist[i].module_key);
		free(allowlist[i].sha256);
	}
	free(allowlist);
}

/*
 * Replace a kvrados bdev's KV-Exec allowlist at RUNTIME (the out-of-tree analogue
 * of the in-tree nvmf_ns_set_kv_exec_allowlist). Looks the disk up by bdev name,
 * deep-copies the borrowed binding view, and swaps it in (freeing the prior one).
 * The forwarder runs a single reactor (-m 0x2), so the RPC and the IO read path
 * (kvrados_exec_op_allowed) execute on the same thread — the swap cannot be observed
 * torn. (A multi-reactor deployment would marshal the swap onto the bdev's thread.)
 * On allocation failure the existing allowlist is left intact. Returns 0, -ENODEV
 * (no such bdev), -EINVAL (not a kvrados bdev), or -ENOMEM.
 */
int
bdev_kvrados_set_exec_allowlist(const char *name,
				const struct kvrados_exec_binding *allowlist, size_t count)
{
	struct spdk_bdev *bdev = spdk_bdev_get_by_name(name);
	struct kvrados_disk *kvrados;
	struct kvrados_exec_binding *na = NULL;
	size_t i;

	if (bdev == NULL) {
		return -ENODEV;
	}
	if (strcmp(spdk_bdev_get_module_name(bdev), kvrados_if.name) != 0) {
		return -EINVAL;		/* not a kvrados bdev */
	}
	kvrados = SPDK_CONTAINEROF(bdev, struct kvrados_disk, disk);

	if (count > 0) {
		na = calloc(count, sizeof(*na));
		if (na == NULL) {
			return -ENOMEM;
		}
		for (i = 0; i < count; i++) {
			const struct kvrados_exec_binding *s = &allowlist[i];
			struct kvrados_exec_binding *d = &na[i];

			d->op_id = s->op_id;
			d->caps = s->caps;
			if ((s->binding && !(d->binding = strdup(s->binding))) ||
			    (s->runtime && !(d->runtime = strdup(s->runtime))) ||
			    (s->module_namespace &&
			     !(d->module_namespace = strdup(s->module_namespace))) ||
			    (s->module_key && !(d->module_key = strdup(s->module_key))) ||
			    (s->sha256 && !(d->sha256 = strdup(s->sha256)))) {
				kvrados_free_exec_allowlist(na, i + 1);
				return -ENOMEM;
			}
		}
	}

	/* Same-thread swap: no reader can observe a torn (pointer, count) pair. */
	kvrados_free_exec_allowlist(kvrados->exec_allowlist, kvrados->exec_allowlist_count);
	kvrados->exec_allowlist = na;
	kvrados->exec_allowlist_count = count;
	return 0;
}

static void
kvrados_disk_unregister_done(void *io_device)
{
	struct kvrados_disk *kvrados = io_device;

	TAILQ_REMOVE(&g_kvrados_disks, kvrados, link);
	kvrados_free_exec_allowlist(kvrados->exec_allowlist, kvrados->exec_allowlist_count);
	free(kvrados->executor_endpoint);
	free(kvrados->disk.name);
	free(kvrados);
}

static int
kvrados_destruct(void *ctx)
{
	struct kvrados_disk *kvrados = ctx;

	/* Async: free the disk only after the per-disk io_device's channels are gone. */
	spdk_io_device_unregister(kvrados, kvrados_disk_unregister_done);
	return 0;
}

static void
kvrados_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct kvrados_disk *kvrados = bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_kvrados_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint32(w, "max_key_size", kvrados->max_key_size);
	spdk_json_write_named_uint32(w, "max_value_size", kvrados->max_value_size);
	spdk_json_write_named_uint32(w, "optimal_value_granularity",
				     kvrados->optimal_value_granularity);
	spdk_json_write_named_uint64(w, "num_keys", kvrados->num_keys);
	if (kvrados->executor_endpoint != NULL) {
		spdk_json_write_named_string(w, "executor_endpoint", kvrados->executor_endpoint);
	}
	if (kvrados->read_only) {
		spdk_json_write_named_bool(w, "read_only", kvrados->read_only);
	}
	if (kvrados->exec_allowlist_count > 0) {
		size_t i;

		spdk_json_write_named_array_begin(w, "exec_allowlist");
		for (i = 0; i < kvrados->exec_allowlist_count; i++) {
			const struct kvrados_exec_binding *b = &kvrados->exec_allowlist[i];

			spdk_json_write_object_begin(w);
			spdk_json_write_named_uint32(w, "op_id", b->op_id);
			if (b->binding != NULL) {
				spdk_json_write_named_string(w, "binding", b->binding);
			}
			if (b->runtime != NULL) {
				spdk_json_write_named_string(w, "runtime", b->runtime);
			}
			if (b->module_namespace != NULL) {
				spdk_json_write_named_string(w, "module_namespace", b->module_namespace);
			}
			if (b->module_key != NULL) {
				spdk_json_write_named_string(w, "module_key", b->module_key);
			}
			if (b->sha256 != NULL) {
				spdk_json_write_named_string(w, "sha256", b->sha256);
			}
			if (b->caps != 0) {
				spdk_json_write_named_uint64(w, "caps", b->caps);
			}
			spdk_json_write_object_end(w);
		}
		spdk_json_write_array_end(w);
	}
	spdk_json_write_named_uuid(w, "uuid", &bdev->uuid);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table kvrados_fn_table = {
	.destruct		= kvrados_destruct,
	.submit_request		= kvrados_submit_request,
	.io_type_supported	= kvrados_io_type_supported,
	.get_io_channel		= kvrados_get_io_channel,
	.write_config_json	= kvrados_write_json_config,
};

#ifdef NKVX_WITH_MERCURY
/* Per-channel Mercury progress poller: drive HG_Progress/HG_Trigger non-blocking on
 * the reactor thread so forward completions fire here (design §4.2). */
static int
kvrados_front_poll(void *arg)
{
	struct kvrados_channel *kch = arg;
	int n = kvdev_rados_nkvx_front_progress(kch->front);

	return (n > 0) ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}
#endif

static int
kvrados_create_channel_cb(void *io_device, void *ctx)
{
	struct kvrados_channel *kch = ctx;

	/* In-flight KV Exec list (for tenant ABORT) — always present. */
	TAILQ_INIT(&kch->exec_inflight);

#ifdef NKVX_WITH_MERCURY
	{
		struct kvrados_disk *kvrados = io_device;

		kch->front = NULL;
		kch->front_poller = NULL;

		/* Stand up this channel's front client + progress poller when an executor is
		 * configured. The executor must be up and listening before the first channel
		 * is created (bootstrap, OQ-8). */
		if (kvrados->executor_endpoint != NULL) {
			int rc = kvdev_rados_nkvx_front_create(kvrados->executor_endpoint, &kch->front);

			if (rc != 0) {
				SPDK_ERRLOG("%s: front client to executor '%s' failed: %s\n",
					    kvrados->disk.name, kvrados->executor_endpoint,
					    spdk_strerror(-rc));
				return rc;
			}
			kch->front_poller = SPDK_POLLER_REGISTER(kvrados_front_poll, kch, 0);
			if (kch->front_poller == NULL) {
				kvdev_rados_nkvx_front_destroy(kch->front);
				kch->front = NULL;
				return -ENOMEM;
			}
		}
	}
#else
	(void)io_device;
#endif
	return 0;
}

static void
kvrados_destroy_channel_cb(void *io_device, void *ctx)
{
	struct kvrados_channel *kch = ctx;

	(void)io_device;

#ifdef NKVX_WITH_MERCURY
	if (kch->front != NULL) {
		/* Drain in-flight forwards before tearing the front down (channel-destroy
		 * teardown — best-effort cancel + bounded drain, then force-fail any
		 * residual so no io hangs; UAF-safe per the front handshake). Each forced
		 * completion fires its done-cb, which unlinks from exec_inflight. */
		kvdev_rados_nkvx_front_cancel_all(kch->front);
		while (kvdev_rados_nkvx_front_outstanding(kch->front) > 0) {
			if (kvdev_rados_nkvx_front_drain_progress(kch->front, 2) < 0) {
				break;
			}
		}
		if (kvdev_rados_nkvx_front_outstanding(kch->front) > 0) {
			kvdev_rados_nkvx_front_fail_all_pending(kch->front,
								SPDK_KVDEV_IO_STATUS_FAILED);
		}
		spdk_poller_unregister(&kch->front_poller);
		kvdev_rados_nkvx_front_destroy(kch->front);
		kch->front = NULL;
	}
#endif

	/*
	 * After the front drain, every forwarded Exec has fired its done-cb, which
	 * unlinked it from exec_inflight; a native Exec completes synchronously. The
	 * list MUST be empty now.
	 */
	if (!TAILQ_EMPTY(&kch->exec_inflight)) {
		SPDK_ERRLOG("kvrados: exec_inflight not empty at channel destroy\n");
		assert(false);
	}
}

int
create_kvrados_disk(struct spdk_bdev **bdev, const struct kvrados_bdev_opts *opts)
{
	struct kvrados_disk *kvrados;
	int rc;

	if (opts->max_key_size > KVRADOS_KEY_MAX_LEN) {
		/* ADR-0014 in-payload keys are bounded at 255 (SPDK_KVDEV_EXEC_KEY_MAX_LEN). */
		SPDK_ERRLOG("max_key_size must not exceed %u\n", KVRADOS_KEY_MAX_LEN);
		return -EINVAL;
	}

	if (opts->max_value_size > 0 && opts->optimal_value_granularity > opts->max_value_size) {
		SPDK_ERRLOG("optimal_value_granularity must not exceed max_value_size\n");
		return -EINVAL;
	}

	kvrados = calloc(1, sizeof(*kvrados));
	if (!kvrados) {
		SPDK_ERRLOG("kvrados calloc() failed\n");
		return -ENOMEM;
	}

	if (opts->name) {
		kvrados->disk.name = strdup(opts->name);
	} else {
		kvrados->disk.name = spdk_sprintf_alloc("KVRados%d", g_kvrados_disk_count);
		g_kvrados_disk_count++;
	}
	if (!kvrados->disk.name) {
		free(kvrados);
		return -ENOMEM;
	}

	if (opts->executor_endpoint) {
		kvrados->executor_endpoint = strdup(opts->executor_endpoint);
		if (!kvrados->executor_endpoint) {
			free(kvrados->disk.name);
			free(kvrados);
			return -ENOMEM;
		}
	}

	kvrados->disk.product_name = "KVRados disk";

	kvrados->max_key_size = opts->max_key_size > 0 ? opts->max_key_size :
			       KVRADOS_DEFAULT_MAX_KEY_SIZE;
	kvrados->max_value_size = opts->max_value_size > 0 ? opts->max_value_size :
				 KVRADOS_DEFAULT_MAX_VALUE_SIZE;
	kvrados->optimal_value_granularity = opts->optimal_value_granularity > 0 ?
					    opts->optimal_value_granularity :
					    KVRADOS_DEFAULT_OPTIMAL_VALUE_GRANULARITY;
	kvrados->num_keys = opts->num_keys;
	kvrados->read_only = opts->read_only;

	/* Deep-copy the KV Exec allowlist/bindings so the disk owns its strings. */
	if (opts->exec_allowlist_count > 0) {
		size_t i;

		kvrados->exec_allowlist = calloc(opts->exec_allowlist_count,
						 sizeof(*kvrados->exec_allowlist));
		if (!kvrados->exec_allowlist) {
			free(kvrados->executor_endpoint);
			free(kvrados->disk.name);
			free(kvrados);
			return -ENOMEM;
		}
		for (i = 0; i < opts->exec_allowlist_count; i++) {
			const struct kvrados_exec_binding *src = &opts->exec_allowlist[i];
			struct kvrados_exec_binding *dst = &kvrados->exec_allowlist[i];

			dst->op_id = src->op_id;
			dst->caps = src->caps;
			if ((src->binding && !(dst->binding = strdup(src->binding))) ||
			    (src->runtime && !(dst->runtime = strdup(src->runtime))) ||
			    (src->module_namespace &&
			     !(dst->module_namespace = strdup(src->module_namespace))) ||
			    (src->module_key && !(dst->module_key = strdup(src->module_key))) ||
			    (src->sha256 && !(dst->sha256 = strdup(src->sha256)))) {
				kvrados->exec_allowlist_count = i + 1;
				kvrados_free_exec_allowlist(kvrados->exec_allowlist,
							    kvrados->exec_allowlist_count);
				free(kvrados->executor_endpoint);
				free(kvrados->disk.name);
				free(kvrados);
				return -ENOMEM;
			}
		}
		kvrados->exec_allowlist_count = opts->exec_allowlist_count;
	}

	/*
	 * KV devices have no block layout, but the bdev layer requires non-zero
	 * blocklen/blockcnt. Use 1-byte blocks with the max value size as a
	 * nominal capacity (mirrors bdev_kvmalloc).
	 */
	kvrados->disk.blocklen = 1;
	kvrados->disk.blockcnt = kvrados->max_value_size;

	kvrados->disk.ctxt = kvrados;
	kvrados->disk.fn_table = &kvrados_fn_table;
	kvrados->disk.module = &kvrados_if;

	kvrados->disk.csi = SPDK_NVME_CSI_KV;
	kvrados->disk.nsid = 1;

	if (!spdk_uuid_is_null(&opts->uuid)) {
		spdk_uuid_copy(&kvrados->disk.uuid, &opts->uuid);
	}

	kvrados->disk.numa.id_valid = 1;
	kvrados->disk.numa.id = opts->numa_id;

	/* Per-disk io_device so each channel's front targets THIS disk's executor
	 * endpoint. Registered before bdev_register so get_io_channel always resolves. */
	spdk_io_device_register(kvrados, kvrados_create_channel_cb,
				kvrados_destroy_channel_cb, sizeof(struct kvrados_channel),
				kvrados->disk.name);

	rc = spdk_bdev_register(&kvrados->disk);
	if (rc) {
		spdk_io_device_unregister(kvrados, NULL);
		kvrados_free_exec_allowlist(kvrados->exec_allowlist, kvrados->exec_allowlist_count);
		free(kvrados->executor_endpoint);
		free(kvrados->disk.name);
		free(kvrados);
		return rc;
	}

	*bdev = &kvrados->disk;

	TAILQ_INSERT_TAIL(&g_kvrados_disks, kvrados, link);
	SPDK_DEBUGLOG(bdev_kvrados,
		      "KV forwarder bdev %s created (max_key=%u max_value=%u opt_gran=%u num_keys=%" PRIu64 " endpoint=%s)\n",
		      spdk_bdev_get_name(*bdev), kvrados->max_key_size,
		      kvrados->max_value_size, kvrados->optimal_value_granularity,
		      kvrados->num_keys,
		      kvrados->executor_endpoint ? kvrados->executor_endpoint : "(none)");

	return 0;
}

void
delete_kvrados_disk(const char *name, delete_kvrados_complete cb_fn, void *cb_arg)
{
	int rc;

	rc = spdk_bdev_unregister_by_name(name, &kvrados_if, cb_fn, cb_arg);
	if (rc != 0) {
		cb_fn(cb_arg, rc);
	}
}

static int
bdev_kvrados_init(void)
{
	/* io_devices are registered per-disk (in create_kvrados_disk) so each channel
	 * can target its disk's executor endpoint. Nothing module-global to set up. */
	return 0;
}

static void
bdev_kvrados_fini(void)
{
}

SPDK_LOG_REGISTER_COMPONENT(bdev_kvrados)
