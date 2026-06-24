/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "bdev_kvrados.h"

#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/uuid.h"
#include "spdk/log.h"

/*
 * Out-of-tree build: this module links against UNMODIFIED SPDK, which does not
 * carry our bdev_kvrados RPC decoders (those used to be generated from
 * schema.json into an SPDK-internal build artifact). So the request structs,
 * JSON object decoders, and free helpers are all defined INLINE here, following the
 * standard out-of-tree SPDK bdev RPC pattern (spdk_json_decode_object with a
 * local spdk_json_object_decoder[]). Field names / types / optionality mirror
 * the original schema.json + N3 generated decoders exactly.
 */

/*
 * Structured KV Exec allowlist entry (ADR-0005/0012/0014). One JSON object per
 * permitted KV-Exec operation; decoded into the borrowed kvrados_exec_binding
 * view in rpc_bdev_kvrados_create() below (the disk deep-copies the strings).
 */
struct rpc_nvmf_kv_exec_allow {
	uint32_t	op_id;
	char		*binding;
	char		*runtime;
	char		*module_namespace;
	char		*module_key;
	char		*sha256;
	uint64_t	caps;
};

static const struct spdk_json_object_decoder rpc_nvmf_kv_exec_allow_decoders[] = {
	{"op_id", offsetof(struct rpc_nvmf_kv_exec_allow, op_id), spdk_json_decode_uint32, false},
	{"binding", offsetof(struct rpc_nvmf_kv_exec_allow, binding), spdk_json_decode_string, true},
	{"runtime", offsetof(struct rpc_nvmf_kv_exec_allow, runtime), spdk_json_decode_string, true},
	{"module_namespace", offsetof(struct rpc_nvmf_kv_exec_allow, module_namespace), spdk_json_decode_string, true},
	{"module_key", offsetof(struct rpc_nvmf_kv_exec_allow, module_key), spdk_json_decode_string, true},
	{"sha256", offsetof(struct rpc_nvmf_kv_exec_allow, sha256), spdk_json_decode_string, true},
	{"caps", offsetof(struct rpc_nvmf_kv_exec_allow, caps), spdk_json_decode_uint64, true},
};

static int
rpc_decode_nvmf_kv_exec_allow(const struct spdk_json_val *val, void *out)
{
	return spdk_json_decode_object(val, rpc_nvmf_kv_exec_allow_decoders,
				       SPDK_COUNTOF(rpc_nvmf_kv_exec_allow_decoders), out);
}

static void
free_rpc_nvmf_kv_exec_allow(struct rpc_nvmf_kv_exec_allow *req)
{
	free(req->binding);
	free(req->runtime);
	free(req->module_namespace);
	free(req->module_key);
	free(req->sha256);
}

#define RPC_NVMF_KV_EXEC_ALLOWLIST_MAX 256

struct rpc_nvmf_kv_exec_allowlist {
	size_t				count;
	struct rpc_nvmf_kv_exec_allow	items[RPC_NVMF_KV_EXEC_ALLOWLIST_MAX];
};

static int
rpc_decode_nvmf_kv_exec_allowlist(const struct spdk_json_val *val, void *out)
{
	struct rpc_nvmf_kv_exec_allowlist *arr = out;

	return spdk_json_decode_array(val, rpc_decode_nvmf_kv_exec_allow, arr->items,
				      RPC_NVMF_KV_EXEC_ALLOWLIST_MAX, &arr->count,
				      sizeof(arr->items[0]));
}

static void
free_rpc_nvmf_kv_exec_allowlist(struct rpc_nvmf_kv_exec_allowlist *arr)
{
	size_t i;

	for (i = 0; i < arr->count; i++) {
		free_rpc_nvmf_kv_exec_allow(&arr->items[i]);
	}
}

struct rpc_bdev_kvrados_create_ctx {
	char				*name;
	struct spdk_uuid		uuid;
	uint32_t			max_key_size;
	uint32_t			max_value_size;
	uint32_t			optimal_value_granularity;
	uint64_t			num_keys;
	char				*executor_endpoint;
	bool				read_only;
	struct rpc_nvmf_kv_exec_allowlist exec_allowlist;
	int32_t				numa_id;
};

static void
free_rpc_bdev_kvrados_create(struct rpc_bdev_kvrados_create_ctx *req)
{
	free(req->name);
	free(req->executor_endpoint);
	free_rpc_nvmf_kv_exec_allowlist(&req->exec_allowlist);
}

