/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

#ifndef SPDK_KVDEV_RADOS_H
#define SPDK_KVDEV_RADOS_H

#include "spdk/stdinc.h"
#include "spdk/config.h"
#include "spdk/uuid.h"
#include "spdk/kvdev.h"
#include "spdk/json.h"
#include "spdk/jsonrpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * librados oid buffer sizing. A binary key is hex-encoded into a NUL-terminated
 * C string for use as the librados oid (and, for KV Exec, the executor cache key).
 *
 * KVDEV_RADOS_OID_MAX      - spec Store/Retrieve/Exist/Delete keys (<= 16 bytes ->
 *                            32 hex chars + NUL).
 * KVDEV_RADOS_EXEC_OID_MAX - KV Exec keys (up to 255 bytes, ADR-0014 -> 510 hex
 *                            chars + NUL). The per-io nkvx_oid (cache + librados
 *                            object key for the Exec data phase) is sized to this
 *                            so a long Exec key is never truncated into a wrong
 *                            (short) oid.
 *
 * These live in the header (not the .c) so the unit test can size its working
 * buffer from the SAME source of truth as the production io->nkvx_oid field; a
 * regression that shrinks that buffer back to KVDEV_RADOS_OID_MAX then makes the
 * long-key oid round-trip test fail on truncation. See test_nkvx_long_key_oid.
 */
#define KVDEV_RADOS_OID_MAX (SPDK_KVDEV_KEY_MAX_LEN * 2 + 1)
#define KVDEV_RADOS_EXEC_OID_MAX (SPDK_KVDEV_EXEC_KEY_MAX_LEN * 2 + 1)

/*
 * Size of struct kvdev_rados_io::nkvx_oid (the Exec data-phase oid / cache key).
 * Named so both the production struct and the unit test reference one constant;
 * shrinking it back to KVDEV_RADOS_OID_MAX is what the long-key regression catches.
 */
#define KVDEV_RADOS_NKVX_OID_BUFSZ KVDEV_RADOS_EXEC_OID_MAX

/*
 * librados oids are NUL-terminated C strings, so a binary key is hex-encoded:
 * key_len bytes -> key_len*2 hex chars + NUL. \c oid must hold at least
 * key_len*2 + 1 bytes (KVDEV_RADOS_EXEC_OID_MAX covers the largest Exec key).
 * Header static-inline so the production datapath and the unit test encode oids
 * identically.
 */
static inline void
kvdev_rados_key_to_oid(const void *key, uint8_t key_len, char *oid)
{
	static const char hex[] = "0123456789abcdef";
	const uint8_t *k = key;
	uint8_t i;

	for (i = 0; i < key_len; i++) {
		oid[i * 2]     = hex[k[i] >> 4];
		oid[i * 2 + 1] = hex[k[i] & 0xf];
	}
	oid[key_len * 2] = '\0';
}

/*
 * librados-backed kvdev (ADR-0002, ADR-0004).
 *
 * Tenancy mapping:
 *   - subsystem -> rados pool (operator pre-provisioned; never auto-created)
 *   - KV namespace -> rados namespace (set on the per-instance ioctx)
 * Object-per-KV:
 *   - key   -> oid (hex-encoded, since the librados oid is a C string)
 *   - value -> object data (write_full / read / stat / remove)
 *
 * Clusters are shared and named: register one with kvdev_rados_register_cluster()
 * (mirrors bdev_rbd's cluster RPC) then create kvdevs that reference it by name.
 */

/* Options needed to register/connect a named rados cluster handle. */
struct kvdev_rados_cluster_info {
	const char		*name;		/* cluster handle name (required) */
	const char		*user_id;	/* ceph client id, e.g. "admin" (optional) */
	const char *const	*config_param;	/* NULL-terminated key,val,key,val,... (optional) */
	const char		*config_file;	/* ceph.conf path (optional) */
	const char		*key_file;	/* keyring path (optional) */
};

