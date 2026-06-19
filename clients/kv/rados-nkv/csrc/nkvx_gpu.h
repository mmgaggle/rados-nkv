/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation. All rights reserved.
 */

/*
 * Native HIP GPU-initiated datapath FFI boundary (bead spdk-jhk.7.15), the
 * mirror of nkvx_shim.h for the in-process `--gpu` path. Only present in the
 * build when the `gpu-native` cargo feature is enabled. Every op returns 0 on
 * success and a negative errno (or -EIO) on failure; retrieve/exec report the
 * TRUE result length (unclamped) so the caller can size-probe + re-read.
 */

#ifndef NKVX_GPU_H
#define NKVX_GPU_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nkvx_gpu_session nkvx_gpu_session;

/* spdk_env_init(--iova-mode=va) [once/process] + nvfu_open + GPU SQ/doorbell
 * map; arms the GPU producer for IO ops. NULL if no HIP device or attach fails. */
nkvx_gpu_session *nkvx_gpu_open(const char *traddr_dir);
void              nkvx_gpu_close(nkvx_gpu_session *s);

int nkvx_gpu_store(nkvx_gpu_session *s, uint32_t nsid, const char *key,
		   const void *val, uint32_t len);
int nkvx_gpu_retrieve(nkvx_gpu_session *s, uint32_t nsid, const char *key,
		      void *out, uint32_t out_len, uint32_t *got);
int nkvx_gpu_exec(nkvx_gpu_session *s, uint32_t nsid, const char *key, uint32_t op_id,
		  const void *in, uint32_t in_len,
		  void *out, uint32_t out_len, uint32_t *rlen);

#ifdef __cplusplus
}
#endif

#endif /* NKVX_GPU_H */
