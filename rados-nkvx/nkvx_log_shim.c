/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation.
 *   All rights reserved.
 */

/*
 * Minimal spdk_log shim for the standalone (non-SPDK) executor (Slice C5a.2).
 *
 * The reused wasm runtime core (kvdev_rados_nkvx_wasm.c) logs via SPDK_*LOG,
 * which resolve to a single external symbol — spdk_log. The executor is not an
 * SPDK app and does not link libspdk_log, so we provide a tiny stderr-backed
 * implementation. This is the ONLY SPDK runtime symbol the wasm core needs
 * (verified by nm); everything else it uses is libc/openssl/dl/pthread.
 */

#include "spdk/log.h"

#include <stdarg.h>
#include <stdio.h>

void
spdk_log(enum spdk_log_level level, const char *file, const int line,
	 const char *func, const char *format, ...)
{
	va_list ap;

	(void)level;
	(void)file;

	fprintf(stderr, "nkvx/wasm[%s:%d] ", func ? func : "?", line);
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
}
