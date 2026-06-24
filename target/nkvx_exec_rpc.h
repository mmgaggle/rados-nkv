/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * Inter-tier Exec RPC contract (Slice C3, ADR-0015 + design doc
 * docs/design/slice-c-exec-rpc-mercury.md §1).
 *
 * One Mercury RPC, `nkvx_exec`: the front (`rados-nkv`, an SPDK reactor that
 * terminates the tenant NVMe-KV Exec) sends a request to the executor
 * (`rados-nkvx`, a standalone non-SPDK Mercury service) and gets back a result.
 *
 * This header is SHARED between the two separately-compiled tiers. Wire encoding
 * is Mercury's portable `hg_proc` serialization (XDR-style, endianness-safe),
 * NOT a C-struct memcpy: the executor is non-SPDK and may differ in ABI. Every
 * field is encoded explicitly (opaque/string/optional-bulk), so the in-memory
 * struct layout is irrelevant to the wire — only the encode/decode order is.
 *
 * The ONLY SPDK dependency this contract pulls in is <spdk/kvdev.h>, for the
 * `enum spdk_kvdev_io_status` codes that the wire status mirrors (§3). That
 * keeps the standalone executor (Slice C2) able to include this header without
 * dragging in the SPDK reactor/event libraries.
 *
 * BUILD INVARIANT (design §3, NORMATIVE): the executor MUST be built against the
 * IDENTICAL <spdk/kvdev.h> as the front (same `enum spdk_kvdev_io_status`
 * definition, same numeric values). The wire status is carried as a fixed int32
 * whose value-stability is a BUILD/PACKAGING invariant of the two-tier
 * deployment, NOT an ABI guarantee of the wire format. A front and executor
 * built from divergent kvdev.h are an UNSUPPORTED configuration. The
 * SPDK_STATIC_ASSERTs in nkvx_exec_rpc.c pin every enum value so that an
 * enum-value bump breaks the BUILD (assert fires) instead of silently re-mapping
 * a tenant CQE.
 */

#ifndef NKVX_EXEC_RPC_H
#define NKVX_EXEC_RPC_H

#include <mercury.h>
#include <mercury_proc.h>
#include <mercury_proc_string.h>
#include <mercury_proc_bulk.h>

#include "spdk/kvdev.h"		/* enum spdk_kvdev_io_status, SPDK_KVDEV_EXEC_KEY_MAX_LEN, SHA256 len */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Inline-vs-bulk threshold (design §1.1, OQ-1: 4 KiB proposed). Inputs and
 * results at or below this travel inline in the Mercury SEND/response; larger
 * payloads use a front-origin bulk RMA handle (input PULL / result-sink WRITE).
 *
 * The threshold compares against `min(result_len, osize)` for results (only the
 * bytes that will actually be delivered) and against `input_len` for inputs.
 */
#define NKVX_INLINE_MAX (4u * 1024u)

/**
 * Inter-tier KV verb discriminant (slice N1, migration §10.1 / ADR-0008 D1/D4).
 *
 * Under the forwarder pivot the front (`bdev_kvrados`) owns no librados and
 * forwards EVERY KV op to the executor, which owns librados + the TB4 cache.
 * The five data verbs below ride a SECOND, additive Mercury RPC (`nkvx_kv`,
 * registered by name like `nkvx_cancel`), distinct from the frozen `nkvx_exec`
 * (0x83) contract so this extension does NOT perturb the existing Exec wire id
 * or envelope. Exec keeps its own dedicated RPC and is intentionally NOT a
 * member of this enum (it carries module-locator/sha256/caps fields the base
 * verbs never need — ADR-0008 D4: "no verb is silently both base-and-Exec").
 *
 * Values are explicit and pinned by SPDK_STATIC_ASSERT in nkvx_exec_rpc.c so a
 * reorder breaks the BUILD (a misrouted verb is a security-relevant event under
 * the verb-based gate, ADR-0008 D1: an unclassified verb is treated as
 * mutating, never silently allowed). 0 is reserved as "unset/invalid" so a
 * zero-initialized request never decodes as a valid verb.
 */
enum nkvx_kv_verb {
	NKVX_KV_VERB_INVALID  = 0,	/* reserved: zero-init must not be a valid verb */
	NKVX_KV_VERB_STORE    = 1,	/* mutating: value in (inline or PULL); status out */
	NKVX_KV_VERB_RETRIEVE = 2,	/* non-mutating: value out (inline or PUSH sink) */
	NKVX_KV_VERB_DELETE   = 3,	/* mutating: status only, no payload */
	NKVX_KV_VERB_EXIST    = 4,	/* non-mutating: status only, no payload */
	NKVX_KV_VERB_LIST     = 5,	/* non-mutating: key listing out (inline or PUSH sink) */
};

