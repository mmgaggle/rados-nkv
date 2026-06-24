/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/**
 * \file
 * Key-Value Device (kvdev) public interface.
 *
 * kvdev is an object-shaped storage abstraction that is a sibling to the
 * block device (bdev) layer.  Where bdev models a linear array of fixed-size
 * blocks, kvdev models a mapping from a short binary key (1-16 bytes) to a
 * variable-length value.  It mirrors bdev's *runtime model* (module
 * registration via a constructor macro, descriptors for open/close, per-thread
 * spdk_io_channels and async completions) while exposing an object-shaped op
 * set.
 */

#ifndef SPDK_KVDEV_H
#define SPDK_KVDEV_H

#include "spdk/stdinc.h"
#include "spdk/assert.h"
#include "spdk/queue.h"
#include "spdk/uuid.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Minimum length of a key in bytes. */
#define SPDK_KVDEV_KEY_MIN_LEN 1

/** Maximum length of a key in bytes (per the NVMe Key-Value Command Set Specification). */
#define SPDK_KVDEV_KEY_MAX_LEN 16

/*
 * Maximum KV Exec data-object key length in bytes (ADR-0014). KV Exec carries
 * its key length-prefixed in the DPTR payload, NOT in the spec's 16-byte inline
 * CDW slots, so it spans both 32-byte content hashes and RADOS object names up
 * to 255 bytes. This bound applies ONLY to the Exec key; Store/Retrieve/etc.
 * remain capped at SPDK_KVDEV_KEY_MAX_LEN.
 */
#define SPDK_KVDEV_EXEC_KEY_MAX_LEN 255

/*
 * Minimum length (bytes) of a KV Exec dma-buf result_sink (B-i V3, bead
 * spdk-avu). A two-tier backend returns a result that fits within the RPC inline
 * cap WITHOUT registering a bulk MR; for an ordinary host-VA sink the front then
 * copies the inline bytes into host_out. The dma-buf path has NO host_out
 * fallback -- the result must be RDMA-WRITTEN into device memory via a
 * registered bulk -- so a sink whose declared length is <= this threshold would
 * ride inline, be DROPPED by the front, yet still complete SUCCESS: silent data
 * loss with the guest believing VRAM was written. sink_len is guest-controlled,
 * so the NVMf KV Exec dispatch MUST reject a dma-buf sink at or below this bound
 * with a clean NVMe error rather than dispatch it. The value mirrors the
 * module-internal inline cap (NKVX_INLINE_MAX == 4096); a dma-buf sink must be
 * STRICTLY LARGER (i.e. length > SPDK_KVDEV_DMABUF_SINK_MIN_LEN) to force a bulk
 * registration. Kept in the public header so lib/nvmf need not depend on a
 * backend-private constant.
 */
#define SPDK_KVDEV_DMABUF_SINK_MIN_LEN 4096

/*
 * KV Exec (0x83) wire ABI — our vendor extension to the NVMe Key-Value command
 * set (ADR-0005/0014). Unmodified upstream SPDK ships the five STANDARD KV
 * opcodes (Store 0x01, Retrieve 0x02, List 0x06, Delete 0x10, Exist 0x14) in
 * <spdk/nvme_spec.h>, but NOT KV Exec and NOT the cdw12/cdw13 union members that
 * name its fields. This header fills exactly that gap so the out-of-tree
 * bdev_kvrados forwarder builds against UNMODIFIED SPDK: do NOT add this opcode
 * or these fields to any upstream header.
 *
 * Command layout (matching the in-tree fork's union spdk_nvme_cmd_cdw12/cdw13):
 *   - CDW12 = osize: 32  (host output buffer cap, bytes; full 32-bit field)
 *   - CDW13 = op_id: 32  (operation identifier selecting the server-side op;
 *                         full 32-bit field — the data-object key rides
 *                         length-prefixed in the DPTR payload, NOT the inline
 *                         CDW slots, per ADR-0014)
 * Because each field occupies the whole dword, decode is a straight read of the
 * RAW cmd->cdw12 / cmd->cdw13 uint32 values. We deliberately do NOT redefine the
 * spec's cdw12_bits/cdw13_bits unions (those belong to <spdk/nvme_spec.h>);
 * instead spdk_kv_exec_decode() pulls op_id/osize from the raw dwords so callers
 * need no upstream-header change.
 */
#define SPDK_NVME_OPC_KV_EXEC 0x83

/** Decoded KV Exec (0x83) command-specific fields (from raw CDW12/CDW13). */
struct spdk_kv_exec_cmd {
	uint32_t op_id;	/**< CDW13: operation identifier (server-side op selector). */
	uint32_t osize;	/**< CDW12: host output buffer cap in bytes. */
};

/**
 * Decode a KV Exec command's op_id/osize from the raw CDW12/CDW13 dwords.
 *
 * \param cdw12 Raw value of NVMe command dword 12 (osize).
 * \param cdw13 Raw value of NVMe command dword 13 (op_id).
 * \return The decoded fields.
 */
