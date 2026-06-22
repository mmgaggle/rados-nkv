#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Launcher for the rados-nkv-client image. Three modes:
#
#   podman run …/rados-nkv-client store myns/k1 -i /data   # one-shot: exec `rkv <args>`
#   podman run -d …/rados-nkv-client                       # idle: stay up for shell-in
#   podman run -d -e RKV_DAEMON=1 …/rados-nkv-client       # idle + warm session daemon
#
# With no command the container idles (tail -f /dev/null) so a user can
#   podman exec -it <container> rkv <cmd>
# and run a SERIES of rkv commands without launching a container per call. With
# RKV_DAEMON=1 the entrypoint first starts rkv's persistent session daemon (one
# vfio-user attach); subsequent `rkv store/get/exec` auto-forward to it, so each
# command skips the per-process SPDK EAL init + controller attach.
set -euo pipefail

# rkv's ~/.rados-nkv.conf loader and the daemon socket path key off $HOME. The
# image also pins ENV HOME=/root so `podman exec`'d rkv calls resolve the SAME
# config + daemon socket as the entrypoint.
export HOME="${HOME:-/root}"

# SPDK root the binary may consult for runtime assets. The static SPDK libs are
# linked in, but keep a sane default in case rkv resolves data relative to it.
export SPDK_ROOT="${SPDK_ROOT:-/usr}"

# --- No command: keep the container alive for interactive `podman exec` use. ---
if [ "$#" -eq 0 ]; then
	if [ "${RKV_DAEMON:-0}" = "1" ]; then
		echo "rkv-entrypoint: starting persistent session daemon (RKV_DAEMON=1)" >&2
		# `daemon start` detaches a worker that does the vfio-user attach once.
		# Tolerate failure so the container still comes up for shell-in even if
		# the front/socket isn't ready yet (user can `rkv daemon start` later).
		rkv daemon start || \
			echo "rkv-entrypoint: 'rkv daemon start' failed; idling without a warm session" >&2
	fi
	echo "rkv-entrypoint: idling — run 'podman exec <container> rkv <cmd>'; stop with 'podman stop'" >&2
	exec tail -f /dev/null
fi

# --- A command was given: one-shot rkv. ---
# Optional default args, prepended before the user's args. Word-split intentionally.
if [ -n "${RKV_DEFAULT_ARGS:-}" ]; then
	# shellcheck disable=SC2086
	exec rkv $RKV_DEFAULT_ARGS "$@"
fi

exec rkv "$@"