/**
 * Inter-tier KV-verb REQUEST envelope (slice N1). Shared, like nkvx_exec_in_t,
 * between the front and the (non-SPDK) executor; encoded field-by-field via
 * hg_proc (NOT a struct memcpy), so in-memory layout is irrelevant to the wire.
 *
 * Payload routing reuses the EXACT inline-vs-bulk envelope mechanics of Exec
 * (NKVX_INLINE_MAX threshold, input PULL / result-sink PUSH):
 *
 *   - Store    ships the VALUE as the request "input": carried inline in
 *              `value_inline` when value_len <= NKVX_INLINE_MAX, else delivered
 *              out of band via `value_bulk` (executor PULLs it). No result sink.
 *   - Retrieve returns the object via the response: inline in nkvx_kv_out_t
 *              when min(result_len, osize) <= NKVX_INLINE_MAX and no sink was
 *              supplied, else the executor PUSHes it into `result_sink`
 *              (the front's pre-registered host DPTR, WRITE-mode bulk).
 *   - List     returns the NRK-prefixed key-listing structure the same way as
 *              Retrieve (inline-or-PUSH), bounded by `osize`; `key` carries the
 *              start-position key (start_key_len == key_len; 0 => from the
 *              beginning, ADR/spec §2.1.6.2 stable order).
 *   - Delete / Exist carry no value and no sink: only status (and, for Exist,
 *              the stored value length echoed in result_len) come back.
 *
 * The status-mapping discipline is identical: nkvx_kv_out_t.status is a fixed
 * int32 enum spdk_kvdev_io_status run through nkvx_status_from_wire().
 */
typedef struct {
	/** One of enum nkvx_kv_verb (NEVER NKVX_KV_VERB_INVALID on a live request). */
	uint8_t		verb;

	/**
	 * Read-only invariant (ADR-0008 D1/D3/D5): the front passes the per-op
	 * read_only bit; the executor enforces it AUTHORITATIVELY at its mutation
	 * point (rejecting Store/Delete on a read-only namespace with the wire
	 * analogue of SPDK_KVDEV_IO_STATUS_READ_ONLY). The front MAY pre-reject as
	 * a fast path, but this field travels regardless so the executor — the
	 * unbypassable boundary — can enforce even for a direct RPC client.
	 */
	uint8_t		read_only;

	/** Front-unique handshake token, mirrors nkvx_exec_in_t.client_call_id;
	 *  names exactly one in-flight op for cancel/telemetry. 0 = unset. */
	uint64_t	client_call_id;

	/**
	 * Key length, 1..SPDK_KVDEV_EXEC_KEY_MAX_LEN (255). For List this is the
	 * start-position key length (0 => begin at the first key). For Store/
	 * Retrieve/Delete/Exist it is the object key length (must be >= 1).
	 */
	uint8_t		key_len;
	/** Key bytes (the RADOS oid identity / List start position). key_len bytes. */
	uint8_t		key[SPDK_KVDEV_EXEC_KEY_MAX_LEN];

	/**
	 * Host output-buffer cap (tenant CDW12, clamped). Drives result truncation
	 * for Retrieve/List exactly like nkvx_exec_in_t.osize. Unused (0) for
	 * Store/Delete/Exist.
	 */
	uint32_t	osize;

	/**
	 * STORE conditional options (slice S4). Carries the SIKE/SINKE store-
	 * conditional flags the front decoded from CDW11 `ro` (mapped to
	 * enum spdk_kvdev_store_flags: SIKE = 1<<0, SINKE = 1<<1). The executor
	 * applies them atomically with the librados write (create-exclusive for
	 * SINKE, assert-exists for SIKE). 0 (NONE) for the read verbs and an
	 * unconditional Store. Additive field on the (unfrozen) nkvx_kv RPC.
	 */
	uint8_t		store_flags;

	/**
	 * STORE value: length of the value to store. When value_len <=
	 * NKVX_INLINE_MAX the bytes ride inline in `value_inline`; otherwise they
	 * are delivered via `value_bulk` (executor PULL). 0 for the read verbs.
	 */
	uint32_t	value_len;
	/**
	 * Inline STORE value bytes. Valid (value_len bytes) iff
	 * value_len <= NKVX_INLINE_MAX. Owned by the caller on encode; malloc'd on
	 * decode and freed by nkvx_kv_in_free().
	 */
	void		*value_inline;
	/**
	 * Optional large-value RMA handle (Store only). Present (non-HG_BULK_NULL)
	 * iff value_len > NKVX_INLINE_MAX; the executor issues
	 * HG_Bulk_transfer(PULL) to read it. Mirrors nkvx_exec_in_t.input_bulk.
	 */
	hg_bulk_t	value_bulk;

	/**
	 * Optional large-result sink handle (Retrieve/List). When present
	 * (non-HG_BULK_NULL) the front pre-registered its host output buffer as a
	 * WRITE-mode bulk and the executor PUSHes the retrieved value / key listing
	 * straight into it (then nkvx_kv_out_t.result_inline is empty). When
	 * HG_BULK_NULL the executor inlines the result. Mirrors
	 * nkvx_exec_in_t.result_sink.
	 */
	hg_bulk_t	result_sink;
} nkvx_kv_in_t;

