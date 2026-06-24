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
 * The bdev_kvrados_create request struct + decoders + free are GENERATED from
 * schema.json into rpc_autogen.h (rpc_bdev_kvrados_create_decoders_autogen /
 * rpc_bdev_kvrados_create_ctx / free_rpc_bdev_kvrados_create). The
 * migrated_decoders set in genrpc.py keeps this file from redefining them.
 * That autogen struct also gives us the structured KV Exec allowlist
 * (rpc_nvmf_kv_exec_allowlist), which we translate into the bdev's
 * kvrados_exec_binding array below.
 */
#include "spdk_internal/rpc_autogen.h"

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

	if (params && spdk_json_decode_object(params, rpc_bdev_kvrados_create_decoders_autogen,
					      SPDK_COUNTOF(rpc_bdev_kvrados_create_decoders_autogen),
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
 * Delete keeps a hand-rolled decoder (single 'name' param). The struct/free are
 * named *_req (not the autogen *_ctx / free_rpc_bdev_kvrados_delete in
 * rpc_autogen.h, which this TU includes) to avoid a redefinition collision.
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
	{"name", offsetof(struct rpc_bdev_kvrados_delete_req, name), spdk_json_decode_string},
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
