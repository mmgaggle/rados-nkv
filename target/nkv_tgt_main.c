/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   Copyright (C) 2026 rados-nkv contributors.
 *   All rights reserved.
 */

/*
 * Custom NVMe-oF target application for the out-of-tree KV bdev forwarder.
 *
 * This is a minimal spdk_app_start() wrapper (adapted from SPDK's
 * app/nvmf_tgt/nvmf_main.c). It links the bdev_kvrados module objects plus the
 * combined libspdk.so. The module's SPDK_BDEV_MODULE_REGISTER and
 * SPDK_RPC_REGISTER constructors self-register at load, so the kvrados bdev and
 * its RPCs (bdev_kvrados_create, etc.) become available over the JSON-RPC
 * socket without any explicit wiring here. Mirrors the compose "nkv" service.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/event.h"

static void
nkv_tgt_usage(void)
{
}

static int
nkv_tgt_parse_arg(int ch, char *arg)
{
	return 0;
}

static void
nkv_tgt_started(void *arg1)
{
	if (getenv("MEMZONE_DUMP") != NULL) {
		spdk_memzone_dump(stdout);
		fflush(stdout);
	}
}

int
main(int argc, char **argv)
{
	int rc;
	struct spdk_app_opts opts = {};

	/* default value in opts */
	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "nkv_tgt";
	if ((rc = spdk_app_parse_args(argc, argv, &opts, "", NULL,
				      nkv_tgt_parse_arg, nkv_tgt_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, nkv_tgt_started, NULL);
	spdk_app_fini();
	return rc;
}
