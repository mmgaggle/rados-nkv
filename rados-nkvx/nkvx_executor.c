/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/*
 * rados-nkvx executor backend (Slice C5a.1). See nkvx_executor.h and
 * docs/design/slice-c-exec-rpc-mercury.md §2.
 */

#define _POSIX_C_SOURCE 200809L

#include "nkvx_executor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include <rados/librados.h>

#include "spdk/kvdev.h"		/* enum spdk_kvdev_io_status, runtime enum, key max */
#include "spdk/util.h"		/* spdk_min (declaration-only; safe for this standalone build) */
#include "kvdev_rados_nkvx_wasm.h"	/* reused wasm runtime + TB4 object cache (C5a.2) */

/* TEST-ONLY in-memory object (slice spdk-7sr.4 / S3 loopback verification). */
struct nkvx_mem_obj {
	char		oid[SPDK_KVDEV_EXEC_KEY_MAX_LEN * 2 + 1];
	void		*value;
	size_t		len;
};

#define NKVX_MEM_MAX_OBJS 64

struct nkvx_executor {
	rados_t		cluster;
	rados_ioctx_t	ioctx;

	/* TEST-ONLY in-memory backend (no librados). Active when mem == true. */
	bool			mem;
	struct nkvx_mem_obj	mem_objs[NKVX_MEM_MAX_OBJS];
	int			mem_count;
};

/* TEST-ONLY: find an in-memory object by oid (NULL if absent). */
static struct nkvx_mem_obj *
nkvx_mem_find(struct nkvx_executor *ex, const char *oid)
{
	for (int i = 0; i < ex->mem_count; i++) {
		if (strcmp(ex->mem_objs[i].oid, oid) == 0) {
			return &ex->mem_objs[i];
		}
	}
	return NULL;
}

/* TEST-ONLY: remove an in-memory object by oid. Returns true if it existed. */
static bool
nkvx_mem_del(struct nkvx_executor *ex, const char *oid)
{
	for (int i = 0; i < ex->mem_count; i++) {
		if (strcmp(ex->mem_objs[i].oid, oid) == 0) {
			free(ex->mem_objs[i].value);
			/* Compact: move the tail entry into the hole (order is irrelevant;
			 * List sorts its own snapshot). */
			ex->mem_objs[i] = ex->mem_objs[ex->mem_count - 1];
			ex->mem_count--;
			return true;
		}
	}
	return false;
}

int
nkvx_executor_open_mem(struct nkvx_executor **out)
{
	struct nkvx_executor *ex;

	if (out == NULL) {
		return -EINVAL;
	}
	*out = NULL;
	ex = calloc(1, sizeof(*ex));
	if (ex == NULL) {
		return -ENOMEM;
	}
	ex->mem = true;
	*out = ex;
	return 0;
}

int
nkvx_executor_mem_put(struct nkvx_executor *ex, const char *oid,
		      const void *value, size_t len)
{
	struct nkvx_mem_obj *o;
	void *copy;

	if (ex == NULL || !ex->mem || oid == NULL ||
	    strlen(oid) >= sizeof(o->oid)) {
		return -EINVAL;
	}
	copy = malloc(len ? len : 1);
	if (copy == NULL) {
		return -ENOMEM;
	}
	if (len > 0) {
		memcpy(copy, value, len);
	}

	o = nkvx_mem_find(ex, oid);
	if (o == NULL) {
		if (ex->mem_count >= NKVX_MEM_MAX_OBJS) {
			free(copy);
			return -ENOSPC;
		}
		o = &ex->mem_objs[ex->mem_count++];
		snprintf(o->oid, sizeof(o->oid), "%s", oid);
	} else {
		free(o->value);
	}
	o->value = copy;
	o->len = len;
	return 0;
}

/* Hex-encode a binary key into a NUL-terminated oid (mirrors
 * kvdev_rados_key_to_oid in kvdev_rados.h — the front and executor MUST encode
 * the oid identically, ADR-0014). oid must hold key_len*2 + 1 bytes. */
static void
nkvx_key_to_oid(const void *key, uint8_t key_len, char *oid)
{
	static const char hex[] = "0123456789abcdef";
	const uint8_t *k = key;

	for (uint8_t i = 0; i < key_len; i++) {
		oid[i * 2]     = hex[k[i] >> 4];
		oid[i * 2 + 1] = hex[k[i] & 0xf];
	}
	oid[key_len * 2] = '\0';
}

int
nkvx_executor_open(const char *conf, const char *user, const char *pool,
		   const char *ns, struct nkvx_executor **out)
{
	struct nkvx_executor *ex;
	int rc;

	if (out == NULL || pool == NULL || pool[0] == '\0') {
		return -EINVAL;
	}
	*out = NULL;

	ex = calloc(1, sizeof(*ex));
	if (ex == NULL) {
		return -ENOMEM;
	}

	rc = rados_create(&ex->cluster, user ? user : "admin");
	if (rc < 0) {
		fprintf(stderr, "nkvx_executor: rados_create failed: %s\n", strerror(-rc));
		free(ex);
		return rc;
	}

	/* Explicit conf must parse; default search is best-effort (matches the
	 * front's kvdev_rados register-cluster discipline). */
	rc = rados_conf_read_file(ex->cluster, conf);
	if (conf != NULL && rc < 0) {
		fprintf(stderr, "nkvx_executor: rados_conf_read_file(%s) failed: %s\n",
			conf, strerror(-rc));
		goto err_shutdown;
	}

	rc = rados_connect(ex->cluster);
	if (rc < 0) {
		fprintf(stderr, "nkvx_executor: rados_connect failed: %s\n", strerror(-rc));
		goto err_shutdown;
	}

	rc = rados_ioctx_create(ex->cluster, pool, &ex->ioctx);
	if (rc < 0) {
		fprintf(stderr, "nkvx_executor: rados_ioctx_create(pool=%s) failed: %s\n",
			pool, strerror(-rc));
		goto err_shutdown;
	}
	if (ns != NULL && ns[0] != '\0') {
		rados_ioctx_set_namespace(ex->ioctx, ns);
	}

	*out = ex;
	return 0;

err_shutdown:
	/* No ioctx is created before any of the goto sites, so only the cluster
	 * handle (valid post-rados_create) needs teardown. */
	rados_shutdown(ex->cluster);
	free(ex);
	return rc;
}