static const struct spdk_json_object_decoder rpc_bdev_kvrados_create_decoders[] = {
	{"name", offsetof(struct rpc_bdev_kvrados_create_ctx, name), spdk_json_decode_string, true},
	{"uuid", offsetof(struct rpc_bdev_kvrados_create_ctx, uuid), spdk_json_decode_uuid, true},
	{"max_key_size", offsetof(struct rpc_bdev_kvrados_create_ctx, max_key_size), spdk_json_decode_uint32, true},
	{"max_value_size", offsetof(struct rpc_bdev_kvrados_create_ctx, max_value_size), spdk_json_decode_uint32, true},
	{"optimal_value_granularity", offsetof(struct rpc_bdev_kvrados_create_ctx, optimal_value_granularity), spdk_json_decode_uint32, true},
	{"num_keys", offsetof(struct rpc_bdev_kvrados_create_ctx, num_keys), spdk_json_decode_uint64, true},
	{"executor_endpoint", offsetof(struct rpc_bdev_kvrados_create_ctx, executor_endpoint), spdk_json_decode_string, true},
	{"read_only", offsetof(struct rpc_bdev_kvrados_create_ctx, read_only), spdk_json_decode_bool, true},
	{"exec_allowlist", offsetof(struct rpc_bdev_kvrados_create_ctx, exec_allowlist), rpc_decode_nvmf_kv_exec_allowlist, true},
	{"numa_id", offsetof(struct rpc_bdev_kvrados_create_ctx, numa_id), spdk_json_decode_int32, true},
};

static void
rpc_bdev_kvrados_create(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_bdev_kvrados_create_ctx req = {};
	struct kvrados_bdev_opts opts = {};
	struct kvrados_exec_binding *allowlist = NULL;
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	size_t i;
	int rc = 0;

	req.numa_id = SPDK_ENV_NUMA_ID_ANY;

	if (params && spdk_json_decode_object(params, rpc_bdev_kvrados_create_decoders,
					      SPDK_COUNTOF(rpc_bdev_kvrados_create_decoders),
					      &req)) {
		SPDK_DEBUGLOG(bdev_kvrados, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* Map the structured allowlist (op_id + ADR-0012/0014 binding fields) onto the
	 * bdev's borrowed kvrados_exec_binding view; the disk deep-copies the strings. */
	if (req.exec_allowlist.count > 0) {
		allowlist = calloc(req.exec_allowlist.count, sizeof(*allowlist));
		if (!allowlist) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Out of memory");
			goto cleanup;
		}
		for (i = 0; i < req.exec_allowlist.count; i++) {
			struct rpc_nvmf_kv_exec_allow *src = &req.exec_allowlist.items[i];

			allowlist[i].op_id = src->op_id;
			allowlist[i].binding = src->binding;
			allowlist[i].runtime = src->runtime;
			allowlist[i].module_namespace = src->module_namespace;
			allowlist[i].module_key = src->module_key;
			allowlist[i].sha256 = src->sha256;
			allowlist[i].caps = src->caps;
		}
	}

	opts.name = req.name;
	opts.uuid = req.uuid;
	opts.max_key_size = req.max_key_size;
	opts.max_value_size = req.max_value_size;
	opts.optimal_value_granularity = req.optimal_value_granularity;
	opts.num_keys = req.num_keys;
	opts.executor_endpoint = req.executor_endpoint;
	opts.read_only = req.read_only;
	opts.exec_allowlist = allowlist;
	opts.exec_allowlist_count = req.exec_allowlist.count;
	opts.numa_id = req.numa_id;

	rc = create_kvrados_disk(&bdev, &opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free(allowlist);		/* the kvrados_exec_binding view; strings are freed below */
	free_rpc_bdev_kvrados_create(&req);
}
SPDK_RPC_REGISTER("bdev_kvrados_create", rpc_bdev_kvrados_create, SPDK_RPC_RUNTIME)

/*
 * Delete: single 'name' param (required), hand-rolled inline decoder.
 */
struct rpc_bdev_kvrados_delete_req {
	char *name;
};

static void
free_rpc_bdev_kvrados_delete_req(struct rpc_bdev_kvrados_delete_req *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_kvrados_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_kvrados_delete_req, name), spdk_json_decode_string, false},
};

static void
rpc_bdev_kvrados_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_kvrados_delete(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_bdev_kvrados_delete_req req = {};

	if (spdk_json_decode_object(params, rpc_bdev_kvrados_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_kvrados_delete_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_kvrados, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	delete_kvrados_disk(req.name, rpc_bdev_kvrados_delete_cb, request);

cleanup:
	free_rpc_bdev_kvrados_delete_req(&req);
}
SPDK_RPC_REGISTER("bdev_kvrados_delete", rpc_bdev_kvrados_delete, SPDK_RPC_RUNTIME)