/**
 * Inter-tier KV-verb RESPONSE envelope (slice N1). Mirrors nkvx_exec_out_t.
 */
typedef struct {
	/** enum spdk_kvdev_io_status as a fixed int32 (run through
	 *  nkvx_status_from_wire() by the front). Same discipline as Exec. */
	int32_t		status;

	/**
	 * TRUE result length (truncation semantics, identical to Exec):
	 *   - Retrieve: the full stored value length (CQE DW0), even when truncated
	 *     to osize (status BUFFER_TOO_SMALL when result_len > osize).
	 *   - List:     the full byte length of the NRK-prefixed listing.
	 *   - Exist:    the stored value length (echoed; status SUCCESS/KEY_NOT_EXIST).
	 *   - Store/Delete: 0 (unused).
	 */
	uint32_t	result_len;

	/**
	 * Inline result bytes (Retrieve value / List listing). Present iff
	 * min(result_len, osize) <= NKVX_INLINE_MAX AND no result_sink was supplied;
	 * result_inline_len is the delivered length in that case, else 0 (delivered
	 * via the WRITE-bulk sink). Owned by the caller on encode; malloc'd on
	 * decode and freed by nkvx_kv_out_free().
	 */
	uint32_t	result_inline_len;
	void		*result_inline;
} nkvx_kv_out_t;

/**
 * Exec RPC REQUEST envelope (design §1.1, `nkvx_exec_in_t`).
 *
 * Field roles mirror the front's existing `struct spdk_kv_exec_binding`
 * (kvdev.h) plus the per-op arguments the front passes into kvdev_rados_exec()
 * (op_id, read_only, key, input, output-buffer cap). All inline fields ride in
 * the Mercury SEND; the object bytes are NEVER carried here (the executor
 * cold-fills them, design §2).
 */