void
nkvx_executor_close(struct nkvx_executor *ex)
{
	if (ex == NULL) {
		return;
	}
	/*
	 * Drop the wasm runtime's process-wide caches and join the epoch ticker
	 * thread (spdk-0k1) — the teardown the SPDK module does in module_fini. The
	 * executor owns these globals, so release them here so a stop/restart leaks
	 * neither the cached object/warm/module entries nor the ticker thread.
	 * No-ops in a --without-wasm stub build.
	 */
	kvdev_rados_nkvx_wasm_cache_reset();
	kvdev_rados_nkvx_wasm_module_cache_reset();
	kvdev_rados_nkvx_wasm_runtime_teardown();

	if (ex->mem) {
		for (int i = 0; i < ex->mem_count; i++) {
			free(ex->mem_objs[i].value);
		}
		free(ex);
		return;
	}

	if (ex->ioctx != NULL) {
		rados_ioctx_destroy(ex->ioctx);
	}
	rados_shutdown(ex->cluster);
	free(ex);
}

void
nkvx_exec_result_free(struct nkvx_exec_result *res)
{
	if (res == NULL) {
		return;
	}
	free(res->buf);
	res->buf = NULL;
	res->buf_len = 0;
}

/* Set the compute outcome: status, the TRUE result_len, and a freshly-allocated
 * copy of the `deliver` bytes at src (the delivery-agnostic buffer the handler
 * inlines or PUSHes, design §1.3). deliver==0 leaves buf NULL. */
static int
nkvx_result_set(struct nkvx_exec_result *res, enum spdk_kvdev_io_status status,
		uint32_t result_len, const void *src, uint32_t deliver)
{
	res->status = status;
	res->result_len = result_len;
	res->buf = NULL;
	res->buf_len = 0;

	if (deliver == 0) {
		return 0;
	}
	res->buf = malloc(deliver);
	if (res->buf == NULL) {
		res->status = SPDK_KVDEV_IO_STATUS_NOMEM;
		res->result_len = 0;
		return 0;
	}
	memcpy(res->buf, src, deliver);
	res->buf_len = deliver;
	return 0;
}

/*
 * Run a built-in module over a cold-filled object. Mirrors the canonical built-in
 * semantics in kvdev_rados_nkvx.c:600-624 (kvdev_rados_nkvx_run_module):
 *   - bytecount: result is the object length as a little-endian uint64 (8 B).
 *   - identity:  result is the object bytes (truncated to the host cap, true
 *                length reported, matching Retrieve/ADR-0014 truncation).
 * The object is the value stored at oid=hex(key); osize is the tenant output cap.
 */
static int
nkvx_run_builtin(struct nkvx_executor *ex, const char *oid,
		 const nkvx_exec_in_t *in, struct nkvx_exec_result *res)
{
	const char *module = in->module_key;
	uint32_t osize = in->osize;
	uint64_t size = 0;
	time_t mtime = 0;
	int rc;

	/*
	 * Input-observing built-ins (spdk-aep): operate on the per-request input
	 * (in->input_inline / in->input_len -- the C7 PULL path repoints input_inline
	 * at the pulled buffer for large inputs, so this is uniform for inline and
	 * bulk). They do NOT read the stored object, so they run before rados_stat.
	 *   - inputlen:  result is the input length as a little-endian uint64 (8 B).
	 *   - inputecho: result is the input bytes (truncated to the host cap, true
	 *                length reported, matching identity/Retrieve truncation).
	 */
	if (strcmp(module, "inputlen") == 0) {
		uint64_t len = in->input_len;
		uint32_t result_len = (uint32_t)sizeof(len);
		uint32_t deliver = spdk_min(osize, result_len);
		enum spdk_kvdev_io_status st = (result_len > osize) ?
			SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL : SPDK_KVDEV_IO_STATUS_SUCCESS;

		return nkvx_result_set(res, st, result_len, &len, deliver);
	}

	if (strcmp(module, "inputecho") == 0) {
		uint32_t result_len = in->input_len;
		uint32_t deliver = spdk_min(osize, result_len);
		enum spdk_kvdev_io_status st = (result_len > osize) ?
			SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL : SPDK_KVDEV_IO_STATUS_SUCCESS;
		char *buf;

		if (deliver == 0) {
			return nkvx_result_set(res, st, result_len, NULL, 0);
		}
		if (in->input_inline == NULL) {
			return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_INVALID, 0, NULL, 0);
		}
		buf = malloc(deliver);
		if (buf == NULL) {
			return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_NOMEM, 0, NULL, 0);
		}
		memcpy(buf, in->input_inline, deliver);
		res->status = st;
		res->result_len = result_len;
		res->buf = buf;
		res->buf_len = deliver;
		return 0;
	}

	rc = rados_stat(ex->ioctx, oid, &size, &mtime);
	if (rc == -ENOENT) {
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0, NULL, 0);
	}
	if (rc < 0) {
		fprintf(stderr, "nkvx_executor: rados_stat(%s) failed: %s\n", oid, strerror(-rc));
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
	}

	if (strcmp(module, "bytecount") == 0) {
		uint64_t count = size;	/* object length, LE on this host's wire */
		uint32_t result_len = (uint32_t)sizeof(count);
		uint32_t deliver = spdk_min(osize, result_len);
		enum spdk_kvdev_io_status st = (result_len > osize) ?
			SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL : SPDK_KVDEV_IO_STATUS_SUCCESS;

		/* 8 B never exceeds NKVX_INLINE_MAX; always inline-deliverable. */
		return nkvx_result_set(res, st, result_len, &count, deliver);
	}

	if (strcmp(module, "identity") == 0) {
		uint32_t result_len = (size > UINT32_MAX) ? UINT32_MAX : (uint32_t)size;
		uint32_t deliver = spdk_min(osize, result_len);
		enum spdk_kvdev_io_status st = (size > osize) ?
			SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL : SPDK_KVDEV_IO_STATUS_SUCCESS;
		char *buf;
		int n;

		/*
		 * The full delivered size (up to osize, possibly 64 MiB) is materialized
		 * here; the handler PUSHes it into the front's result_sink when it
		 * exceeds NKVX_INLINE_MAX (Slice C7). The backend is delivery-agnostic.
		 */
		if (deliver == 0) {
			return nkvx_result_set(res, st, result_len, NULL, 0);
		}
		buf = malloc(deliver);
		if (buf == NULL) {
			return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_NOMEM, 0, NULL, 0);
		}
		n = rados_read(ex->ioctx, oid, buf, deliver, 0);
		if (n < 0) {
			fprintf(stderr, "nkvx_executor: rados_read(%s) failed: %s\n",
				oid, strerror(-n));
			free(buf);
			return nkvx_result_set(res,
				(n == -ENOENT) ? SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST
					       : SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
		}
		/* Hand the buffer straight to the result (no second copy) — adopt it. */
		res->status = st;
		res->result_len = result_len;
		res->buf = buf;
		res->buf_len = (uint32_t)n;
		return 0;
	}

	/* Unknown built-in: no static binding for this op. */
	return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED, 0, NULL, 0);
}

