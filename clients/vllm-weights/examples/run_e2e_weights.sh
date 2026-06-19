#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
#
# End-to-end demo: distribute REAL model weights through a live NVMe-KV
# namespace backed by RADOS (vstart Ceph), using the two CLIs.
#
#   1. Ensure a vstart Ceph cluster is up and the KV pool is reachable.
#   2. Ensure nvmf_tgt is built and the native KV host shim (.so) is built.
#   3. HF-download a model's safetensors (default ibm-granite/granite-4.0-h-micro).
#   4. Bring up nvmf_tgt with a librados-backed kvdev bound to a KV namespace
#      over VFIOUSER (subsystem -> pool, KV namespace -> rados namespace).
#   5. PUBLISH the model into the namespace via the admin NvmeKvClient
#      (`rados-nkv-publish --vfu-addr ...`): chunks land as rados objects.
#   6. Show the chunks really landed (`rados ls` on the pool/namespace).
#   7. LOAD it back via the read-only NvmeKvClient (`rados-nkv-load --vfu-addr
#      ... --verify`): every tensor is checked byte-for-byte against the source.
#
# The publisher and loader run as SEPARATE processes (one SPDK env per process)
# against the SAME running target, exercising the ADR-0008 read/write split.
#
# Everything is env-parameterized (defaults match the dev box) and the whole
# target lifecycle lives in this one invocation (the target is killed on exit).
#
# Usage:   bash examples/run_e2e_weights.sh
# Key env: SPDK_ROOT, CEPH_BUILD, KV_POOL, KV_NS, HF_MODEL, PRECISION, HF_HOME,
#          MODEL_REV, CORE_MASK, TGT_MEM, REBUILD_SHIM.

set -uo pipefail

# ---- Configuration --------------------------------------------------------
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WEIGHTS_ROOT="${WEIGHTS_ROOT:-$(cd "$HERE/.." && pwd)}"
VENV="${VENV:-$WEIGHTS_ROOT/.venv}"
SPDK_ROOT="${SPDK_ROOT:-/mnt/spdk}"

CEPH_BUILD="${CEPH_BUILD:-/mnt/ceph/build}"
CEPH_CONF="${CEPH_CONF:-$CEPH_BUILD/ceph.conf}"
CEPH_KEYRING="${CEPH_KEYRING:-$CEPH_BUILD/keyring}"
RADOS_BIN="${RADOS_BIN:-$CEPH_BUILD/bin/rados}"
CEPH_USER="${CEPH_USER:-admin}"
KV_POOL="${KV_POOL:-kvpool}"
KV_NS="${KV_NS:-weights}"

HF_MODEL="${HF_MODEL:-ibm-granite/granite-4.0-h-micro}"
PRECISION="${PRECISION:-bf16}"
HF_HOME="${HF_HOME:-/mnt/hf-cache}"; export HF_HOME
MODEL_REV="${MODEL_REV:-${HF_MODEL}@main}"

NQN="${NQN:-nqn.2026-06.io.spdk:weights-cnode0}"
CLUSTER="${CLUSTER:-cephcluster0}"
KVDEV="${KVDEV:-WeightsRados0}"
CORE_MASK="${CORE_MASK:-0x2}"
TGT_MEM="${TGT_MEM:-4096}"
SHIM_SO="${SHIM_SO:-$WEIGHTS_ROOT/native/libradosnkv_kvshim.so}"
REBUILD_SHIM="${REBUILD_SHIM:-0}"

NVMF_TGT="$SPDK_ROOT/build/bin/nvmf_tgt"
RPC_PY="$SPDK_ROOT/scripts/rpc.py"

RUN="$(mktemp -d /tmp/run_e2e_weights.XXXXXX)"
MUSER="$RUN/muser/0"; mkdir -p "$MUSER"
RPC_SOCK="$RUN/rpc.sock"
TGT=""

cleanup() {
    [ -n "$TGT" ] && kill -9 "$TGT" 2>/dev/null
    rm -rf "$RUN"
}
trap cleanup EXIT

log()  { printf '\n\033[1;34m== %s ==\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m   %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31mFAIL: %s\033[0m\n' "$*" >&2; exit 1; }

rpc() { python3 "$RPC_PY" -s "$RPC_SOCK" "$@"; }

# Wait for the target's RPC socket without a shell sleep-loop.
wait_for_sock() {
    python3 - "$RPC_SOCK" <<'PY'
import socket, sys, time
path = sys.argv[1]
deadline = time.time() + 30
while time.time() < deadline:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(path); s.close(); sys.exit(0)
    except OSError:
        time.sleep(0.2)
    finally:
        try: s.close()
        except OSError: pass
sys.exit(1)
PY
}

log "Config"
printf '   model=%s @ %s\n   pool/ns=%s/%s  user=%s\n   SPDK_ROOT=%s  HF_HOME=%s\n' \
    "$HF_MODEL" "$PRECISION" "$KV_POOL" "$KV_NS" "$CEPH_USER" "$SPDK_ROOT" "$HF_HOME"

# ---- venv -----------------------------------------------------------------
[ -f "$VENV/bin/activate" ] || die "venv not found at $VENV (create it and pip install -e .[dev,publish])"
# shellcheck disable=SC1091
source "$VENV/bin/activate"

