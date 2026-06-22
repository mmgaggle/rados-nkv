#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Launcher for the rados-nkv-client image: exec the `rkv` CLI with whatever
# args the container was run with, so `podman run …/rados-nkv-client <rkv args>`
# behaves like running `rkv <rkv args>` directly, e.g.:
#   podman run --rm …/rados-nkv-client store myns/k1 -i /data
set -euo pipefail

# rkv's ~/.rados-nkv.conf loader keys off $HOME; default to /root so the conf is
# found at a stable path in the (rootful-by-default) image.
export HOME="${HOME:-/root}"

# SPDK root the binary may consult for runtime assets. The static SPDK libs are
# linked in, but keep a sane default in case rkv resolves data relative to it.
export SPDK_ROOT="${SPDK_ROOT:-/usr}"

# Optional default args, prepended before the user's args (e.g. a fixed
# --socket-dir or --verbose). Word-split intentionally.
if [ -n "${RKV_DEFAULT_ARGS:-}" ]; then
	# shellcheck disable=SC2086
	exec rkv $RKV_DEFAULT_ARGS "$@"
fi

exec rkv "$@"