/*
 * Object cold-fill callback (Slice C5a.2): invoked by run_cached ONLY on a TB4
 * object-cache MISS to populate the (page-rounded) cache slot from RADOS. On a
 * cache HIT this never runs — the cold-fill-once property the TB4 acceptance
 * proof asserts (kvdev_rados_nkvx_wasm.h:154-157).
 */
struct nkvx_obj_fill {
	rados_ioctx_t	ioctx;
	const char	*oid;
};

static int
nkvx_obj_fill(void *buf, size_t cap, size_t *out_len, void *arg)
{
	struct nkvx_obj_fill *f = arg;
	int n = rados_read(f->ioctx, f->oid, buf, cap, 0);

	if (n < 0) {
		return n;	/* negative errno; run_cached discards the slot */
	}
	*out_len = (size_t)n;
	return 0;
}

/*
 * Cold-fetch the module object (the verified .wasm bytes) from its locator. The
 * locator namespace is "pool" or "pool/namespace"; the module object name is
 * module_key. Returns 0 and sets buf_out + len_out (caller frees buf_out), or a
 * negative errno (notably -ENOENT for a missing module object).
 */
static int
nkvx_fetch_module(struct nkvx_executor *ex, const char *module_ns,
		  const char *module_key, void **buf_out, size_t *len_out)
{
	char pool[256];
	const char *ns = NULL;
	const char *slash = strchr(module_ns, '/');
	rados_ioctx_t mctx;
	uint64_t size = 0;
	time_t mtime = 0;
	void *buf;
	int rc, n;

	if (slash != NULL) {
		size_t pl = (size_t)(slash - module_ns);

		if (pl == 0 || pl >= sizeof(pool)) {
			return -EINVAL;
		}
		memcpy(pool, module_ns, pl);
		pool[pl] = '\0';
		ns = slash + 1;
	} else {
		int w = snprintf(pool, sizeof(pool), "%s", module_ns);

		if (w < 0 || (size_t)w >= sizeof(pool)) {
			return -EINVAL;
		}
	}

	rc = rados_ioctx_create(ex->cluster, pool, &mctx);
	if (rc < 0) {
		return rc;
	}
	if (ns != NULL && ns[0] != '\0') {
		rados_ioctx_set_namespace(mctx, ns);
	}

	rc = rados_stat(mctx, module_key, &size, &mtime);
	if (rc < 0) {
		rados_ioctx_destroy(mctx);
		return rc;
	}
	buf = malloc(size ? size : 1);
	if (buf == NULL) {
		rados_ioctx_destroy(mctx);
		return -ENOMEM;
	}
	n = rados_read(mctx, module_key, buf, size, 0);
	rados_ioctx_destroy(mctx);
	if (n < 0) {
		free(buf);
		return n;
	}

	*buf_out = buf;
	*len_out = (size_t)n;
	return 0;
}

/*
 * Run a real wasm module (Slice C5a.2): fetch + sha256-verify the module (the
 * deny-by-default ADR-0010 anchor, enforced inside the wasm core), then run it
 * over the object identified by oid through the TB4 object cache — cold-fill-once
 * on a miss, served zero-copy on a hit. Mirrors the wasm branch of the canonical
 * kvdev_rados_nkvx_run_module (kvdev_rados_nkvx.c:563-592), synchronous here.
 */