static inline struct spdk_kv_exec_cmd
spdk_kv_exec_decode(uint32_t cdw12, uint32_t cdw13)
{
	struct spdk_kv_exec_cmd out;

	out.osize = cdw12;	/* CDW12 = osize:32 (full dword) */
	out.op_id = cdw13;	/* CDW13 = op_id:32 (full dword) */
	return out;
}

struct spdk_kvdev;
struct spdk_kvdev_desc;
struct spdk_kvdev_module;

/** Length of an artifact content hash (SHA-256) in bytes. */
#define SPDK_KV_EXEC_SHA256_LEN 32

/**
 * Per-invocation capability TIER encoding for a binding's \c caps word (ADR-0014).
 * \c caps is NOT a free-form fuel/byte count: it names a fixed capability tier whose
 * fuel/epoch/memory triple is defined by the executor (rados-nkvx). Valid words are
 * the small set [0, SPDK_KV_EXEC_CAPS_TIER_MAX]:
 *   - 0: executor defaults (env-overridable) -- the v1/reserved value.
 *   - 1: SMALL  (tight: short compute, small memory).
 *   - 2: MEDIUM (default-equivalent fixed triple).
 *   - 3: LARGE  (generous: long compute, large memory).
 * The control plane MUST reject any other value; a raw byte/fuel count would be
 * silently truncated to a tier by the executor and is therefore not accepted here.
 */
#define SPDK_KV_EXEC_CAPS_TIER_MAX 3

/**
 * KV Exec module runtime kind (ADR-0012, runtime-agnostic binding). Selects the
 * backend that executes a bound module.
 */
enum spdk_kv_exec_runtime {
	/** Unset/invalid. */
	SPDK_KV_EXEC_RUNTIME_NONE	= 0,
	/**
	 * The rados-nkvx executor. Backend for BOTH execution flavors, distinguished
	 * by module_namespace:
	 *   - module_namespace == "nkvx": a BUILT-IN NATIVE module compiled into the
	 *     executor (e.g. bytecount); selected by module_key, sha256 anchor unused.
	 *   - module_namespace != "nkvx": a sandboxed wasm module cold-fetched from
	 *     RADOS via (module_namespace, module_key) and authorized by the sha256
	 *     content hash (ADR-0010 gate).
	 * Value 1 is unchanged from the former WASM kind, so the wasm wire value is
	 * preserved; only built-in modules moved here off CLS.
	 */
	SPDK_KV_EXEC_RUNTIME_NKVX	= 1,
	/**
	 * Genuine Ceph object-class (cls) method, executed in the OSD. Preserved for
	 * the in-tree "class:method" path; (module_namespace, module_key) carry the
	 * (class, method) pair and the sha256 anchor is unused. This is the only
	 * real-cls runtime; built-in native modules are NOT cls (see NKVX).
	 */
	SPDK_KV_EXEC_RUNTIME_CLS	= 2,
};

/**
 * Structured KV Exec binding (ADR-0010 / ADR-0012 / ADR-0014).
 *
 * Replaces the prototype's opaque "class:method" string. Resolved from the
 * per-namespace allowlist by the NVMf control plane and threaded into the
 * backend exec op. Field roles:
 *   - \c runtime selects the executor backend.
 *   - \c sha256 / \c sha256_valid is the SOLE authorization + integrity anchor
 *     (ADR-0010) for cold-fetched wasm modules (runtime NKVX, non-"nkvx"
 *     namespace): a fetched artifact whose content hash does not match is
 *     rejected. \c sha256_valid is false on the cls path and on built-in
 *     native modules (runtime NKVX, "nkvx" namespace), which carry no artifact.
 *   - (\c module_namespace, \c module_key) is the cold-fetch locator, consulted
 *     ONLY on a content-cache miss; it never participates in authorization.
 *   - \c caps selects a per-invocation capability TIER (not a raw count); valid
 *     values are [0, SPDK_KV_EXEC_CAPS_TIER_MAX], with 0 meaning executor defaults.
 *
 * Strings are NUL-terminated and owned by the caller (the namespace allowlist);
 * a backend must copy anything it needs to outlive the call.
 */
struct spdk_kv_exec_binding {
	enum spdk_kv_exec_runtime	runtime;
	/** Cold-fetch locator namespace (e.g. RADOS pool/namespace). May be NULL. */
	const char			*module_namespace;
	/** Cold-fetch locator key (e.g. module object name / cls method). May be NULL. */
	const char			*module_key;
	/** Artifact content hash; the auth/integrity anchor (ADR-0010). */
	uint8_t				sha256[SPDK_KV_EXEC_SHA256_LEN];
	/** True when sha256 carries a real hash (false on the cls path and on
	 *  built-in native NKVX modules; true only for cold-fetched wasm). */
	bool				sha256_valid;
	/** Per-invocation capability TIER selector, [0, SPDK_KV_EXEC_CAPS_TIER_MAX]. */
	uint64_t			caps;
};

/**
 * Status codes returned via kvdev completions.  These are deliberately a small
 * device-neutral set; the NVMf layer translates them into NVMe status codes.
 */
