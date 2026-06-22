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

#include "nkvx_oid.h"		/* nkvx_key_to_oid / nkvx_deliver_len / nkvx_result_too_small
				 * (pure ADR-0014 contract helpers, unit-tested standalone) */

struct nkvx_executor {
	rados_t		cluster;
	rados_ioctx_t	ioctx;
};

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
		uint32_t deliver = nkvx_deliver_len(result_len, osize);
		enum spdk_kvdev_io_status st = nkvx_result_too_small(result_len, osize) ?
			SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL : SPDK_KVDEV_IO_STATUS_SUCCESS;

		return nkvx_result_set(res, st, result_len, &len, deliver);
	}

	if (strcmp(module, "inputecho") == 0) {
		uint32_t result_len = in->input_len;
		uint32_t deliver = nkvx_deliver_len(result_len, osize);
		enum spdk_kvdev_io_status st = nkvx_result_too_small(result_len, osize) ?
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
		uint32_t deliver = nkvx_deliver_len(result_len, osize);
		enum spdk_kvdev_io_status st = nkvx_result_too_small(result_len, osize) ?
			SPDK_KVDEV_IO_STATUS_BUFFER_TOO_SMALL : SPDK_KVDEV_IO_STATUS_SUCCESS;

		/* 8 B never exceeds NKVX_INLINE_MAX; always inline-deliverable. */
		return nkvx_result_set(res, st, result_len, &count, deliver);
	}

	if (strcmp(module, "identity") == 0) {
		uint32_t result_len = (size > UINT32_MAX) ? UINT32_MAX : (uint32_t)size;
		uint32_t deliver = nkvx_deliver_len(result_len, osize);
		/* True-size check (unclamped): an object larger than UINT32_MAX still
		 * overflows osize even though result_len saturates. */
		enum spdk_kvdev_io_status st = nkvx_result_too_small(size, osize) ?
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