static int
nkvx_run_wasm(struct nkvx_executor *ex, const char *oid,
	      const nkvx_exec_in_t *in, struct nkvx_exec_result *res)
{
	struct kvdev_rados_nkvx_module mod;
	struct kvdev_rados_nkvx_wasm_stats stats;
	static const char wasm_pfx[] = "wasm:";
	const char *name;
	void *mod_buf = NULL;
	size_t mod_len = 0;
	void *out_buf = NULL;
	void *pin;
	uint32_t osize = in->osize;
	uint32_t rlen = 0;
	uint32_t deliver;
	int kvst;

	/* Deny-by-default (ADR-0010): a wasm binding MUST carry a bound sha256 and a
	 * module locator. The front already gated this; re-check defensively. */
	if (!in->sha256_valid || in->module_key == NULL || in->module_key[0] == '\0' ||
	    in->module_ns == NULL || in->module_ns[0] == '\0') {
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_INVALID, 0, NULL, 0);
	}

	/* Run name = exported function / warm-cache label = module_key sans "wasm:". */
	name = in->module_key;
	if (strncmp(name, wasm_pfx, sizeof(wasm_pfx) - 1) == 0) {
		name += sizeof(wasm_pfx) - 1;
	}
	if (name[0] == '\0') {
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_INVALID, 0, NULL, 0);
	}

	memset(&mod, 0, sizeof(mod));
	memcpy(mod.sha256, in->sha256, SPDK_KV_EXEC_SHA256_LEN);
	mod.caps = in->caps;
	/*
	 * Read-only-executor guarantee (ADR-0008 D3): thread the inter-tier read_only
	 * bit into the run so a WRITE-CAPABLE wasm module (one declaring write intent via
	 * the reserved export) is rejected with READ_ONLY at the mutation point, before
	 * it runs. A read-only module runs normally on a read-only namespace.
	 */
	mod.read_only = in->read_only != 0;

	/*
	 * Fetch the module only on a compiled-module cache MISS; on a hit the wasm
	 * core serves the cached compiled artifact keyed by sha256 (no refetch). On a
	 * miss, run the HARD deny-by-default hash gate (ADR-0010) explicitly BEFORE
	 * any run — exactly as the front datapath does on the reactor
	 * (kvdev_rados.c:832-842, kvdev_rados_nkvx_wasm_module_verify). This both maps
	 * a mismatch to INVALID (design §3) and closes the warm-instance bypass: an
	 * uncached (e.g. wrong) sha never reaches the (name,oid)-keyed warm cache.
	 * A cached sha was already verified when it was first admitted.
	 */
	if (!kvdev_rados_nkvx_wasm_module_cached(mod.sha256)) {
		int rc = nkvx_fetch_module(ex, in->module_ns, in->module_key,
					   &mod_buf, &mod_len);
		if (rc < 0) {
			/* Missing/unreadable module object: a bad locator/binding. */
			return nkvx_result_set(res,
				(rc == -ENOENT) ? SPDK_KVDEV_IO_STATUS_INVALID
						: SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
		}
		int gate = kvdev_rados_nkvx_wasm_module_verify(mod.sha256, mod_buf, mod_len);
		if (gate != SPDK_KVDEV_IO_STATUS_SUCCESS) {
			/* Hash mismatch -> INVALID; runtime unavailable -> NOT_SUPPORTED.
			 * Unverified bytes are NEVER compiled or run. */
			if (gate == SPDK_KVDEV_IO_STATUS_INVALID) {
				/*
				 * OQ-6 telemetry (design §3): a hash mismatch collapses to INVALID
				 * at the tenant (indistinguishable from a malformed request, which
				 * is acceptable — the tenant cannot fix either). Emit a DISTINCT
				 * executor-side line so an operator can tell a provisioning bug /
				 * tamper (fetched module bytes != bound sha256) from a bad request.
				 * No new tenant-visible status.
				 */
				fprintf(stderr, "nkvx_executor: HASH_MISMATCH module_ns=%s module_key=%s "
					"(fetched %zu bytes do NOT match bound sha256) -> INVALID; "
					"unverified bytes NOT run (ADR-0010)\n",
					in->module_ns ? in->module_ns : "(null)",
					in->module_key ? in->module_key : "(null)", mod_len);
			}
			free(mod_buf);
			return nkvx_result_set(res, (enum spdk_kvdev_io_status)gate, 0, NULL, 0);
		}
		mod.bytes = mod_buf;
		mod.bytes_len = mod_len;
	}

	if (osize > 0) {
		out_buf = malloc(osize);
		if (out_buf == NULL) {
			free(mod_buf);
			return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_NOMEM, 0, NULL, 0);
		}
	}

	/*
	 * TB4: probe-and-pin first; a hit runs zero-copy off the cached object with
	 * NO librados (proving cold-fill-once). A miss stats the object (cheap) then
	 * run_cached cold-fills it via nkvx_obj_fill exactly once.
	 */
	pin = kvdev_rados_nkvx_wasm_cache_pin(oid);
	if (pin != NULL) {
		kvst = kvdev_rados_nkvx_wasm_run_pinned(name, &mod, pin, out_buf, osize, &rlen);
		kvdev_rados_nkvx_wasm_cache_unpin(pin);
	} else {
		uint64_t size = 0;
		time_t mtime = 0;
		int rc = rados_stat(ex->ioctx, oid, &size, &mtime);

		if (rc == -ENOENT) {
			free(out_buf);
			free(mod_buf);
			return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0, NULL, 0);
		}
		if (rc < 0) {
			free(out_buf);
			free(mod_buf);
			return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
		}
		struct nkvx_obj_fill fill = { .ioctx = ex->ioctx, .oid = oid };

		kvst = kvdev_rados_nkvx_wasm_run_cached(name, &mod, oid, (size_t)size,
						        nkvx_obj_fill, &fill,
						        out_buf, osize, &rlen);
	}
	free(mod_buf);

	kvdev_rados_nkvx_wasm_get_stats(&stats);
	fprintf(stderr, "nkvx_executor: wasm '%s' done status=%d result_len=%u "
		"cold_fills=%llu content_hits=%llu warm_hits=%llu\n",
		name, kvst, rlen,
		(unsigned long long)stats.cold_fills,
		(unsigned long long)stats.content_hits,
		(unsigned long long)stats.warm_hits);

	deliver = spdk_min(osize, rlen);
	/*
	 * Adopt out_buf as the result (no second copy): the handler inlines it when
	 * deliver <= NKVX_INLINE_MAX, else PUSHes it into the front's result_sink
	 * (Slice C7). out_buf is osize bytes; only the first `deliver` are valid.
	 */
	if (deliver == 0) {
		free(out_buf);
		return nkvx_result_set(res, (enum spdk_kvdev_io_status)kvst, rlen, NULL, 0);
	}
	res->status = (enum spdk_kvdev_io_status)kvst;
	res->result_len = rlen;
	res->buf = out_buf;		/* adopted; freed by nkvx_exec_result_free */
	res->buf_len = deliver;
	return 0;
}