/* Options to create a rados-backed kvdev instance. */
struct kvdev_rados_opts {
	const char		*name;		/* kvdev name (required) */
	const char		*cluster_name;	/* named cluster handle (required) */
	const char		*pool_name;	/* rados pool == subsystem (required) */
	const char		*namespace_name;/* rados namespace == KV namespace (optional) */
	struct spdk_uuid	uuid;
	/* Max value length in bytes. 0 selects the ADR-0002 default (64 MB). */
	uint32_t		max_value_len;
#ifdef SPDK_CONFIG_MERCURY
	/*
	 * Slice C4 (ADR-0015): optional remote executor self-address (as published
	 * by the standalone rados-nkvx service, e.g. "ofi+tcp://10.110.0.2:7000").
	 * When set, nkvx Exec is forwarded over Mercury to that executor (two-tier)
	 * instead of run in-process. NULL keeps the single-tier path. Compiled in
	 * only with --with-mercury, so a stock build is byte-identical.
	 */
	const char		*remote_executor;
#endif
};

/**
 * Register (create+connect) a named, shared rados cluster handle. Mirrors
 * bdev_rbd's cluster registry: the connect can fail (no monitor, bad auth) and
 * the error is returned to the caller without leaking. The handle is connected
 * on a non-SPDK thread to avoid contending with SPDK reactors.
 *
 * \return 0 on success, negative errno otherwise.
 */
int kvdev_rados_register_cluster(const struct kvdev_rados_cluster_info *info);

/**
 * Unregister a named cluster handle. Fails (-EBUSY) if any kvdev still
 * references it.
 *
 * \return 0 on success, negative errno otherwise.
 */
int kvdev_rados_unregister_cluster(const char *name);

/**
 * Dump cluster registry info as a JSON-RPC result. If \c name is NULL, all
 * clusters are emitted as an array; otherwise just the named one.
 *
 * \return 0 on success, -ENOENT if not found.
 */
int kvdev_rados_get_clusters_info(struct spdk_jsonrpc_request *request, const char *name);

/**
 * Create a rados-backed kvdev. The referenced pool must already exist (pools are
 * operator pre-provisioned per ADR-0004).
 *
 * \return 0 on success, negative errno otherwise.
 */
int kvdev_rados_create(const struct kvdev_rados_opts *opts, struct spdk_kvdev **kvdev);

/**
 * Delete a rados-backed kvdev by name.
 *
 * \return 0 on success, negative errno otherwise.
 */
int kvdev_rados_delete(const char *name);

/*
 * ---- durable name->nsid registry (epic spdk-jhk.8) -----------------------
 *
 * The source of truth for a rados-nkv namespace's friendly name is an omap on a
 * deterministic object ('nkv.ns.registry') stored in a reserved rados namespace
 * within the namespace's pool. Each omap entry is name -> {nsid, rados_namespace},
 * encoded as a compact JSON object. Registry I/O is performed TARGET-SIDE via the
 * target's existing named librados cluster handle, so CLIs stay rados-free.
 */

/* Deterministic registry object id and the reserved rados namespace it lives in,
 * so a registry entry never collides with a data namespace's keys. */
#define KVDEV_RADOS_REGISTRY_OID "nkv.ns.registry"
#define KVDEV_RADOS_REGISTRY_NAMESPACE "nkv.registry"

/* One decoded registry entry. */
struct kvdev_rados_registry_entry {
	uint32_t	nsid;
	char		rados_namespace[256];
};

/**
 * Look up \c name in the registry omap on \c pool_name via the named cluster.
 *
 * \param cluster_name registered librados cluster handle name.
 * \param pool_name    rados pool that holds the registry object.
 * \param name         friendly namespace name (omap key).
 * \param out          decoded entry on success (caller-owned).
 *
 * \return 0 on hit (\c out filled), -ENOENT if the name is not registered,
 *         other negative errno on a rados/connect error.
 */
int kvdev_rados_registry_get(const char *cluster_name, const char *pool_name,
			     const char *name, struct kvdev_rados_registry_entry *out);

/**
 * Write (insert/overwrite) the registry entry \c name -> {nsid, rados_namespace}
 * into the registry omap on \c pool_name via the named cluster.
 *
 * \return 0 on success, negative errno otherwise.
 */
int kvdev_rados_registry_put(const char *cluster_name, const char *pool_name,
			     const char *name, uint32_t nsid, const char *rados_namespace);

/**
 * Emit JSON-RPC methods (cluster registrations + kvdev_rados_create) that
 * recreate the current rados kvdev state for config save/restore.
 */
void kvdev_rados_write_config_json(struct spdk_json_write_ctx *w);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_KVDEV_RADOS_H */
