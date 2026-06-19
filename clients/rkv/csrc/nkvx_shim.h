/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Flat extern "C" FFI boundary for the rados-nkv Rust CLI (bead spdk-jhk.7.1).
 * Wraps the proven raw vfio-user NVMe-KV driver (nkv_vfu.h) behind an opaque
 * session handle so Rust never sees struct nvfu_dev or SPDK types directly.
 *
 * All ops return 0 on success and a negative errno (or -EIO) on failure.
 */

#ifndef NKVX_SHIM_H
#define NKVX_SHIM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nkvx_session nkvx_session;

/* spdk_env_init(--iova-mode=va) [once per process] + nvfu_open(traddr_dir). */
nkvx_session *nkvx_open(const char *traddr_dir);
void          nkvx_close(nkvx_session *s);

/* store_opt is the CDW11 Store Option byte (SIKE/SINKE, TTL_VALID, EPHEMERAL,
 * TOUCH); ttl the CDW12 TTL in seconds, honoured only when TTL_VALID is set in
 * store_opt. Pass store_opt=0, ttl=0 for a plain unconditional store. */
int nkvx_store(nkvx_session *s, uint32_t nsid, const char *key,
	       const void *val, uint32_t len, uint8_t store_opt, uint32_t ttl);
int nkvx_retrieve(nkvx_session *s, uint32_t nsid, const char *key,
		  void *out, uint32_t out_len, uint32_t *got);
int nkvx_exec(nkvx_session *s, uint32_t nsid, const char *key, uint32_t op_id,
	      const void *in, uint32_t in_len,
	      void *out, uint32_t out_len, uint32_t *rlen);

#ifdef __cplusplus
}
#endif

#endif /* NKVX_SHIM_H */