int
nkvx_executor_run(struct nkvx_executor *ex, const nkvx_exec_in_t *in,
		  struct nkvx_exec_result *res)
{
	char oid[SPDK_KVDEV_EXEC_KEY_MAX_LEN * 2 + 1];

	memset(res, 0, sizeof(*res));

	if (ex == NULL || in == NULL) {
		nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
		return -EINVAL;
	}
	if (in->key_len == 0) {
		nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_INVALID, 0, NULL, 0);
		return 0;
	}

	nkvx_key_to_oid(in->key, in->key_len, oid);

	/*
	 * Route exactly as the front did (design §2.2): both flavors are runtime=NKVX,
	 * distinguished by module_ns. The BUILT-IN native route is runtime=NKVX with
	 * module_ns "nkvx" and module_key the built-in name. The cold-fetch WASM route
	 * (runtime=NKVX, non-"nkvx" module_ns, fetch+verify+run_cached) is Slice C5a.2.
	 */
	if (in->runtime == (uint8_t)SPDK_KV_EXEC_RUNTIME_NKVX &&
	    in->module_ns != NULL && strcmp(in->module_ns, "nkvx") == 0 &&
	    in->module_key != NULL) {
		return nkvx_run_builtin(ex, oid, in, res);
	}

	if (in->runtime == (uint8_t)SPDK_KV_EXEC_RUNTIME_NKVX) {
		return nkvx_run_wasm(ex, oid, in, res);
	}

	nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_INVALID, 0, NULL, 0);
	return 0;
}

/*
 * RETRIEVE (slice spdk-7sr.4 / S3): read the object stored at oid=hex(key) through
 * the SAME TB4 object cache the Exec path uses. The value is returned delivery-
 * agnostically in res (the Mercury handler inlines it or PUSHes it into the front's
 * result_sink). Truncation semantics match ADR-0014 / Exec identity: res->result_len
 * is always the TRUE value length (CQE DW0); the delivered buffer is min(len, osize).
 *
 * Cache discipline (mirrors nkvx_run_wasm):
 *   - pin/hit: serve the cached bytes with NO librados (cold-fill-once on the shared
 *     cache means a prior Retrieve/Exec already warmed it).
 *   - miss: rados_read the object, then warm the shared cache via
 *     kvdev_rados_nkvx_wasm_cache_fill so a subsequent Retrieve/Exec hits. The
 *     cache_fill is best-effort (a stub/no-wasm build has no cache); the value the
 *     caller gets comes from the read either way.
 */
static int
nkvx_retrieve(struct nkvx_executor *ex, const char *oid,
	      const nkvx_kv_in_t *in, struct nkvx_exec_result *res)
{
	uint32_t osize = in->osize;
	uint64_t size = 0;
	time_t mtime = 0;
	uint32_t result_len;
	uint32_t deliver;
	enum spdk_kvdev_io_status st;
	char *buf;
	void *pin;
	int rc;

	/* TEST-ONLY in-memory backend: serve straight from the table (still exercises
	 * the truncation / KEY_NOT_EXIST / DW0 logic and the whole front->RPC->executor
	 * ->result path over na+sm). The librados path below is the real implementation. */
	if (ex->mem) {
		struct nkvx_mem_obj *o = nkvx_mem_find(ex, oid);

		if (o == NULL) {
			return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0, NULL, 0);
		}
		result_len = (o->len > UINT32_MAX) ? UINT32_MAX : (uint32_t)o->len;
		deliver = spdk_min(osize, result_len);
		st = (result_len > osize) ? SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL
					  : SPDK_KVDEV_IO_STATUS_SUCCESS;
		return nkvx_result_set(res, st, result_len,
				       (deliver > 0) ? o->value : NULL, deliver);
	}

	/* Cache HIT: serve the pinned bytes, no librados. */
	pin = kvdev_rados_nkvx_wasm_cache_pin(oid);
	if (pin != NULL) {
		const void *obytes = NULL;
		size_t olen = 0;

		kvdev_rados_nkvx_wasm_pin_object(pin, &obytes, &olen);
		result_len = (olen > UINT32_MAX) ? UINT32_MAX : (uint32_t)olen;
		deliver = spdk_min(osize, result_len);
		st = (result_len > osize) ? SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL
					  : SPDK_KVDEV_IO_STATUS_SUCCESS;
		rc = nkvx_result_set(res, st, result_len,
				     (deliver > 0) ? obytes : NULL, deliver);
		kvdev_rados_nkvx_wasm_cache_unpin(pin);
		return rc;
	}

	/* MISS: stat for the true length (and key-not-found), then read. */
	rc = rados_stat(ex->ioctx, oid, &size, &mtime);
	if (rc == -ENOENT) {
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0, NULL, 0);
	}
	if (rc < 0) {
		fprintf(stderr, "nkvx_executor: rados_stat(%s) failed: %s\n", oid, strerror(-rc));
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
	}

	result_len = (size > UINT32_MAX) ? UINT32_MAX : (uint32_t)size;
	deliver = spdk_min(osize, result_len);
	st = (result_len > osize) ? SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL
				  : SPDK_KVDEV_IO_STATUS_SUCCESS;

	/* Warm the shared TB4 cache (best-effort; reads the full object once). On a
	 * stub/no-wasm build this returns NOT_SUPPORTED and we fall back to a direct
	 * read below. On success the bytes are then served from the cache (one read). */
	{
		struct nkvx_obj_fill fill = { .ioctx = ex->ioctx, .oid = oid };
		int frc = kvdev_rados_nkvx_wasm_cache_fill(oid, (size_t)size,
							   nkvx_obj_fill, &fill);
		if (frc == SPDK_KVDEV_IO_STATUS_SUCCESS) {
			pin = kvdev_rados_nkvx_wasm_cache_pin(oid);
			if (pin != NULL) {
				const void *obytes = NULL;
				size_t olen = 0;

				kvdev_rados_nkvx_wasm_pin_object(pin, &obytes, &olen);
				/* The object may have changed length between stat and fill;
				 * trust the cached length as the authoritative value. */
				result_len = (olen > UINT32_MAX) ? UINT32_MAX : (uint32_t)olen;
				deliver = spdk_min(osize, result_len);
				st = (result_len > osize) ? SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL
							  : SPDK_KVDEV_IO_STATUS_SUCCESS;
				rc = nkvx_result_set(res, st, result_len,
						     (deliver > 0) ? obytes : NULL, deliver);
				kvdev_rados_nkvx_wasm_cache_unpin(pin);
				return rc;
			}
		}
	}

	/* Fallback (no cache, or pin lost a race to an invalidation): read directly. */
	if (deliver == 0) {
		return nkvx_result_set(res, st, result_len, NULL, 0);
	}
	buf = malloc(deliver);
	if (buf == NULL) {
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_NOMEM, 0, NULL, 0);
	}
	rc = rados_read(ex->ioctx, oid, buf, deliver, 0);
	if (rc < 0) {
		fprintf(stderr, "nkvx_executor: rados_read(%s) failed: %s\n", oid, strerror(-rc));
		free(buf);
		return nkvx_result_set(res,
			(rc == -ENOENT) ? SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST
					: SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
	}
	/* Adopt the buffer (no second copy). Only `rc` bytes are valid. */
	res->status = st;
	res->result_len = result_len;
	res->buf = buf;
	res->buf_len = (uint32_t)rc;
	return 0;
}

