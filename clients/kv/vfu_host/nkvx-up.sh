#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Bring up the full two-tier rados-nkvx target and LEAVE IT RUNNING.
# Prints the <traddr> to use with nkv_vfu_gpu. Tear down with nkvx-down.sh.
#
# Prereqs (one-time per boot):
#   sudo /home/kyle/nkvx-repro/rdma-loopback-validate.sh setup     # 10.110.0.1/.2 + netns
#   cd /home/kyle/src/ceph/build && MON=1 OSD=3 ../src/vstart.sh --without-dashboard   # reuse
set -euo pipefail
HERE=$(readlink -f "$(dirname "$0")")
WT=$(readlink -f "$HERE/../../../spdk")   # spdk submodule of the umbrella repo
CEPH=${CEPH_ROOT:-/home/kyle/src/ceph}
CEPH_LIB=$CEPH/build/lib; CEPH_CONF=$CEPH/build/ceph.conf
MERCURY=$WT/vendor/mercury-install
NKVX_SVC=$WT/module/kvdev/rados/nkvx_service/nkvx_service
U=$(id -un); G=$(id -gn)
RUN=/tmp/nkvx; rm -rf "$RUN"; mkdir -p "$RUN/muser/0"
TRADDR="$RUN/muser/0"; RPC_SOCK="$RUN/rpc.sock"; EXEC_ADDR="$RUN/exec.addr"
NQN="nqn.2026-06.io.spdk:nkvx"
rpc() { "$WT"/scripts/rpc.py -s "$RPC_SOCK" "$@"; }

# sanity: network + ceph must be up
ip addr show enp101s0f0np0 2>/dev/null | grep -q 10.110.0.1 \
  || { echo "ERROR: 10.110.0.1 missing -- run: sudo /home/kyle/nkvx-repro/rdma-loopback-validate.sh setup"; exit 1; }
LD_LIBRARY_PATH="$CEPH_LIB" timeout 15 "$CEPH/build/bin/rados" -c "$CEPH_CONF" -p kvpool ls >/dev/null 2>&1 \
  || { echo "ERROR: Ceph/kvpool unreachable -- start vstart (reuse) first"; exit 1; }

sudo -n pkill -x nkvx_service 2>/dev/null || true
sudo -n pkill -f 'nvmf_tgt -r /tmp/nkvx/rpc.sock' 2>/dev/null || true
sleep 0.5; rm -f "$EXEC_ADDR"

echo "[1/3] rados-nkvx executor (netns nvmf_e810, verbs)"
sudo -n bash -c "ulimit -l unlimited; exec ip netns exec nvmf_e810 \
  env LD_LIBRARY_PATH='$MERCURY/lib:$CEPH_LIB' '$NKVX_SVC' \
  --listen 'ofi+verbs;ofi_rxm://10.110.0.2' --addr-file '$EXEC_ADDR' \
  --rados-pool kvpool --rados-conf '$CEPH_CONF' --rados-user admin" >"$RUN/exec.log" 2>&1 &
disown
for i in $(seq 1 100); do [ -s "$EXEC_ADDR" ] && break; sleep 0.1; done
[ -s "$EXEC_ADDR" ] || { echo "ERROR: executor published no addr"; tail -10 "$RUN/exec.log"; exit 1; }
EXEC_SELF=$(cat "$EXEC_ADDR"); echo "      exec = $EXEC_SELF"

echo "[2/3] rados-nkv front (nvmf_tgt, unlimited memlock, --remote-executor)"
sudo -n bash -c "ulimit -l unlimited; exec setpriv --reuid $U --regid $G --init-groups \
  env LD_LIBRARY_PATH='$CEPH_LIB:$MERCURY/lib:/usr/local/lib' \
  '$WT/build/bin/nvmf_tgt' -r '$RPC_SOCK' -m 0x3" >"$RUN/front.log" 2>&1 &
disown
for i in $(seq 1 150); do [ -S "$RPC_SOCK" ] && rpc spdk_get_version >/dev/null 2>&1 && break; sleep 0.1; done
[ -S "$RPC_SOCK" ] || { echo "ERROR: front rpc sock never appeared"; tail -15 "$RUN/front.log"; exit 1; }

echo "[3/3] wire VFIOUSER + rados kvdev + KV ns + exec allowlist"
rpc nvmf_create_transport -t VFIOUSER >/dev/null
rpc kvdev_rados_register_cluster ceph0 --user admin --config-file "$CEPH_CONF" >/dev/null
rpc kvdev_rados_create KvRados0 ceph0 kvpool --remote-executor "$EXEC_SELF" >/dev/null
rpc nvmf_create_subsystem "$NQN" -s SPDKNKVX01 -a >/dev/null
rpc nvmf_subsystem_add_kv_ns "$NQN" KvRados0 >/dev/null
rpc nvmf_subsystem_add_listener "$NQN" -t VFIOUSER -a "$TRADDR" -s 0 >/dev/null
rpc nvmf_ns_set_kv_exec_allowlist "$NQN" 1 "10:nkvx:bytecount 11:nkvx:identity" >/dev/null
[ -S "$TRADDR/cntrl" ] || { echo "ERROR: cntrl socket missing"; tail -15 "$RUN/front.log"; exit 1; }

echo
echo "two-tier rados-nkvx UP.  TRADDR = $TRADDR"
echo "  GPU client:  $HERE/nkv_vfu_gpu $TRADDR <store|retrieve|exec> ..."
echo "  tear down:   $HERE/nkvx-down.sh"
