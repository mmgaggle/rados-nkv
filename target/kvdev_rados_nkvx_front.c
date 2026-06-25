/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/*
 * Two-tier front bridge (Slice C4): adapts the SPDK kvdev_rados datapath to the
 * Mercury front client core. Compiled only under NKVX_WITH_MERCURY (our
 * out-of-tree build knob; target/Makefile adds this source and passes
 * -DNKVX_WITH_MERCURY when Mercury is enabled). See kvdev_rados_nkvx_front.h
 * and docs/design/slice-c-exec-rpc-mercury.md §2/§4.
 */

#include "kvdev_rados_nkvx_front.h"
#include "nkvx_front_client.h"		/* struct nkvx_front, nkvx_exec_in_t, NKVX_INLINE_MAX */

#include "spdk/log.h"
#include "spdk/util.h"			/* spdk_min */

/*
 * Per-forward completion context. Outlives kvdev_rados_nkvx_front_forward() and
 * is freed in the done trampoline (which runs from a progress tick on the
 * reactor thread, design §4.2).
 */
struct kvdev_rados_nkvx_fwd_ctx {
	spdk_kvdev_io_completion_cb	cb_fn;
	void				*cb_arg;
	void				*host_out;
	uint32_t			host_out_len;
};

/*
 * Mercury-side completion: copy the inline result into the tenant's output
 * buffer (truncated to its cap; the executor already truncated to osize, design
 * §1.2) and fire the kvdev completion with the TRUE result length.
 */
static void
kvdev_rados_nkvx_front_done(void *arg, enum spdk_kvdev_io_status status,
			    uint32_t result_len, const void *result_inline,
			    uint32_t result_inline_len)
{
	struct kvdev_rados_nkvx_fwd_ctx *ctx = arg;

	if (result_inline != NULL && result_inline_len > 0 &&
	    ctx->host_out != NULL && ctx->host_out_len > 0) {
		uint32_t n = spdk_min(result_inline_len, ctx->host_out_len);
		memcpy(ctx->host_out, result_inline, n);
	}

	/*
	 * status is passed straight through (correct: ABORTED maps to the tenant
	 * ABORTED_BY_REQUEST). NB a cancel-induced ABORTED (channel-destroy teardown)
	 * shares the tenant CQE status with a resource-cap ABORTED — telemetry only,
	 * no functional difference at the tenant.
	 */
	ctx->cb_fn(ctx->cb_arg, status, result_len);
	free(ctx);
}

int
kvdev_rados_nkvx_front_create(const char *target_addr, struct nkvx_front **out)
{
	const char *sep;
	char na_init[64];
	size_t n;

	if (target_addr == NULL || out == NULL) {
		return -EINVAL;
	}

	/* Derive the origin NA init/provider string from the address prefix:
	 * everything up to and including "://" (e.g. "ofi+tcp://h:p" -> "ofi+tcp://",
	 * "na+sm://7-0" -> "na+sm://"). */
	sep = strstr(target_addr, "://");
	if (sep == NULL) {
		SPDK_ERRLOG("nkvx front: malformed executor address '%s' (no '://')\n",
			    target_addr);
		return -EINVAL;
	}
	n = (size_t)(sep - target_addr) + 3;
	if (n + 1 > sizeof(na_init)) {
		SPDK_ERRLOG("nkvx front: executor provider prefix too long in '%s'\n",
			    target_addr);
		return -EINVAL;
	}
	memcpy(na_init, target_addr, n);
	na_init[n] = '\0';

	return nkvx_front_init(na_init, target_addr, out);
}

void
kvdev_rados_nkvx_front_destroy(struct nkvx_front *front)
{
	nkvx_front_fini(front);
}

int
kvdev_rados_nkvx_front_progress(struct nkvx_front *front)
{
	/* Non-blocking: timeout 0 so the reactor poller never stalls (design §4.2). */
	return nkvx_front_progress(front, 0);
}

int
kvdev_rados_nkvx_front_drain_progress(struct nkvx_front *front, unsigned int timeout_ms)
{
	/* Channel-destroy ONLY: the channel is being torn down (no more datapath on
	 * this reactor), so a small BLOCKING progress is fine and is what avoids the
	 * busy-spin of timeout-0 polling while we wait for cancelled forwards to reach
	 * a terminal completion. Never called on the hot datapath. */
	return nkvx_front_progress(front, timeout_ms);
}