/*
 * STORE (slice S4): write the value at oid=hex(key) through librados, then
 * invalidate the shared TB4 object cache so a later Retrieve/Exec cold-reads the
 * fresh bytes (MANDATORY coherence, ADR-0008 D3 / spdk-xmu.9 — a Store mutates
 * through the executor under the forwarder pivot). SIKE/SINKE ride in_store_flags:
 *   SINKE -> create-exclusive (fails KEY_EXIST if the object already exists)
 *   SIKE  -> assert-exists    (fails KEY_NOT_EXIST if the object is absent)
 * applied atomically with the write so the conditional and the value land as one
 * rados op (mirrors kvdev_rados_store_v).
 *
 * Read-only is enforced AUTHORITATIVELY here (the unbypassable boundary): a Store
 * on a read-only namespace is rejected with READ_ONLY before any librados write,
 * even for a direct RPC client that skipped the front fast-path (ADR-0008 D2/D3).
 */
static int
nkvx_store(struct nkvx_executor *ex, const char *oid,
	   const nkvx_kv_in_t *in, struct nkvx_exec_result *res)
{
	uint8_t flags = in->store_flags;
	const void *value = in->value_inline;
	uint32_t value_len = in->value_len;
	int rc;

	/* Authoritative read-only gate (ADR-0008 D3): reject the mutation at the
	 * data, before any write. The front MAY have pre-rejected as a fast path. */
	if (in->read_only) {
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_READ_ONLY, 0, NULL, 0);
	}

	/*
	 * value_inline is in place by now: a small value (<= NKVX_INLINE_MAX) rode inline;
	 * a large value was RDMA-PULLed from the front's value_bulk into a local buffer by
	 * nkvx_kv_handler before dispatch (the input-PULL analogue of Exec large-input).
	 * A non-empty value with no buffer means neither path delivered it — decline
	 * cleanly rather than write a truncated object.
	 */
	if (value_len > 0 && value == NULL) {
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED, 0, NULL, 0);
	}

	/* TEST-ONLY in-memory backend (loopback verification without a cluster). */
	if (ex->mem) {
		struct nkvx_mem_obj *o = nkvx_mem_find(ex, oid);

		if ((flags & SPDK_KVDEV_STORE_FLAG_SINKE) && o != NULL) {
			return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_KEY_EXIST, 0, NULL, 0);
		}
		if ((flags & SPDK_KVDEV_STORE_FLAG_SIKE) && o == NULL) {
			return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0, NULL, 0);
		}
		rc = nkvx_executor_mem_put(ex, oid, value, value_len);
		if (rc != 0) {
			return nkvx_result_set(res,
				(rc == -ENOSPC) ? SPDK_KVDEV_IO_STATUS_NOMEM
						: SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
		}
		kvdev_rados_nkvx_wasm_cache_invalidate(oid);
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_SUCCESS, 0, NULL, 0);
	}

	/*
	 * Invalidate BEFORE the write so a concurrent Exec/Retrieve cannot get a fresh
	 * cache hit on the old bytes after this point (mirrors kvdev_rados_store_v's
	 * submit-time invalidate). Conservative: even if the write fails, the only cost
	 * is a re-read of the unchanged object — never a stale read.
	 */
	kvdev_rados_nkvx_wasm_cache_invalidate(oid);

	if (flags & (SPDK_KVDEV_STORE_FLAG_SINKE | SPDK_KVDEV_STORE_FLAG_SIKE)) {
		rados_write_op_t op = rados_create_write_op();

		if (op == NULL) {
			return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_NOMEM, 0, NULL, 0);
		}
		if (flags & SPDK_KVDEV_STORE_FLAG_SINKE) {
			rados_write_op_create(op, LIBRADOS_CREATE_EXCLUSIVE, NULL);
		} else {
			rados_write_op_assert_exists(op);
		}
		rados_write_op_write_full(op, value_len ? value : "", value_len);
		rc = rados_write_op_operate(op, ex->ioctx, oid, NULL, 0);
		rados_release_write_op(op);
	} else {
		rc = rados_write_full(ex->ioctx, oid, value_len ? value : "", value_len);
	}

	if (rc == -EEXIST) {
		/* SINKE conflict: the key already exists. */
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_KEY_EXIST, 0, NULL, 0);
	}
	if (rc == -ENOENT) {
		/* SIKE conflict: the key was absent. */
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0, NULL, 0);
	}
	if (rc < 0) {
		fprintf(stderr, "nkvx_executor: STORE(%s) failed: %s\n", oid, strerror(-rc));
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
	}
	return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_SUCCESS, 0, NULL, 0);
}

