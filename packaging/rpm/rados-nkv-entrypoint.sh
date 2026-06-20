#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# rados-nkv container/service entrypoint — one image, two roles, selected by
# $NKV_ROLE (ADR bead spdk-jhk.14 §4). Both the systemd units and the cephadm-
# generated `podman run` units invoke this same launcher, so the per-role
# bring-up lives in exactly one place.
#
#   NKV_ROLE=nkv   the front/target: start nvmf_tgt, then `rados-nkv up`
#   NKV_ROLE=nkvx  the executor: nkvx_service (Mercury Exec target)
#
# Every knob has an env default; the systemd EnvironmentFile (/etc/sysconfig/
# rados-nkv{,x}) or the cephadm daemon spec overrides them.
set -euo pipefail

log() { echo "rados-nkv[$NKV_ROLE]: $*" >&2; }

# ---------------------------------------------------------------------------
# nkv — the NVMe-KV target front: nvmf_tgt (the long-running daemon) + the
# one-shot `rados-nkv up` RPC config once its socket is listening.
# ---------------------------------------------------------------------------
start_nkv() {
	local nvmf_tgt="${NVMF_TGT:-/usr/local/bin/nvmf_tgt}"
	local rpc_sock="${RPC_SOCK:-/var/run/spdk.sock}"
	# The installed rados-nkv script finds rpc.py under $SPDK_ROOT/scripts;
	# spdk-scripts ships it at /usr/libexec/spdk/scripts/rpc.py.
	export SPDK_ROOT="${SPDK_ROOT:-/usr/libexec/spdk}"
	export RPC_SOCK="$rpc_sock"

	[ -x "$nvmf_tgt" ] || { log "nvmf_tgt not found at $nvmf_tgt (install spdk)"; exit 127; }
	rm -f "$rpc_sock"

	# NVMF_TGT_ARGS is an intentional word-split ops knob (e.g. "-m 0x1 -s 4096",
	# or "--no-huge -s 1024" for a hugepage-less dev box).
	# shellcheck disable=SC2086
	"$nvmf_tgt" -r "$rpc_sock" ${NVMF_TGT_ARGS:-} &
	local tgt_pid=$!
	trap 'log "stopping"; kill -TERM "$tgt_pid" 2>/dev/null || true' TERM INT

	# Wait for the RPC socket — but fail fast if the target died on startup.
	local tries="${RPC_WAIT_TRIES:-100}"
	while [ ! -S "$rpc_sock" ]; do
		kill -0 "$tgt_pid" 2>/dev/null || { log "nvmf_tgt exited before its RPC socket appeared"; wait "$tgt_pid"; exit 1; }
		tries=$((tries - 1)); [ "$tries" -le 0 ] && { log "timed out waiting for $rpc_sock"; kill -TERM "$tgt_pid"; exit 1; }
		sleep 0.1
	done

	log "nvmf_tgt up (pid $tgt_pid); applying KV target config"
	rados-nkv up

	# Hand the foreground to the daemon and forward its exit/signals.
	wait "$tgt_pid"
}

# ---------------------------------------------------------------------------
# nkvx — the Exec executor: a single foreground Mercury target process.
# ---------------------------------------------------------------------------
start_nkvx() {
	local -a args=( --listen "${NKVX_LISTEN:-na+sm://}" )
	[ -n "${NKVX_ADDR_FILE:-}" ]      && args+=( --addr-file      "$NKVX_ADDR_FILE" )
	[ -n "${NKVX_RADOS_POOL:-}" ]     && args+=( --rados-pool      "$NKVX_RADOS_POOL" )
	[ -n "${NKVX_RADOS_NAMESPACE:-}" ]&& args+=( --rados-namespace "$NKVX_RADOS_NAMESPACE" )
	[ -n "${NKVX_RADOS_CONF:-}" ]     && args+=( --rados-conf      "$NKVX_RADOS_CONF" )
	[ -n "${NKVX_RADOS_USER:-}" ]     && args+=( --rados-user      "$NKVX_RADOS_USER" )
	log "exec nkvx_service ${args[*]}"
	exec nkvx_service "${args[@]}"
}

case "${NKV_ROLE:-}" in
	nkv)  start_nkv ;;
	nkvx) start_nkvx ;;
	*) echo "rados-nkv entrypoint: set NKV_ROLE=nkv|nkvx (got '${NKV_ROLE:-}')" >&2; exit 64 ;;
esac