typedef struct {
	/** Tenant CDW13; opaque to the executor, for tracing/telemetry only. */
	uint32_t	op_id;

	/**
	 * Front-assigned, per-front-UNIQUE handshake token (Slice C6b). op_id is the
	 * tenant CDW13 and is NOT unique among a front's concurrent in-flight Execs, so
	 * it cannot key the executor's cancel registry (two same-op_id Execs would alias
	 * and a cancel could abort the wrong one / ack the other early — a UAF). This
	 * monotonic per-front id names exactly one in-flight Exec. 0 = unset/none.
	 */
	uint64_t	client_call_id;

	/**
	 * Read-only invariant (design OQ-5): the front passes the per-op
	 * read_only bit; the executor enforces it at its mutation point.
	 */
	uint8_t		read_only;

	/** Module runtime kind; mirrors enum spdk_kv_exec_runtime (NKVX=1, CLS=2).
	 *  NKVX + module_ns "nkvx" => built-in native; NKVX + other ns => wasm. */
	uint8_t		runtime;

	/**
	 * Per-invocation capability TIER selector, [0, SPDK_KV_EXEC_CAPS_TIER_MAX].
	 * Carried as the binding's full uint64 caps word.
	 */
	uint64_t	caps;

	/** Key length, 1..SPDK_KVDEV_EXEC_KEY_MAX_LEN (255). */
	uint8_t		key_len;
	/** Key bytes (the RADOS oid identity). Only key_len bytes are encoded. */
	uint8_t		key[SPDK_KVDEV_EXEC_KEY_MAX_LEN];

	/** Artifact content hash; the SOLE auth/integrity anchor (ADR-0010). */
	uint8_t		sha256[SPDK_KV_EXEC_SHA256_LEN];
	/** True when sha256 carries a real hash (false on the cls path and on
	 *  built-in native NKVX modules; true only for cold-fetch wasm). */
	uint8_t		sha256_valid;

	/**
	 * Cold-fetch locator key (module object name / cls method). NUL-terminated,
	 * may be NULL/empty. Owned by the caller on encode; malloc'd on decode and
	 * freed by nkvx_exec_in_free().
	 */
	char		*module_key;
	/**
	 * Cold-fetch locator namespace (pool or pool/namespace). NUL-terminated,
	 * may be NULL/empty. Owned/freed same as module_key.
	 */
	char		*module_ns;

	/**
	 * Host output-buffer cap (tenant CDW12, clamped) == the front's
	 * output_buf_len. Drives result truncation (design §1.2).
	 */
	uint32_t	osize;

	/**
	 * Length of the per-request input. When input_len <= NKVX_INLINE_MAX the
	 * bytes are carried inline in `input_inline`; otherwise they are delivered
	 * out of band via `input_bulk` (PULL, executor-side; transfer is Slice C7).
	 */
	uint32_t	input_len;
	/**
	 * Inline input bytes. Valid (input_len bytes) iff input_len <= NKVX_INLINE_MAX.
	 * Owned by the caller on encode; malloc'd on decode and freed by
	 * nkvx_exec_in_free().
	 */
	void		*input_inline;

	/**
	 * Optional large-input RMA handle (design §1.3). Present (non-HG_BULK_NULL)
	 * iff input_len > NKVX_INLINE_MAX. The executor issues HG_Bulk_transfer(PULL)
	 * to read it. The actual transfer is Slice C7; here it is contract-only.
	 */
	hg_bulk_t	input_bulk;

	/**
	 * Optional large-result sink handle (design §1.3, `result_sink`). When
	 * present (non-HG_BULK_NULL), the front has pre-registered its host output
	 * buffer (the tenant DPTR) as a WRITE-mode bulk handle and the executor
	 * PUSHes the result straight into it (then `result_inline` in the response
	 * is empty). When HG_BULK_NULL, the executor inlines the result. The actual
	 * transfer is Slice C7; here it is contract-only.
	 */
	hg_bulk_t	result_sink;
} nkvx_exec_in_t;

/**
 * Exec RPC RESPONSE envelope (design §1.2, `nkvx_exec_out_t`).
 */
typedef struct {
	/**
	 * Result status: an `enum spdk_kvdev_io_status` value carried on the wire
	 * as a FIXED int32 via hg_proc (endianness/width-safe). The front runs it
	 * through nkvx_status_from_wire() before feeding nvmf_kvdev_complete().
	 */
	int32_t		status;

	/**
	 * The TRUE result length (Retrieve-style truncation, ADR-0014/design §1.2):
	 * always the full length the module produced, even when truncated to osize.
	 * The front sets tenant CQE DW0 to this. When result_len > osize the front
	 * maps status BUFFER_TOO_SMALL exactly as the tenant edge does.
	 */
	uint32_t	result_len;

	/**
	 * Inline result bytes. Present iff min(result_len, osize) <= NKVX_INLINE_MAX
	 * AND the request carried no result_sink. The number of inline bytes encoded
	 * is `result_inline_len` == min(result_len, osize) in that case, else 0
	 * (large result was/will be delivered via the result_sink WRITE bulk).
	 * Owned by the caller on encode; malloc'd on decode, freed by
	 * nkvx_exec_out_free().
	 */
	uint32_t	result_inline_len;
	void		*result_inline;
} nkvx_exec_out_t;

/**
 * Exec CANCEL RPC (Slice C6b, bead spdk-5ia; design §C6b). A SECOND, additive
 * Mercury RPC the front forwards to the executor to make the cross-process abort
 * use-after-free-safe: the executor sets do-not-PUSH / HG_Bulk_cancels any
 * in-flight result PUSH and acks ONLY once its remote bulk view is quiescent; the
 * front defers releasing the result_sink MR / tenant DPTR until that ack. The new
 * RPC is registered by name ("nkvx_cancel"), so its name-hashed id does NOT
 * perturb the frozen nkvx_exec contract — this is a freeze-respecting EXTENSION,
 * not a change to nkvx_exec_in_t (HITL nod).
 *
 * Registry key on the executor is (origin_addr, op_id), NOT op_id alone: op_id is
 * the tenant CDW13 and is not unique across fronts; a single executor serves
 * several. The origin address rides implicitly via HG_Get_info(handle)->addr, so
 * ONLY op_id is on the wire — deliberately avoiding a new field on the frozen
 * request envelope.
 */