# ---- Step 1: Ceph ---------------------------------------------------------
log "Step 1/7: Ceph (vstart) + pool reachable"
[ -x "$RADOS_BIN" ] || die "rados CLI not found at $RADOS_BIN (set CEPH_BUILD)"
timeout 20 "$RADOS_BIN" -c "$CEPH_CONF" -p "$KV_POOL" ls >/dev/null 2>&1 \
    || die "pool '$KV_POOL' not reachable; bring vstart up (see run_e2e_rados_kv.sh Step 1)"
ok "Ceph up; pool '$KV_POOL' reachable."

# ---- Step 2: target + shim ------------------------------------------------
log "Step 2/7: nvmf_tgt + native KV host shim"
[ -x "$NVMF_TGT" ] || die "nvmf_tgt not built at $NVMF_TGT (cd $SPDK_ROOT && ./configure --with-rbd --with-vfio-user --without-nvme-cuse && make -j)"
if [ "$REBUILD_SHIM" = "1" ] || [ ! -f "$SHIM_SO" ]; then
    SPDK_ROOT="$SPDK_ROOT" bash "$WEIGHTS_ROOT/native/build.sh" || die "shim build failed"
fi
[ -f "$SHIM_SO" ] || die "shim .so missing at $SHIM_SO"
ok "nvmf_tgt present; shim at $SHIM_SO"

# ---- Step 3: HF download --------------------------------------------------
log "Step 3/7: HuggingFace download ($HF_MODEL)"
command -v hf >/dev/null 2>&1 || die "the 'hf' CLI is missing (pip install huggingface_hub)"
# `hf download` prints the resolved snapshot dir on stdout (some versions prefix
# it with "path="). Take the last line and strip any prefix.
SNAP="$(hf download "$HF_MODEL" 2>/dev/null | tail -1)" || die "hf download failed"
SNAP="${SNAP#path=}"
[ -d "$SNAP" ] || die "could not resolve a snapshot dir from hf download (got: $SNAP)"
shards=$(find "$SNAP" -maxdepth 1 -name '*.safetensors' | wc -l)
[ "$shards" -ge 1 ] || die "no .safetensors under $SNAP"
ok "model at $SNAP ($shards shard(s))"

# ---- Step 4: bring up the rados-kvdev KV namespace ------------------------
log "Step 4/7: nvmf_tgt + librados kvdev + KV namespace (VFIOUSER)"
"$NVMF_TGT" -r "$RPC_SOCK" -m "$CORE_MASK" --no-huge -s "$TGT_MEM" >"$RUN/tgt.log" 2>&1 &
TGT=$!
wait_for_sock || { tail -20 "$RUN/tgt.log"; die "nvmf_tgt RPC socket did not come up"; }
kill -0 "$TGT" 2>/dev/null || { tail -20 "$RUN/tgt.log"; die "nvmf_tgt exited at startup"; }

rpc nvmf_create_transport -t VFIOUSER >/dev/null || die "create_transport failed"
rpc kvdev_rados_register_cluster "$CLUSTER" --user "$CEPH_USER" \
    --config-file "$CEPH_CONF" --key-file "$CEPH_KEYRING" >/dev/null \
    || die "kvdev_rados_register_cluster failed (ceph.conf/keyring/user?)"
rpc kvdev_rados_create "$KVDEV" "$CLUSTER" "$KV_POOL" --namespace "$KV_NS" >/dev/null \
    || die "kvdev_rados_create failed (pool must exist)"
rpc nvmf_create_subsystem "$NQN" -s SPDKWEIGHTS01 -a >/dev/null || die "create_subsystem failed"
rpc nvmf_subsystem_add_kv_ns "$NQN" "$KVDEV" >/dev/null || die "add_kv_ns failed"
rpc nvmf_subsystem_add_listener "$NQN" -t VFIOUSER -a "$MUSER" -s 0 >/dev/null || die "add_listener failed"
ok "KV namespace live: $NQN -> kvdev $KVDEV -> rados $KV_POOL/$KV_NS (VFIOUSER @ $MUSER)"

# objects already in the namespace before we publish (for a clean delta)
before=$(timeout 20 "$RADOS_BIN" -c "$CEPH_CONF" -p "$KV_POOL" -N "$KV_NS" ls 2>/dev/null | wc -l)

# ---- Step 5: PUBLISH via the admin client ---------------------------------
log "Step 5/7: rados-nkv-publish (admin NvmeKvClient -> RADOS)"
rados-nkv-publish --safetensors "$SNAP" --model-revision "$MODEL_REV" \
    --precision "$PRECISION" --vfu-addr "$MUSER" --nsid 0 \
    || die "publish failed"
ok "published $MODEL_REV @ $PRECISION"

# ---- Step 6: prove the chunks are in RADOS --------------------------------
log "Step 6/7: verify chunks landed in RADOS ($KV_POOL/$KV_NS)"
after=$(timeout 20 "$RADOS_BIN" -c "$CEPH_CONF" -p "$KV_POOL" -N "$KV_NS" ls 2>/dev/null | wc -l)
echo "   objects in $KV_POOL/$KV_NS: before=$before after=$after (delta=$((after - before)))"
[ "$after" -gt "$before" ] || die "no new objects appeared in $KV_POOL/$KV_NS"
ok "chunks present in RADOS"

# ---- Step 7: LOAD + byte-verify via the read-only client ------------------
log "Step 7/7: rados-nkv-load (read-only NvmeKvClient) + byte verify"
rados-nkv-load "$MODEL_REV" "$PRECISION" --vfu-addr "$MUSER" --nsid 0 --verify "$SNAP" \
    || die "load/verify failed"

log "ALL STEPS PASSED — $HF_MODEL published to and loaded from NVMe-KV-on-RADOS, byte-exact"
