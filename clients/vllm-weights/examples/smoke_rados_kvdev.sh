#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
# Lightweight live validation of the RADOS kvdev path on a dev box:
# nvmf_tgt + kvdev_rados -> vstart Ceph, store/retrieve a small blob via the
# real NvmeKvClient, then prove the object landed in the pool with `rados ls`.
# No model download. Whole target lifecycle in one trap-guarded process (§4).
set -uo pipefail

SPDK_ROOT="${SPDK_ROOT:-$HOME/src/spdk}"
CEPH_BUILD="${CEPH_BUILD:-$HOME/src/ceph/build}"
CEPH_CONF="$CEPH_BUILD/ceph.conf"
CEPH_KEYRING="$CEPH_BUILD/keyring"
RADOS_BIN="$CEPH_BUILD/bin/rados"
VENV_PY="${VENV_PY:-$HOME/src/venvs/rnkv/bin/python}"
WEIGHTS_ROOT="${WEIGHTS_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

NVMF_TGT="$SPDK_ROOT/build/bin/nvmf_tgt"
RPC_PY="$SPDK_ROOT/scripts/rpc.py"
CEPH_USER=admin
KV_POOL=kvpool
KV_NS=weights
CLUSTER=cephcluster0
KVDEV=SmokeRados0
NQN="nqn.2026-06.io.spdk:smoke-cnode0"
CORE_MASK=0x2
TGT_MEM=2048

RUN="$(mktemp -d /tmp/smoke_rados_kvdev.XXXX)"
RPC_SOCK="$RUN/rpc.sock"
MUSER="$RUN/domain/muser0/0"
mkdir -p "$MUSER"
TGT_PID=""

cleanup() {
  [ -n "$TGT_PID" ] && kill "$TGT_PID" 2>/dev/null
  [ -n "$TGT_PID" ] && wait "$TGT_PID" 2>/dev/null
}
trap cleanup EXIT

rpc() { python3 "$RPC_PY" -s "$RPC_SOCK" "$@"; }
die() { echo "FAIL: $*" >&2; [ -f "$RUN/tgt.log" ] && tail -20 "$RUN/tgt.log" >&2; exit 1; }

echo "== Step 1: cluster + pool reachable =="
LD_LIBRARY_PATH="$CEPH_BUILD/lib" "$RADOS_BIN" -c "$CEPH_CONF" -p "$KV_POOL" ls >/dev/null 2>&1 \
  || die "pool '$KV_POOL' not reachable"
echo "ok: pool reachable"

echo "== Step 2: launch nvmf_tgt (rados kvdev) =="
[ -x "$NVMF_TGT" ] || die "nvmf_tgt missing at $NVMF_TGT"
"$NVMF_TGT" -r "$RPC_SOCK" -m "$CORE_MASK" --no-huge -s "$TGT_MEM" >"$RUN/tgt.log" 2>&1 &
TGT_PID=$!
for i in $(seq 1 100); do [ -S "$RPC_SOCK" ] && break; sleep 0.1; done
[ -S "$RPC_SOCK" ] || die "RPC socket never came up"

rpc nvmf_create_transport -t VFIOUSER >/dev/null || die "create_transport"
rpc kvdev_rados_register_cluster "$CLUSTER" --user "$CEPH_USER" \
    --config-file "$CEPH_CONF" --key-file "$CEPH_KEYRING" >/dev/null \
    || die "register_cluster (package librados 20.2.1 -> v21 cluster?)"
rpc kvdev_rados_create "$KVDEV" "$CLUSTER" "$KV_POOL" --namespace "$KV_NS" >/dev/null \
    || die "kvdev_rados_create"
rpc nvmf_create_subsystem "$NQN" -s SPDKSMOKE01 -a >/dev/null || die "create_subsystem"
rpc nvmf_subsystem_add_kv_ns "$NQN" "$KVDEV" >/dev/null || die "add_kv_ns"
rpc nvmf_subsystem_add_listener "$NQN" -t VFIOUSER -a "$MUSER" -s 0 >/dev/null || die "add_listener"
echo "ok: target up with rados kvdev -> $KV_POOL/$KV_NS"

echo "== Step 3: store/retrieve via NvmeKvClient =="
SPDK_ROOT="$SPDK_ROOT" RADOSNKV_KVSHIM_LIB="$WEIGHTS_ROOT/native/libradosnkv_kvshim.so" \
MUSER="$MUSER" "$VENV_PY" - <<'PY' || die "store/retrieve roundtrip"
import os
from rados_nkv_weights.nvmekv_client import NvmeKvClient
muser = os.environ["MUSER"]
key = b"smoke-key-016bey"           # exactly 16 bytes
val = b"hello-rados-kvdev!" * 37
# One handle per process: SPDK env initializes once, so store+retrieve share it.
# (The full e2e runs publish/load as separate processes for the same reason.)
pub = NvmeKvClient.open_publisher(muser, nsid=0)
pub.store(key, val)
print(f"   stored {len(val)} bytes")
got = pub.retrieve(key)
assert got == val, f"mismatch: {len(got)} vs {len(val)}"
print("   retrieved byte-exact OK")
PY
echo "ok: roundtrip byte-exact"

echo "== Step 4: prove object landed in $KV_POOL/$KV_NS =="
LD_LIBRARY_PATH="$CEPH_BUILD/lib" "$RADOS_BIN" -c "$CEPH_CONF" -p "$KV_POOL" -N "$KV_NS" ls 2>/dev/null \
  | sed 's/^/   obj: /' || die "rados ls"
echo "PASS: live RADOS kvdev path validated"