typedef struct {
	/** The front-unique client_call_id of the in-flight nkvx_exec to cancel
	 *  (matches nkvx_exec_in_t.client_call_id — NOT op_id, which is not unique). */
	uint64_t	call_id;
} nkvx_cancel_in_t;

/**
 * Cancel ack code (TELEMETRY only). All three values mean the same thing for
 * SAFETY — "the executor's remote view of result_sink is gone" — so the front
 * never branches on the value; the DELIVERED ack is itself the proof. The value
 * only lets the executor log / tests assert which cancel case (a/b/c) was taken.
 */
enum nkvx_cancel_ack {
	NKVX_CANCEL_ALREADY_DONE = 0,	/* (a) op not found: already finished / unknown / dup */
	NKVX_CANCEL_ABORTED      = 1,	/* (b) found, no PUSH in flight: do-not-PUSH honored */
	NKVX_CANCEL_PUSH_CANCELED = 2,	/* (c) found, PUSH in flight: HG_Bulk_cancel'd */
};

/** Cancel RPC RESPONSE: a fixed int32 ack (one of enum nkvx_cancel_ack), endian-safe. */
typedef struct {
	int32_t		ack;
} nkvx_cancel_out_t;

/*
 * hg_proc serializers (design §1.2 / C3). One routine per envelope; each
 * encodes/decodes every field explicitly. Usable as the proc callback in
 * HG_Register() and directly in a manual hg_proc_create_set() round-trip.
 *
 * Return HG_SUCCESS or a Mercury error. On HG_DECODE these allocate
 * module_key/module_ns/input_inline/result_inline; call the matching _free()
 * (which is also the HG_FREE path of the proc) to release them.
 */
hg_return_t hg_proc_nkvx_exec_in_t(hg_proc_t proc, void *data);
hg_return_t hg_proc_nkvx_exec_out_t(hg_proc_t proc, void *data);

/*
 * KV-verb RPC serializers (slice N1). Same contract as the Exec procs: every
 * field explicit, strings/opaque-bytes allocated on HG_DECODE and released by
 * the HG_FREE path / the matching _free() helper.
 */
hg_return_t hg_proc_nkvx_kv_in_t(hg_proc_t proc, void *data);
hg_return_t hg_proc_nkvx_kv_out_t(hg_proc_t proc, void *data);

/*
 * Cancel RPC serializers (Slice C6b). Each is a single fixed-width scalar
 * (uint32 op_id / int32 ack), so there is nothing to allocate on decode and
 * therefore no matching _free() helper.
 */
hg_return_t hg_proc_nkvx_cancel_in_t(hg_proc_t proc, void *data);
hg_return_t hg_proc_nkvx_cancel_out_t(hg_proc_t proc, void *data);

/** Release heap allocations a decoded request holds (strings + inline input). */
void nkvx_exec_in_free(nkvx_exec_in_t *in);
/** Release heap allocations a decoded response holds (inline result). */
void nkvx_exec_out_free(nkvx_exec_out_t *out);

/** Release heap allocations a decoded KV-verb request holds (inline value). */
void nkvx_kv_in_free(nkvx_kv_in_t *in);
/** Release heap allocations a decoded KV-verb response holds (inline result). */
void nkvx_kv_out_free(nkvx_kv_out_t *out);

/*
 * Wire-status mapping (design §3). The wire status is a fixed int32; these map
 * between the wire value and `enum spdk_kvdev_io_status`. The front does NOT
 * blindly cast the incoming int32 — it runs it through nkvx_status_from_wire(),
 * which returns FAILED for any value outside the known set (a transport-level
 * fault synthesized to a retryable device error, design §3).
 */

/** Map an enum spdk_kvdev_io_status to its fixed int32 wire value. */
int32_t nkvx_status_to_wire(enum spdk_kvdev_io_status status);

/**
 * Map a fixed int32 wire value back to an enum spdk_kvdev_io_status. An
 * unrecognized wire value (outside the 10 known codes) maps to
 * SPDK_KVDEV_IO_STATUS_FAILED (design §3: never cast an unknown value through).
 */
enum spdk_kvdev_io_status nkvx_status_from_wire(int32_t wire);

#ifdef __cplusplus
}
#endif

#endif /* NKVX_EXEC_RPC_H */