/*
 * DELETE (slice S4): remove the object at oid=hex(key) and invalidate the cache.
 * Read-only gated authoritatively (mutating verb). KEY_NOT_EXIST when absent.
 */
static int
nkvx_delete(struct nkvx_executor *ex, const char *oid,
	    const nkvx_kv_in_t *in, struct nkvx_exec_result *res)
{
	int rc;

	if (in->read_only) {
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_READ_ONLY, 0, NULL, 0);
	}

	if (ex->mem) {
		bool existed = nkvx_mem_del(ex, oid);

		kvdev_rados_nkvx_wasm_cache_invalidate(oid);
		return nkvx_result_set(res,
			existed ? SPDK_KVDEV_IO_STATUS_SUCCESS
				: SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0, NULL, 0);
	}

	kvdev_rados_nkvx_wasm_cache_invalidate(oid);
	rc = rados_remove(ex->ioctx, oid);
	if (rc == -ENOENT) {
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0, NULL, 0);
	}
	if (rc < 0) {
		fprintf(stderr, "nkvx_executor: DELETE(%s) failed: %s\n", oid, strerror(-rc));
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
	}
	return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_SUCCESS, 0, NULL, 0);
}

/*
 * EXIST (slice S4): non-mutating probe. SUCCESS + the stored value length echoed
 * in result_len (CQE DW0) when present; KEY_NOT_EXIST when absent. No body.
 */
static int
nkvx_exist(struct nkvx_executor *ex, const char *oid,
	   const nkvx_kv_in_t *in, struct nkvx_exec_result *res)
{
	uint64_t size = 0;
	time_t mtime = 0;
	int rc;

	(void)in;

	if (ex->mem) {
		struct nkvx_mem_obj *o = nkvx_mem_find(ex, oid);

		if (o == NULL) {
			return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0, NULL, 0);
		}
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_SUCCESS,
				       (o->len > UINT32_MAX) ? UINT32_MAX : (uint32_t)o->len,
				       NULL, 0);
	}

	rc = rados_stat(ex->ioctx, oid, &size, &mtime);
	if (rc == -ENOENT) {
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_KEY_NOT_EXIST, 0, NULL, 0);
	}
	if (rc < 0) {
		fprintf(stderr, "nkvx_executor: EXIST(%s) failed: %s\n", oid, strerror(-rc));
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
	}
	return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_SUCCESS,
			       (size > UINT32_MAX) ? UINT32_MAX : (uint32_t)size, NULL, 0);
}

/*
 * LIST helpers + handler (slice S4). The return structure is the NRK-prefixed
 * format the tenant edge expects (mirrors lib/nvmf/ctrlr_kvdev.c list assembly):
 *   [u32 NRK][ per key: u16 key_len, key_len key bytes, pad to 4-byte align ]...
 * Keys are the BINARY keys (oid hex-decoded back to bytes). Truncation: only whole
 * keys that fit in osize are emitted; result_len is the FULL byte length the
 * listing WOULD occupy (CQE DW0), BUFFER_TOO_SMALL when it exceeds osize.
 */
#define NKVX_LIST_NRK_SIZE 4u

static uint32_t
nkvx_list_entry_size(uint8_t key_len)
{
	/* u16 len + key, rounded up to a 4-byte boundary. */
	return (uint32_t)SPDK_ALIGN_CEIL(sizeof(uint16_t) + key_len, 4);
}

/* Decode a hex oid back into the binary key. Returns the key length, or 0 if the
 * oid is not a valid even-length hex string within the key bound. */
