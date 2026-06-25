/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_BDEV_KVRADOS_H
#define SPDK_BDEV_KVRADOS_H

#include "spdk/stdinc.h"

#include "spdk/bdev_module.h"

/*
 * bdev_kvrados is a PURE FORWARDER. It does NOT link librados on the front.
 * The NVMe-oF / vfio-user controller routes KV commands to this bdev via NVMe
 * I/O / admin passthru; this module decodes the KV command and (in later
 * slices) forwards it to the rados-nkvx executor over an inter-tier RPC.
 *
 * The create-time config below supplies the KV capabilities that the executor
 * exposes (max key/value length, number of keys) so that admin IDENTIFY
 * (CNS 05h / 06h) can be answered locally without a front librados.
 */

typedef void (*delete_kvrados_complete)(void *cb_arg, int bdeverrno);

/*
 * A single KV Exec allowlist entry (ADR-0005/0012/0014). These are the
 * per-namespace Exec bindings that used to live on the nvmf KV namespace and
 * now ride the bdev create RPC. The forwarder/executor slices consume them to
 * gate and route KV Exec (0x83). String fields are borrowed in opts and
 * strdup-owned once copied onto the disk.
 */
struct kvrados_exec_binding {
	uint32_t	op_id;			/* permitted KV Exec operation ID */
	char		*binding;		/* legacy 'class:method' (ADR-0005, mapped to runtime="cls") */
	char		*runtime;		/* "wasm" or "cls" (ADR-0012/0014) */
	char		*module_namespace;	/* cold-fetch locator namespace (ADR-0014) */
	char		*module_key;		/* cold-fetch locator key (ADR-0014) */
	char		*sha256;		/* artifact content hash (ADR-0010) */
	uint64_t	caps;			/* capability TIER selector (ADR-0014) */
};

struct kvrados_bdev_opts {
	char			*name;
	struct spdk_uuid	uuid;

	/* KV capability limits, advertised via IDENTIFY. */
	uint32_t		max_key_size;		/* bytes */
	uint32_t		max_value_size;		/* bytes */
	uint32_t		optimal_value_granularity; /* bytes */
	uint64_t		num_keys;		/* advertised key count / nominal capacity */

	/*
	 * Executor endpoint (rados-nkvx). Stub for S2 — the real RPC wiring is
	 * a later slice (N1/S3/S4). Carried so the create RPC surface is stable.
	 */
	char			*executor_endpoint;

	/*
	 * Per-namespace read-only bit (ADR-0008). Carried on the inter-tier wire to the
	 * executor (the authoritative enforcement point); the bdev-side verb gate is S4.
	 */
	bool			read_only;

	/*
	 * KV Exec allowlist/bindings (ADR-0005/0012/0014) — the per-namespace Exec
	 * config that used to live on the nvmf KV namespace. Borrowed array; the
	 * entries (and their strings) are copied onto the disk by create_kvrados_disk.
	 */
	const struct kvrados_exec_binding	*exec_allowlist;
	size_t					exec_allowlist_count;

	int32_t			numa_id;
};

int create_kvrados_disk(struct spdk_bdev **bdev, const struct kvrados_bdev_opts *opts);

void delete_kvrados_disk(const char *name, delete_kvrados_complete cb_fn, void *cb_arg);

/* Replace the named kvrados bdev's KV-Exec allowlist at runtime (deep-copies the
 * borrowed view). Returns 0, -ENODEV, -EINVAL (not a kvrados bdev), or -ENOMEM. */
int bdev_kvrados_set_exec_allowlist(const char *name,
				    const struct kvrados_exec_binding *allowlist, size_t count);

#endif /* SPDK_BDEV_KVRADOS_H */