enum spdk_kvdev_io_status {
	SPDK_KVDEV_IO_STATUS_SUCCESS		= 0,
	/** Generic failure. */
	SPDK_KVDEV_IO_STATUS_FAILED		= -1,
	/** The requested key does not exist. */
	SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST	= -2,
	/** The key already exists (e.g. a Store-If-No-Key-Exists conflict). */
	SPDK_KVDEV_IO_STATUS_KEY_EXIST		= -6,
	/** The provided buffer was too small to hold the value. */
	SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL	= -3,
	/** The request was malformed (bad key length, etc.). */
	SPDK_KVDEV_IO_STATUS_INVALID		= -4,
	/** Out of memory / capacity. */
	SPDK_KVDEV_IO_STATUS_NOMEM		= -5,
	/**
	 * The backend does not support this operation (e.g. the librados module
	 * defers List per ADR-0002). The NVMf layer maps this to the NVMe
	 * "command not supported"/invalid-opcode status.
	 */
	SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED	= -7,
	/**
	 * The operation was aborted because it exceeded a per-invocation resource
	 * cap (a KV Exec module that exhausted its fuel budget, blew past its
	 * wall-clock epoch deadline, or tried to grow linear memory past its cap).
	 * This is a CONTAINED kill, never a crash/hang of the target. The NVMf
	 * layer maps it to the NVMe "command aborted" status.
	 */
	SPDK_KVDEV_IO_STATUS_ABORTED		= -8,
	/**
	 * A mutating operation was attempted on a read-only namespace. KV Exec is
	 * permitted on a read-only namespace because the executor is read-only by
	 * contract (ADR-0014), but that invariant is ENFORCED at the mutation
	 * point: a backend that would write (e.g. a mutating built-in or a write
	 * runtime/module) rejects the op with this status when \c read_only is set,
	 * rather than the gate ASSUMING no Exec mutates. The NVMf layer maps it to
	 * Command-Specific "Attempted Write to Read Only Range".
	 */
	SPDK_KVDEV_IO_STATUS_READ_ONLY		= -9,
};

/**
 * kvdev I/O completion callback.
 *
 * \param cb_arg Context passed to the originating op.
 * \param status One of enum spdk_kvdev_io_status.
 * \param value_len For retrieve, the actual length of the stored value (which
 *                  may exceed the supplied buffer; see status). Ignored for
 *                  store.
 */
typedef void (*spdk_kvdev_io_completion_cb)(void *cb_arg, int status, uint32_t value_len);

/**
 * kvdev List iteration callback. Invoked once per key the backend visits, in
 * the backend's stable iteration order, until the consumer asks it to stop.
 *
 * \param cb_arg Context passed to the originating list op.
 * \param key Key bytes for this entry (valid only for the duration of the call).
 * \param key_len Length of \c key in bytes (in [1,16]).
 *
 * \return true to continue iterating to the next key, false to stop early (for
 *         example, when the next key would not fit in the host buffer).
 */
typedef bool (*spdk_kvdev_list_cb)(void *cb_arg, const void *key, uint8_t key_len);

/**
 * kvdev List completion callback. Fired once after iteration finishes (either
 * the backend ran out of keys or the per-key callback asked it to stop).
 *
 * \param cb_arg Context passed to the originating list op.
 * \param status One of enum spdk_kvdev_io_status (SUCCESS unless the request
 *               could not be serviced).
 * \param num_keys Number of keys the per-key callback accepted (i.e. the count
 *                 for which spdk_kvdev_list_cb returned and the key was emitted).
 */
typedef void (*spdk_kvdev_list_done_cb)(void *cb_arg, int status, uint32_t num_keys);

/**
 * Store conditional flags. These mirror the NVMe KV Store command "Store
 * Option" bits (CDW11 bits 8/9 in the SQE) so the NVMf layer can pass the
 * host's intent straight through to the backend.
 */