int
kvdev_rados_nkvx_front_forward(struct nkvx_front *front,
			       const void *key, uint8_t key_len,
			       uint32_t op_id, bool read_only,
			       uint8_t runtime,
			       const char *module_key, const char *module_ns,
			       const uint8_t *sha256, bool sha256_valid,
			       uint64_t caps,
			       const void *input, uint32_t input_len,
			       void *output_buf, uint32_t output_buf_len,
			       spdk_kvdev_io_completion_cb cb_fn, void *cb_arg,
			       uint64_t *out_token)
{
	struct kvdev_rados_nkvx_fwd_ctx *ctx;
	nkvx_exec_in_t in;
	int rc;

	if (out_token != NULL) {
		*out_token = KVDEV_RADOS_NKVX_TOKEN_NONE;
	}

	/* key_len is a uint8_t and SPDK_KVDEV_EXEC_KEY_MAX_LEN == 255, so it cannot
	 * exceed the cap; only the empty-key case is invalid here. */
	if (front == NULL || cb_fn == NULL || key_len == 0) {
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->host_out = output_buf;
	ctx->host_out_len = output_buf_len;

	memset(&in, 0, sizeof(in));
	in.op_id = op_id;
	in.read_only = read_only ? 1 : 0;
	in.runtime = runtime;
	in.caps = caps;
	in.key_len = key_len;
	memcpy(in.key, key, key_len);
	if (sha256 != NULL && sha256_valid) {
		memcpy(in.sha256, sha256, SPDK_KV_EXEC_SHA256_LEN);
	}
	in.sha256_valid = sha256_valid ? 1 : 0;
	in.module_key = (char *)module_key;	/* borrowed; encoded synchronously */
	in.module_ns = (char *)module_ns;
	in.osize = output_buf_len;
	in.input_len = input_len;
	in.input_inline = (void *)input;	/* borrowed; registered/encoded synchronously */
	in.input_bulk = HG_BULK_NULL;		/* originated by nkvx_front_forward */
	in.result_sink = HG_BULK_NULL;		/* originated by nkvx_front_forward */

	/*
	 * Pass the tenant's host output buffer as the result sink (Slice C7): when it
	 * can hold a large result (> NKVX_INLINE_MAX) the front client registers it
	 * WRITE-mode and the executor PUSHes straight into it. A small result still
	 * comes back inline and the done-cb copies it into host_out. Either way the
	 * bytes land in this same buffer — no double delivery (the executor inlines
	 * XOR pushes), and the done-cb's memcpy is a no-op on the push path (inline
	 * is empty). Large input is likewise registered from in.input_inline.
	 *
	 * UNCACHED variant (bead spdk-4i7): this is the PER-COMMAND forwarder. The
	 * output_buf (a per-command vfio-user DMA region the bdev scatters from) and the
	 * large-input source (a per-op heap bounce) are TRANSIENT — their VA can be
	 * reused across ops with different backing pages — so they must bypass the
	 * VA-keyed C7.2 handle cache, or a stale sticky MR makes the executor RDMA the
	 * wrong/zero pages (the KV path already registers uncached for the same reason).
	 */
	rc = nkvx_front_forward_tok_uncached(front, &in, output_buf, output_buf_len,
					     kvdev_rados_nkvx_front_done, ctx, out_token);
	if (rc != 0) {
		free(ctx);
		return rc;
	}
	return 0;
}

int
kvdev_rados_nkvx_front_forward_dmabuf(struct nkvx_front *front,
				      const void *key, uint8_t key_len,
				      uint32_t op_id, bool read_only,
				      uint8_t runtime,
				      const char *module_key, const char *module_ns,
				      const uint8_t *sha256, bool sha256_valid,
				      uint64_t caps,
				      const void *input, uint32_t input_len,
				      void *sink_va, uint32_t sink_len,
				      int sink_fd, uint64_t sink_offset,
				      spdk_kvdev_io_completion_cb cb_fn, void *cb_arg,
				      uint64_t *out_token)
{
	struct kvdev_rados_nkvx_fwd_ctx *ctx;
	nkvx_exec_in_t in;
	int rc;

	if (out_token != NULL) {
		*out_token = KVDEV_RADOS_NKVX_TOKEN_NONE;
	}

	/* A dma-buf forward must carry a valid fd; key_len bounds as for the VA path. */
	if (front == NULL || cb_fn == NULL || key_len == 0 || sink_fd < 0) {
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	/*
	 * host_out is the CPU buffer the done-cb copies an INLINE result into. For a
	 * dma-buf (VRAM) sink there is no CPU VA, so leave it NULL: a large result is
	 * PUSHed straight into the dma-buf and the inline-copy is a no-op (inline is
	 * empty on the push path). sink_va is the dma-buf segment's guest IOVA: the
	 * advertised VA used as the verbs dma-buf MR base (load-bearing, never derefed).
	 */
	ctx->host_out = NULL;
	ctx->host_out_len = 0;

	memset(&in, 0, sizeof(in));
	in.op_id = op_id;
	in.read_only = read_only ? 1 : 0;
	in.runtime = runtime;
	in.caps = caps;
	in.key_len = key_len;
	memcpy(in.key, key, key_len);
	if (sha256 != NULL && sha256_valid) {
		memcpy(in.sha256, sha256, SPDK_KV_EXEC_SHA256_LEN);
	}
	in.sha256_valid = sha256_valid ? 1 : 0;
	in.module_key = (char *)module_key;	/* borrowed; encoded synchronously */
	in.module_ns = (char *)module_ns;
	in.osize = sink_len;
	in.input_len = input_len;
	in.input_inline = (void *)input;	/* borrowed; registered/encoded synchronously */
	in.input_bulk = HG_BULK_NULL;		/* originated by the forward */
	in.result_sink = HG_BULK_NULL;		/* originated by the forward */

	/*
	 * B-i V2: register the result sink from the dma-buf fd (HG_Bulk_create_attr
	 * with {mem_type=HG_MEM_TYPE_HOST, dmabuf_fd, dmabuf_offset}); the executor
	 * RDMA-WRITEs the result straight into the dma-buf-backed VRAM region. The
	 * dma-buf handle bypasses the VA-keyed bulk cache (it is keyed by (fd,offset)).
	 */
	rc = nkvx_front_forward_dmabuf(front, &in, sink_va, sink_len,
				       sink_fd, sink_offset,
				       kvdev_rados_nkvx_front_done, ctx, out_token);
	if (rc != 0) {
		free(ctx);
		return rc;
	}
	return 0;
}

int
kvdev_rados_nkvx_front_retrieve(struct nkvx_front *front,
				const void *key, uint8_t key_len,
				bool read_only,
				void *output_buf, uint32_t output_buf_len,
				spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_nkvx_fwd_ctx *ctx;
	nkvx_kv_in_t in;
	int rc;

	/* key_len is uint8_t and SPDK_KVDEV_EXEC_KEY_MAX_LEN == 255, so it cannot
	 * exceed the cap; only the empty-key case is invalid for Retrieve. */
	if (front == NULL || cb_fn == NULL || key_len == 0) {
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->host_out = output_buf;
	ctx->host_out_len = output_buf_len;

	memset(&in, 0, sizeof(in));
	in.verb = NKVX_KV_VERB_RETRIEVE;
	in.read_only = read_only ? 1 : 0;
	in.key_len = key_len;
	memcpy(in.key, key, key_len);
	in.osize = output_buf_len;
	in.value_inline = NULL;			/* no STORE value on a Retrieve */
	in.value_bulk = HG_BULK_NULL;		/* originated by the forward */
	in.result_sink = HG_BULK_NULL;		/* originated by the forward */

	/*
	 * Pass the tenant host output buffer as the result sink (S3): a large value
	 * (> NKVX_INLINE_MAX) is PUSHed straight into it; a small value comes back
	 * inline and kvdev_rados_nkvx_front_done copies it into host_out. Same single
	 * buffer either way — the executor inlines XOR pushes (no double delivery).
	 */
	rc = nkvx_front_kv_forward(front, &in, output_buf, output_buf_len,
				   kvdev_rados_nkvx_front_done, ctx);
	if (rc != 0) {
		free(ctx);
		return rc;
	}
	return 0;
}

int
kvdev_rados_nkvx_front_store(struct nkvx_front *front,
			     const void *key, uint8_t key_len,
			     bool read_only, uint8_t store_flags,
			     const void *value, uint32_t value_len,
			     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_nkvx_fwd_ctx *ctx;
	nkvx_kv_in_t in;
	int rc;

	if (front == NULL || cb_fn == NULL || key_len == 0) {
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->host_out = NULL;		/* Store has no result body */
	ctx->host_out_len = 0;

	memset(&in, 0, sizeof(in));
	in.verb = NKVX_KV_VERB_STORE;
	in.read_only = read_only ? 1 : 0;
	in.store_flags = store_flags;
	in.key_len = key_len;
	memcpy(in.key, key, key_len);
	in.value_len = value_len;
	/* value points at the value region (zero-copy SGL span or a bounce that outlives
	 * the RPC). A small value (<= NKVX_INLINE_MAX) rides inline; for a large value
	 * nkvx_front_kv_forward registers this region READ-mode as value_bulk and the
	 * executor RDMA-PULLs it. Either way the caller keeps value valid until the cb. */
	in.value_inline = (value_len > 0) ? (void *)value : NULL;
	in.value_bulk = HG_BULK_NULL;		/* originated by the forward */
	in.result_sink = HG_BULK_NULL;

	/* No result sink (Store returns only status). */
	rc = nkvx_front_kv_forward(front, &in, NULL, 0,
				   kvdev_rados_nkvx_front_done, ctx);
	if (rc != 0) {
		free(ctx);
		return rc;
	}
	return 0;
}

static int
kvdev_rados_nkvx_front_status_only(struct nkvx_front *front,
				   uint8_t verb, const void *key, uint8_t key_len,
				   bool read_only,
				   spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_nkvx_fwd_ctx *ctx;
	nkvx_kv_in_t in;
	int rc;

	if (front == NULL || cb_fn == NULL || key_len == 0) {
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->host_out = NULL;		/* no result body (Exist echoes len in DW0) */
	ctx->host_out_len = 0;

	memset(&in, 0, sizeof(in));
	in.verb = verb;
	in.read_only = read_only ? 1 : 0;
	in.key_len = key_len;
	memcpy(in.key, key, key_len);
	in.value_inline = NULL;
	in.value_bulk = HG_BULK_NULL;
	in.result_sink = HG_BULK_NULL;

	rc = nkvx_front_kv_forward(front, &in, NULL, 0,
				   kvdev_rados_nkvx_front_done, ctx);
	if (rc != 0) {
		free(ctx);
		return rc;
	}
	return 0;
}

int
kvdev_rados_nkvx_front_delete(struct nkvx_front *front,
			      const void *key, uint8_t key_len,
			      bool read_only,
			      spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	return kvdev_rados_nkvx_front_status_only(front, NKVX_KV_VERB_DELETE,
						  key, key_len, read_only, cb_fn, cb_arg);
}

int
kvdev_rados_nkvx_front_exist(struct nkvx_front *front,
			     const void *key, uint8_t key_len,
			     bool read_only,
			     spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	return kvdev_rados_nkvx_front_status_only(front, NKVX_KV_VERB_EXIST,
						  key, key_len, read_only, cb_fn, cb_arg);
}

int
kvdev_rados_nkvx_front_list(struct nkvx_front *front,
			    const void *start_key, uint8_t start_key_len,
			    bool read_only,
			    void *output_buf, uint32_t output_buf_len,
			    spdk_kvdev_io_completion_cb cb_fn, void *cb_arg)
{
	struct kvdev_rados_nkvx_fwd_ctx *ctx;
	nkvx_kv_in_t in;
	int rc;

	if (front == NULL || cb_fn == NULL) {
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->host_out = output_buf;
	ctx->host_out_len = output_buf_len;

	memset(&in, 0, sizeof(in));
	in.verb = NKVX_KV_VERB_LIST;
	in.read_only = read_only ? 1 : 0;
	in.key_len = start_key_len;	/* 0 => list from the first key */
	if (start_key_len > 0) {
		memcpy(in.key, start_key, start_key_len);
	}
	in.osize = output_buf_len;
	in.value_inline = NULL;
	in.value_bulk = HG_BULK_NULL;
	in.result_sink = HG_BULK_NULL;

	/* Same inline-or-PUSH delivery as Retrieve: the listing lands in output_buf. */
	rc = nkvx_front_kv_forward(front, &in, output_buf, output_buf_len,
				   kvdev_rados_nkvx_front_done, ctx);
	if (rc != 0) {
		free(ctx);
		return rc;
	}
	return 0;
}

bool
kvdev_rados_nkvx_front_cancel(struct nkvx_front *front, uint64_t token)
{
	return nkvx_front_cancel(front, token);
}

void
kvdev_rados_nkvx_front_cancel_all(struct nkvx_front *front)
{
	nkvx_front_cancel_all(front);
}

unsigned
kvdev_rados_nkvx_front_outstanding(struct nkvx_front *front)
{
	return nkvx_front_outstanding(front);
}

void
kvdev_rados_nkvx_front_fail_all_pending(struct nkvx_front *front,
					enum spdk_kvdev_io_status status)
{
	nkvx_front_fail_all_pending(front, status);
}