static uint8_t
nkvx_oid_to_key(const char *oid, uint8_t key[SPDK_KVDEV_EXEC_KEY_MAX_LEN])
{
	size_t len = strlen(oid);
	size_t klen;

	if (len == 0 || (len & 1) || (len / 2) > SPDK_KVDEV_EXEC_KEY_MAX_LEN) {
		return 0;
	}
	klen = len / 2;
	for (size_t i = 0; i < klen; i++) {
		int hi, lo;
		char c;

		c = oid[i * 2];
		hi = (c >= '0' && c <= '9') ? c - '0' :
		     (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
		c = oid[i * 2 + 1];
		lo = (c >= '0' && c <= '9') ? c - '0' :
		     (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
		if (hi < 0 || lo < 0) {
			return 0;
		}
		key[i] = (uint8_t)((hi << 4) | lo);
	}
	return (uint8_t)klen;
}

/* Append one { u16 key_len, key, pad } entry into buf at *off, bounded by cap.
 * Returns false (stop) when the whole entry would not fit. */
static bool
nkvx_list_emit(uint8_t *buf, uint32_t cap, uint32_t *off, const uint8_t *key,
	       uint8_t key_len)
{
	uint32_t entry = nkvx_list_entry_size(key_len);
	uint16_t kl16 = key_len;
	uint8_t *p;

	if (*off + entry > cap) {
		return false;
	}
	p = buf + *off;
	memcpy(p, &kl16, sizeof(kl16));
	memcpy(p + sizeof(kl16), key, key_len);
	memset(p + sizeof(kl16) + key_len, 0, entry - sizeof(kl16) - key_len);
	*off += entry;
	return true;
}

static int
nkvx_list(struct nkvx_executor *ex, const nkvx_kv_in_t *in,
	  struct nkvx_exec_result *res)
{
	uint32_t osize = in->osize;
	uint32_t cap = (osize > NKVX_LIST_NRK_SIZE) ? osize : NKVX_LIST_NRK_SIZE;
	uint8_t *buf;
	uint32_t off = NKVX_LIST_NRK_SIZE;	/* reserve the NRK header */
	uint32_t nrk = 0;
	uint32_t full_len = NKVX_LIST_NRK_SIZE;	/* TRUE listing length (all keys) */
	bool overflow = false;
	char start_oid[SPDK_KVDEV_EXEC_KEY_MAX_LEN * 2 + 1] = "";
	bool started = (in->key_len == 0);	/* key_len 0 => from the first key */

	if (in->key_len > 0) {
		nkvx_key_to_oid(in->key, in->key_len, start_oid);
	}

	buf = calloc(1, cap);
	if (buf == NULL) {
		return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_NOMEM, 0, NULL, 0);
	}

	if (ex->mem) {
		/* Stable order over a snapshot: insertion order is not stable across
		 * delete-compaction, so sort the oids lexicographically (matches the
		 * RADOS object enumeration order closely enough for the loopback test). */
		int i, j;

		for (i = 0; i < ex->mem_count; i++) {
			for (j = i + 1; j < ex->mem_count; j++) {
				if (strcmp(ex->mem_objs[i].oid, ex->mem_objs[j].oid) > 0) {
					struct nkvx_mem_obj t = ex->mem_objs[i];
					ex->mem_objs[i] = ex->mem_objs[j];
					ex->mem_objs[j] = t;
				}
			}
		}
		for (i = 0; i < ex->mem_count; i++) {
			uint8_t key[SPDK_KVDEV_EXEC_KEY_MAX_LEN];
			uint8_t klen;

			if (!started) {
				/* Start position is exclusive of keys before start_oid. */
				if (strcmp(ex->mem_objs[i].oid, start_oid) < 0) {
					continue;
				}
				started = true;
			}
			klen = nkvx_oid_to_key(ex->mem_objs[i].oid, key);
			if (klen == 0) {
				continue;	/* skip a non-KV object */
			}
			full_len += nkvx_list_entry_size(klen);
			if (!overflow && nkvx_list_emit(buf, cap, &off, key, klen)) {
				nrk++;
			} else {
				overflow = true;
			}
		}
	} else {
		rados_list_ctx_t lctx;
		int rc = rados_nobjects_list_open(ex->ioctx, &lctx);
		const char *entry;

		if (rc < 0) {
			free(buf);
			fprintf(stderr, "nkvx_executor: LIST open failed: %s\n", strerror(-rc));
			return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
		}
		while ((rc = rados_nobjects_list_next(lctx, &entry, NULL, NULL)) == 0) {
			uint8_t key[SPDK_KVDEV_EXEC_KEY_MAX_LEN];
			uint8_t klen;

			if (!started) {
				if (strcmp(entry, start_oid) < 0) {
					continue;
				}
				started = true;
			}
			klen = nkvx_oid_to_key(entry, key);
			if (klen == 0) {
				continue;
			}
			full_len += nkvx_list_entry_size(klen);
			if (!overflow && nkvx_list_emit(buf, cap, &off, key, klen)) {
				nrk++;
			} else {
				overflow = true;
			}
		}
		rados_nobjects_list_close(lctx);
		if (rc != -ENOENT) {
			free(buf);
			fprintf(stderr, "nkvx_executor: LIST next failed: %s\n", strerror(-rc));
			return nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
		}
	}

	/* Patch the NRK header (number of keys that fully fit in osize). */
	memcpy(buf, &nrk, sizeof(nrk));

	/*
	 * result_len is the FULL listing byte length (CQE DW0). When it exceeds
	 * osize the host buffer was too small for the complete listing —
	 * BUFFER_TOO_SMALL, the delivered body holds the prefix that fit (off bytes).
	 */
	res->status = (full_len > osize) ? SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL
					 : SPDK_KVDEV_IO_STATUS_SUCCESS;
	res->result_len = full_len;
	res->buf = buf;			/* adopt (no second copy) */
	res->buf_len = off;
	return 0;
}

int
nkvx_executor_kv(struct nkvx_executor *ex, const nkvx_kv_in_t *in,
		 struct nkvx_exec_result *res)
{
	char oid[SPDK_KVDEV_EXEC_KEY_MAX_LEN * 2 + 1];

	memset(res, 0, sizeof(*res));

	if (ex == NULL || in == NULL) {
		nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_FAILED, 0, NULL, 0);
		return -EINVAL;
	}

	switch (in->verb) {
	case NKVX_KV_VERB_RETRIEVE:
		/* Retrieve requires a non-empty key (List may start at key_len 0; the
		 * other verbs land in S4). */
		if (in->key_len == 0) {
			nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_INVALID, 0, NULL, 0);
			return 0;
		}
		nkvx_key_to_oid(in->key, in->key_len, oid);
		return nkvx_retrieve(ex, oid, in, res);
	case NKVX_KV_VERB_STORE:
		if (in->key_len == 0) {
			nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_INVALID, 0, NULL, 0);
			return 0;
		}
		nkvx_key_to_oid(in->key, in->key_len, oid);
		return nkvx_store(ex, oid, in, res);
	case NKVX_KV_VERB_DELETE:
		if (in->key_len == 0) {
			nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_INVALID, 0, NULL, 0);
			return 0;
		}
		nkvx_key_to_oid(in->key, in->key_len, oid);
		return nkvx_delete(ex, oid, in, res);
	case NKVX_KV_VERB_EXIST:
		if (in->key_len == 0) {
			nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_INVALID, 0, NULL, 0);
			return 0;
		}
		nkvx_key_to_oid(in->key, in->key_len, oid);
		return nkvx_exist(ex, oid, in, res);
	case NKVX_KV_VERB_LIST:
		/* List may start at key_len 0 (from the first key); no oid required. */
		return nkvx_list(ex, in, res);
	default:
		/* NKVX_KV_VERB_INVALID or an unknown verb: never silently allowed. */
		nkvx_result_set(res, SPDK_KVDEV_IO_STATUS_INVALID, 0, NULL, 0);
		return 0;
	}
}