enum spdk_kvdev_store_flags {
	/** Default behaviour: insert or overwrite unconditionally. */
	SPDK_KVDEV_STORE_FLAG_NONE	= 0,
	/**
	 * Store-If-Key-Exists (SIKE). Only store when the key already exists;
	 * otherwise fail with SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST.
	 */
	SPDK_KVDEV_STORE_FLAG_SIKE	= 1u << 0,
	/**
	 * Store-If-No-Key-Exists (SINKE). Only store when the key does not yet
	 * exist; otherwise fail with SPDK_KVDEV_IO_STATUS_KEY_EXIST.
	 */
	SPDK_KVDEV_STORE_FLAG_SINKE	= 1u << 1,
	/**
	 * TTL valid (vendor extension, ADR-0003). When set, opts->ttl carries a
	 * time-to-live in seconds. The backend persists it as an absolute deadline
	 * and ENFORCES expiry lazily: once the deadline passes, the key is treated
	 * as absent (Retrieve/Exist report KEY_NOT_EXIST and List omits it) and the
	 * entry is dropped on the next access. A ttl of 0 with this flag set means
	 * "expires immediately". When clear, opts->ttl is ignored. (spdk-jhk.7.14)
	 */
	SPDK_KVDEV_STORE_F_TTL		= 1u << 2,
	/**
	 * Ephemeral (vendor extension, spdk-jhk.7.14). Marks the entry as
	 * non-durable: it carries no durability guarantee and is excluded from any
	 * durable-config snapshot the backend may emit. For the in-memory backend
	 * (which is wholly volatile) this is a recorded, introspectable property
	 * rather than a behaviour change. When clear, the entry is a normal entry.
	 */
	SPDK_KVDEV_STORE_F_EPHEMERAL	= 1u << 3,
	/**
	 * Touch (vendor extension, spdk-jhk.7.14). Refresh an EXISTING key's TTL
	 * and last-access time WITHOUT changing its value: the store's value payload
	 * is ignored and the stored value is left intact. If the key does not exist
	 * the store fails with SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST. When
	 * SPDK_KVDEV_STORE_F_TTL is also set, opts->ttl becomes the new TTL;
	 * otherwise any existing TTL is cleared (the entry becomes non-expiring).
	 */
	SPDK_KVDEV_STORE_F_TOUCH	= 1u << 4,
};

/**
 * Extensible options for a Store operation.
 *
 * The struct is versioned by its leading \c size field: callers set \c size to
 * sizeof(struct spdk_kvdev_store_opts) and backends must only read fields that
 * fall within the supplied size. This lets later slices append members without
 * breaking the store ABI. Always populate via spdk_kvdev_store_opts_init()
 * before setting fields.
 */
struct spdk_kvdev_store_opts {
	/** Size of this structure as known to the caller. Must be set first. */
	size_t		size;
	/** Bitmask of enum spdk_kvdev_store_flags. */
	uint32_t	flags;

	/*
	 * Time-to-live in seconds (vendor extension, ADR-0003). Honoured only
	 * when SPDK_KVDEV_STORE_F_TTL is set in \c flags; otherwise ignored.
	 * The backend persists this as an absolute deadline and enforces expiry
	 * lazily (spdk-jhk.7.14). Future slices APPEND fields below this line
	 * (never reorder/remove existing ones); the \c size field handles
	 * versioning.
	 */
	uint32_t	ttl;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_kvdev_store_opts) == 16, "Incorrect size");

/**
 * Initialize a store options struct to defaults. \c size is typically
 * sizeof(struct spdk_kvdev_store_opts).
 */
static inline void
spdk_kvdev_store_opts_init(struct spdk_kvdev_store_opts *opts, size_t size)
{
	memset(opts, 0, size);
	opts->size = size;
}

/**
 * Advertised capabilities/limits of a kvdev. Used by the NVMf layer to build
 * KV Identify namespace data (kvkml/kvvml/mnks).
 */
struct spdk_kvdev_caps {
	/** Maximum key length in bytes (<= SPDK_KVDEV_KEY_MAX_LEN). */
	uint16_t	max_key_len;
	/** Maximum value length in bytes. */
	uint32_t	max_value_len;
	/** Maximum number of keys (0 = no limit advertised). */
	uint32_t	max_num_keys;
};

/**
 * The function table that a kvdev module supplies for each kvdev it creates.
 *
 * All I/O ops are asynchronous: they queue/perform the work and invoke
 * cb_fn(cb_arg, status, value_len) on the calling thread's io_channel context.
 * Ops return 0 if the request was accepted (the completion will fire) or a
 * negative errno if it could not even be submitted (no completion fires).
 *
 * The vtable is intentionally op-per-verb so later slices (delete/exist/list)
 * slot in as additional members without disturbing the store/retrieve ABI.
 */
struct spdk_kvdev_fn_table {
	/** Tear down the kvdev. Called from spdk_kvdev_unregister context. */
	int (*destruct)(void *ctx);

	/** Return a per-thread io_channel for this kvdev. */
	struct spdk_io_channel *(*get_io_channel)(void *ctx);

	/**
	 * Store (insert or overwrite) a value under a key.
	 *
	 * \param ch io_channel obtained from get_io_channel().
	 * \param key Key bytes (key_len in [1,16]).
	 * \param value Value bytes to store (value_len bytes).
	 * \param opts Store options (flags such as SIKE/SINKE, future TTL). May be
	 *             NULL for default (unconditional) behaviour. The backend must
	 *             honour opts->size and ignore fields beyond it.
	 */
	int (*store)(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
		     const void *value, uint32_t value_len,
		     const struct spdk_kvdev_store_opts *opts,
		     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

	/**
	 * Iovec-native Store (optional; may be NULL). Gathers exactly \c value_len
	 * bytes from \c iov[0..iovcnt) directly, with no contiguous bounce buffer.
	 * Same options/semantics as \c store. When NULL the caller must fall back to
	 * \c store with a contiguous buffer.
	 */
	int (*storev)(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
		      struct iovec *iov, int iovcnt, uint32_t value_len,
		      const struct spdk_kvdev_store_opts *opts,
		      spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

	/**
	 * Retrieve the value stored under a key into value_buf.
	 *
	 * On completion, value_len reports the true stored length. If the
	 * buffer was too small the status is SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL
	 * and buf_len bytes are still copied.
	 */
	int (*retrieve)(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
			void *value_buf, uint32_t buf_len,
			spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

	/**
	 * Iovec-native Retrieve (optional; may be NULL). Scatters the stored value
	 * across \c iov[0..iovcnt) (up to \c buf_len bytes total) directly, with no
	 * contiguous bounce buffer. Same completion semantics as \c retrieve. When
	 * NULL the caller must fall back to \c retrieve with a contiguous buffer.
	 */
	int (*retrievev)(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
			 struct iovec *iov, int iovcnt, uint32_t buf_len,
			 spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

	/**
	 * Delete the key (and its value) from the kvdev.
	 *
	 * On completion the status is SPDK_KVDEV_IO_STATUS_SUCCESS if the key was
	 * present and removed, or SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST if it was
	 * absent. The value_len argument to the completion is unused.
	 */
	int (*del)(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
		   spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

	/**
	 * Test whether the key exists in the kvdev.
	 *
	 * On completion the status is SPDK_KVDEV_IO_STATUS_SUCCESS if the key is
	 * present, or SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST if it is absent. The
	 * value_len argument to the completion is unused.
	 */
	int (*exist)(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
		     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

	/**
	 * List (iterate) the keys present in the kvdev.
	 *
	 * The backend visits keys in an UNSPECIFIED but STABLE order: the order is
	 * not defined, but as long as the namespace is not modified (no store/delete)
	 * two List calls visit keys in the same sequence. \c start_key is a POSITION
	 * into that stable order: iteration begins at start_key if it is present,
	 * otherwise at the first key that sorts at-or-after start_key in the backend's
	 * order (a vendor-specific-but-stable start point per the KV spec §2.1.6.2).
	 * A NULL/zero-length start_key begins at the very first key.
	 *
	 * For each visited key the backend invokes \c iter_cb(iter_arg, key, key_len)
	 * and stops early as soon as it returns false. When iteration finishes the
	 * backend invokes \c done_cb(done_arg, status, num_keys) where num_keys is the
	 * number of keys the iterator callback accepted.
	 *
	 * \param ch io_channel obtained from get_io_channel().
	 * \param start_key Position key, or NULL to start from the beginning.
	 * \param start_key_len Length of start_key (0 when start_key is NULL).
	 */
	int (*list)(struct spdk_io_channel *ch, const void *start_key, uint8_t start_key_len,
		    spdk_kvdev_list_cb iter_cb, void *iter_arg,
		    spdk_kvdev_list_done_cb done_cb, void *done_arg);

	/**
	 * Execute a server-side operation against a key (vendor extension,
	 * ADR-0005, "KV Exec"). OPTIONAL: a module may leave this NULL, in which
	 * case spdk_kvdev_exec() returns -ENOTSUP and the NVMf layer maps the
	 * command to an NVMe not-supported status. This keeps backends that do
	 * not (yet) support exec valid (e.g. the librados module, KVX-3).
	 *
	 * The operation is selected by a small integer \c op_id (never a
	 * class/method string on the data path). The NVMf layer resolves \c op_id
	 * against the per-namespace allowlist (KVX-2) and passes the matching
	 * entry's opaque \c binding through to the backend (KVX-3): the in-memory
	 * module ignores it (op_id alone selects the built-in), while the librados
	 * module parses it as "class:method" for rados_aio_exec. \c binding may be
	 * NULL when the allowlist entry has none. \c input/input_len carry the
	 * input blob; the backend writes its output into \c output_buf (at most
	 * \c output_buf_len bytes). On completion the value_len argument reports
	 * the TRUE output length: if it exceeds \c output_buf_len the status is
	 * SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL and output_buf_len bytes are
	 * still copied (same truncation contract as Retrieve). An unknown op_id
	 * fails with SPDK_KVDEV_IO_STATUS_INVALID.
	 *
	 * \param ch io_channel obtained from get_io_channel().
	 * \param key Key bytes (key_len in [1,16]).
	 * \param op_id Operation identifier selecting the server-side operation.
	 * \param binding Structured binding resolved from the allowlist entry for
	 *                \c op_id (ADR-0010/0012/0014). NULL when the entry has none.
	 *                The in-memory module ignores it; the librados module routes
	 *                on \c runtime (NKVX -> rados-nkvx executor: built-in native
	 *                when module_namespace=="nkvx", else wasm; CLS -> genuine
	 *                object-class method).
	 * \param read_only The namespace is read-only. The backend MUST reject any
	 *                  Exec path that could mutate stored state (a mutating
	 *                  built-in op, or a write-capable runtime/module) with
	 *                  SPDK_KVDEV_IO_STATUS_READ_ONLY. Non-mutating Exec (echo,
	 *                  the read-only nkvx executor) still runs. This enforces
	 *                  the "Exec never mutates" invariant at the mutation point
	 *                  (ADR-0014) instead of assuming it at the opcode gate.
	 * \param input Input blob bytes (may be NULL when input_len is 0).
	 * \param input_len Length of \c input in bytes.
	 * \param output_buf Buffer that receives the output blob.
	 * \param output_buf_len Capacity of \c output_buf in bytes.
	 */
	int (*exec)(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
		    uint32_t op_id, bool read_only,
		    const struct spdk_kv_exec_binding *binding,
		    const void *input, uint32_t input_len,
		    void *output_buf, uint32_t output_buf_len,
		    spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

	/**
	 * KV Exec variant whose RESULT SINK is a dma-buf (B-i V3, bead spdk-avu,
	 * MIXED-SGL): the output is not addressable as a host VA but as
	 * (\c sink_fd, \c sink_offset, \c sink_len) referencing exported device memory
	 * (e.g. GPU VRAM). A two-tier backend forwards the fd so the remote executor
	 * RDMA-WRITEs the result straight into device memory
	 * (nkvx_front_forward_dmabuf). \c sink_va is the guest IOVA the result_sink SGL
	 * segment advertised: it is load-bearing as the verbs/irdma dma-buf MR base
	 * (FI_MR_VIRT_ADDR addresses the remote MR by VA) and is NEVER dereferenced.
	 * OPTIONAL: a backend that cannot target a dma-buf leaves this NULL, in which
	 * case spdk_kvdev_exec_dmabuf() returns -ENOTSUP and the NVMf layer fails the
	 * command rather than silently writing to the wrong place. The key+input come
	 * from the SGL's RAM head; \c sink_len is the dma-buf segment capacity.
	 */
	int (*exec_dmabuf)(struct spdk_io_channel *ch, const void *key, uint8_t key_len,
			   uint32_t op_id, bool read_only,
			   const struct spdk_kv_exec_binding *binding,
			   const void *input, uint32_t input_len,
			   int sink_fd, uint64_t sink_offset, uint32_t sink_len,
			   uint64_t sink_va,
			   spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

	/**
	 * Abort an in-flight operation (KV Exec) on this channel that was submitted
	 * with the opaque \c cb_arg (vendor extension, Slice C6c). OPTIONAL: a module
	 * may leave this NULL, in which case spdk_kvdev_abort() returns -ENOTSUP and
	 * the NVMf layer reports the NVMe ABORT command as "command not aborted"
	 * (cdw0 bit0 = 1), i.e. the original command runs to natural completion.
	 *
	 * \c cb_arg is the SAME pointer the originating op was given: it uniquely
	 * identifies one in-flight command on the channel, so it is the abort lookup
	 * key (no separate handle). The op does NOT fire \c cb_arg's completion itself
	 * — it only requests cancellation; the original op's own cb_fn still fires
	 * exactly once (typically SPDK_KVDEV_IO_STATUS_ABORTED) on a later tick once
	 * the cancellation resolves. This is therefore a non-blocking, best-effort
	 * request: a slow backend cancel completes the original op asynchronously.
	 *
	 * Both calls run on the SAME io_channel/thread (the NVMf ABORT retry-thread
	 * fix routes the abort onto the aborted request's thread), so no cross-thread
	 * synchronization is needed at this layer.
	 *
	 * \param ch io_channel obtained from get_io_channel() — the channel the target
	 *           command is in flight on.
	 * \param cb_arg The opaque cb_arg the target op was submitted with.
	 * \return 0 if a matching in-flight op was found and cancellation was
	 *         requested; -ENOENT if no in-flight op matches (already completed, or
	 *         not abortable); other negative errno on failure.
	 */
	int (*abort)(struct spdk_io_channel *ch, void *cb_arg);
};

/**
 * A registered kvdev instance.  A module embeds this in its own private struct
 * and fills in name/ctxt/fn_table/module/caps before calling
 * spdk_kvdev_register().
 */
struct spdk_kvdev {
	/** Unique name of this kvdev. */
	char					*name;

	/** Module-private context passed back to the fn_table ops. */
	void					*ctxt;

	/** Operation table supplied by the module. */
	const struct spdk_kvdev_fn_table	*fn_table;

	/** Owning module. */
	struct spdk_kvdev_module		*module;

	/** Advertised limits. */
	struct spdk_kvdev_caps			caps;

	/** Unique identifier. */
	struct spdk_uuid			uuid;

	/* Internal kvdev-layer bookkeeping below this line. */
	TAILQ_HEAD(, spdk_kvdev_desc)		open_descs;
	TAILQ_ENTRY(spdk_kvdev)			internal_link;
};

/**
 * A kvdev module.  Registered once at load time via SPDK_KVDEV_MODULE_REGISTER.
 */
struct spdk_kvdev_module {
	/** Module name. */
	const char	*name;

	/** Optional module init / fini hooks. */
	int	(*module_init)(void);
	void	(*module_fini)(void);

	TAILQ_ENTRY(spdk_kvdev_module)	internal_link;
};

/**
 * Register a kvdev module. Use the SPDK_KVDEV_MODULE_REGISTER macro instead of
 * calling this directly.
 */
void spdk_kvdev_module_list_add(struct spdk_kvdev_module *module);

#define SPDK_KVDEV_MODULE_REGISTER(_name, _module)					\
	static void __attribute__((constructor)) _spdk_kvdev_module_register_##_name(void) \
	{										\
		spdk_kvdev_module_list_add(_module);					\
	}

/**
 * Initialize the kvdev subsystem. Runs registered module_init hooks. Safe to
 * call multiple times (idempotent).
 */
int spdk_kvdev_initialize(void);

/**
 * Tear down the kvdev subsystem and run module_fini hooks.
 */
void spdk_kvdev_finish(void);

/**
 * Register a kvdev. The module fills in the spdk_kvdev fields first.
 *
 * \return 0 on success, negative errno on failure (e.g. -EEXIST on name clash).
 */
int spdk_kvdev_register(struct spdk_kvdev *kvdev);

/**
 * Unregister a kvdev by name. Fails if descriptors are still open.
 *
 * \return 0 on success, negative errno otherwise.
 */
int spdk_kvdev_unregister(struct spdk_kvdev *kvdev);

/**
 * Look up a kvdev by name.
 *
 * \return the kvdev, or NULL if not found.
 */
struct spdk_kvdev *spdk_kvdev_get_by_name(const char *name);

/**
 * Iterate registered kvdevs.
 */
struct spdk_kvdev *spdk_kvdev_first(void);
struct spdk_kvdev *spdk_kvdev_next(struct spdk_kvdev *prev);

/**
 * Open a kvdev by name, obtaining a descriptor.
 *
 * \param name kvdev name.
 * \param write Reserved for future use (write access). Currently informational.
 * \param desc Output descriptor handle.
 *
 * \return 0 on success, negative errno otherwise.
 */
int spdk_kvdev_open(const char *name, bool write, struct spdk_kvdev_desc **desc);

/**
 * Close a previously opened descriptor.
 */
void spdk_kvdev_close(struct spdk_kvdev_desc *desc);

/**
 * Get the kvdev backing a descriptor.
 */
struct spdk_kvdev *spdk_kvdev_desc_get_kvdev(struct spdk_kvdev_desc *desc);

/**
 * Get the kvdev name.
 */
const char *spdk_kvdev_get_name(const struct spdk_kvdev *kvdev);

/**
 * Get the advertised capabilities of a kvdev.
 */
const struct spdk_kvdev_caps *spdk_kvdev_get_caps(const struct spdk_kvdev *kvdev);

/**
 * Get a per-thread io_channel for the kvdev backing this descriptor.
 *
 * \return io_channel, or NULL on failure. Release with spdk_put_io_channel().
 */
struct spdk_io_channel *spdk_kvdev_get_io_channel(struct spdk_kvdev_desc *desc);

/**
 * Submit a Store on the descriptor's kvdev. Thin wrapper over the fn_table.
 *
 * \param opts Store options (SIKE/SINKE flags, future TTL). May be NULL for
 *             default (unconditional insert-or-overwrite) behaviour.
 */
int spdk_kvdev_store(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		     const void *key, uint8_t key_len,
		     const void *value, uint32_t value_len,
		     const struct spdk_kvdev_store_opts *opts,
		     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a Retrieve on the descriptor's kvdev.
 */
int spdk_kvdev_retrieve(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
			const void *key, uint8_t key_len,
			void *value_buf, uint32_t buf_len,
			spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * True if the descriptor's kvdev implements the iovec-native Store/Retrieve ops
 * (spdk_kvdev_storev / spdk_kvdev_retrievev). A caller with a multi-iov payload
 * can then avoid gathering/scattering through a contiguous bounce buffer.
 */
bool spdk_kvdev_io_supports_iov(struct spdk_kvdev_desc *desc);

/**
 * Iovec-native Store: gather exactly \c value_len bytes from \c iov[0..iovcnt)
 * with no contiguous bounce buffer. Returns -ENOTSUP if the backend has no
 * storev op (check spdk_kvdev_io_supports_iov first).
 */
int spdk_kvdev_storev(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		      const void *key, uint8_t key_len,
		      struct iovec *iov, int iovcnt, uint32_t value_len,
		      const struct spdk_kvdev_store_opts *opts,
		      spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * Iovec-native Retrieve: scatter the stored value across \c iov[0..iovcnt) (up
 * to \c buf_len bytes total) with no contiguous bounce buffer. Returns -ENOTSUP
 * if the backend has no retrievev op (check spdk_kvdev_io_supports_iov first).
 */
int spdk_kvdev_retrievev(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
			 const void *key, uint8_t key_len,
			 struct iovec *iov, int iovcnt, uint32_t buf_len,
			 spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a Delete on the descriptor's kvdev. Thin wrapper over the fn_table.
 *
 * The completion fires with SPDK_KVDEV_IO_STATUS_SUCCESS if the key existed and
 * was deleted, or SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST if the key was absent. The
 * completion's value_len argument is unused.
 */
int spdk_kvdev_delete(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		      const void *key, uint8_t key_len,
		      spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * Submit an Exist on the descriptor's kvdev. Thin wrapper over the fn_table.
 *
 * The completion fires with SPDK_KVDEV_IO_STATUS_SUCCESS if the key exists, or
 * SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST if it does not. The completion's value_len
 * argument is unused.
 */
int spdk_kvdev_exist(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		     const void *key, uint8_t key_len,
		     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * Submit a List (key iteration) on the descriptor's kvdev. Thin wrapper over
 * the fn_table.
 *
 * Keys are visited in the backend's unspecified-but-stable order; \c start_key
 * is a position into that order (NULL/0 starts from the beginning). \c iter_cb
 * is invoked per key and returns false to stop early; \c done_cb fires once at
 * the end with the accepted key count. See struct spdk_kvdev_fn_table::list.
 *
 * \param start_key Position key, or NULL to start from the first key.
 * \param start_key_len Length of start_key in bytes; must be 0 when start_key
 *                      is NULL.
 *
 * \return 0 if the request was accepted (a completion will fire), negative
 *         errno if it could not be submitted (no completion fires).
 */
int spdk_kvdev_list(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		    const void *start_key, uint8_t start_key_len,
		    spdk_kvdev_list_cb iter_cb, void *iter_arg,
		    spdk_kvdev_list_done_cb done_cb, void *done_arg);

/**
 * Submit a KV Exec (vendor extension, ADR-0005) on the descriptor's kvdev. Thin
 * wrapper over the fn_table's optional \c exec op.
 *
 * Runs the server-side operation selected by \c op_id against \c key, passing
 * the \c input blob and scattering the operation's output (bounded by
 * \c output_buf_len) into \c output_buf. The completion's value_len argument
 * reports the true output length; if it exceeds \c output_buf_len the status is
 * SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL (output_buf_len bytes still copied).
 *
 * \param binding Structured binding the NVMf layer resolved from the allowlist
 *                entry for \c op_id (ADR-0010/0012/0014); passed through to the
 *                backend exec op. May be NULL.
 * \param read_only The namespace is read-only; forwarded to the backend so it
 *                  rejects any mutating Exec path with
 *                  SPDK_KVDEV_IO_STATUS_READ_ONLY (ADR-0014 invariant enforced
 *                  at the mutation point).
 *
 * \return 0 if the request was accepted (a completion will fire), -ENOTSUP if
 *         the backend has no exec op (no completion fires), or another negative
 *         errno if it could not be submitted.
 */
int spdk_kvdev_exec(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		    const void *key, uint8_t key_len, uint32_t op_id, bool read_only,
		    const struct spdk_kv_exec_binding *binding,
		    const void *input, uint32_t input_len,
		    void *output_buf, uint32_t output_buf_len,
		    spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * KV Exec whose RESULT SINK is a dma-buf-backed region (B-i V3, bead spdk-avu,
 * MIXED-SGL) rather than a host VA buffer. Identical to spdk_kvdev_exec() except
 * the output is identified by (\c sink_fd, \c sink_offset, \c sink_len) -- exported
 * device memory (e.g. GPU VRAM that is not CPU-mmappable) -- so a two-tier backend
 * can RDMA the result straight into it. \c sink_va is the guest IOVA the
 * result_sink SGL segment advertised (load-bearing as the verbs dma-buf MR base;
 * never dereferenced). The \c key / \c input come from the SGL's RAM head. Thin
 * wrapper over the optional fn_table::exec_dmabuf op.
 *
 * \return 0 if the request was accepted (a completion will fire), -ENOTSUP if
 *         the backend has no exec_dmabuf op (no completion fires), or another
 *         negative errno if it could not be submitted.
 */
int spdk_kvdev_exec_dmabuf(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
			   const void *key, uint8_t key_len, uint32_t op_id, bool read_only,
			   const struct spdk_kv_exec_binding *binding,
			   const void *input, uint32_t input_len,
			   int sink_fd, uint64_t sink_offset, uint32_t sink_len,
			   uint64_t sink_va,
			   spdk_kvdev_io_completion_cb cb_fn, void *cb_arg);

/**
 * Request cancellation of an in-flight op (KV Exec) on the descriptor's kvdev
 * that was submitted with the opaque \c cb_arg (Slice C6c). Thin wrapper over the
 * fn_table's optional \c abort op.
 *
 * The original op is NOT completed by this call; its own completion callback
 * still fires exactly once (typically SPDK_KVDEV_IO_STATUS_ABORTED) once the
 * cancellation resolves. Both this call and the target op run on the same
 * io_channel/thread.
 *
 * \return 0 if a matching in-flight op was found and cancellation requested,
 *         -ENOTSUP if the backend has no abort op, -ENOENT if no in-flight op
 *         matches \c cb_arg, or another negative errno on failure.
 */
int spdk_kvdev_abort(struct spdk_kvdev_desc *desc, struct spdk_io_channel *ch,
		     void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_KVDEV_H */
