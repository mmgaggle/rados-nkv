#!/usr/bin/env bash
# rados-nkvx Exec over NVMe/RDMA (RoCEv2) on the E810 — confirms the bidirectional
# Exec fabrics limitation is NOT TCP-specific. Target (root ns) RDMA listener on
# f0 = 10.110.0.1:4420; initiator (root ns) RDMA-connects via irdma0. Run under sudo -E.
set +e
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../spdk")
source "$rootdir/test/common/autotest_common.sh"
: "${CEPH_CONF:=/home/kyle/src/ceph/build/ceph.conf}"; export CEPH_CONF
pool_name="${KV_RADOS_POOL:-kvpool}"
cluster_name="nkvx_cluster"; kvdev_name="KvNkvxRdma0"
nqn="nqn.2026-06.io.spdk:kv-nkvx-rdma-cnode0"; nsid=1
TGT_IP=10.110.0.1; TGT_PORT=4420
WASM_DIR="${SPDK_NKVX_WASM_DIR:-$testdir/wasm}"
sock_dir=$(mktemp -d /tmp/kv_nkvx_rdma.XXXXXX)
rpc_sock="$sock_dir/rpc.sock"; rpc_py="$rootdir/scripts/rpc.py -s $rpc_sock"

rados_bin="$(dirname "$CEPH_CONF")/bin/rados"; [[ -x "$rados_bin" ]] || rados_bin="rados"
if ! timeout 15 "$rados_bin" -c "$CEPH_CONF" -p "$pool_name" ls >/dev/null 2>&1; then
	echo "kv_nkvx_exec_rdma: FAIL (pool '$pool_name' not reachable)"; exit 1
fi
echo "OSD precheck: pool '$pool_name' reachable"

tgt_log="$sock_dir/nvmf_tgt.log"
cleanup() { cp "$tgt_log" /tmp/nvmf_tgt_rdma.log 2>/dev/null; [[ -n ${nvmfpid:-} ]] && killprocess "$nvmfpid"; rm -rf "$sock_dir"; }
trap cleanup EXIT

make -C "$testdir" >/dev/null 2>&1
"$rootdir/build/bin/nvmf_tgt" -r "$rpc_sock" -m 0x3 > "$tgt_log" 2>&1 &
nvmfpid=$!
waitforlisten "$nvmfpid" "$rpc_sock"

$rpc_py nvmf_create_transport -t RDMA
$rpc_py kvdev_rados_register_cluster "$cluster_name" --user admin --config-file "$CEPH_CONF"
$rpc_py kvdev_rados_create "$kvdev_name" "$cluster_name" "$pool_name"
$rpc_py nvmf_create_subsystem "$nqn" -s SPDKNKVXRDMA -a
$rpc_py nvmf_subsystem_add_kv_ns "$nqn" "$kvdev_name"
$rpc_py nvmf_subsystem_add_listener "$nqn" -t RDMA -a "$TGT_IP" -s "$TGT_PORT" -f IPv4
$rpc_py nvmf_ns_set_kv_exec_allowlist "$nqn" "$nsid" "10:nkvx:bytecount 11:nkvx:identity 12:nkvx:wasm:bytecount"
echo "allowlist => $($rpc_py nvmf_ns_get_kv_exec_allowlist "$nqn" "$nsid")"

host_log="$sock_dir/kv_host.log"
env KV_HOST_TRTYPE=rdma KV_HOST_ADDR="$TGT_IP" KV_HOST_PORT="$TGT_PORT" KV_HOST_NQN="$nqn" \
	SPDK_NKVX_WASM_DIR="$WASM_DIR" "$testdir/kv_host" x nkvx-exec 2>&1 | tee "$host_log"
rc=${PIPESTATUS[0]}

echo "=== RESULT: Store vs Exec over NVMe/RDMA ==="
grep -aE 'NVMe/|Found KV|Store OK|Exec|sc=0x|sct=|PASS|FAIL' "$host_log" | head
proof=$(grep -c "nkvx: off-reactor proof" "$tgt_log")
echo "off-reactor proof lines in target log = $proof  (0 => Exec never reached the executor)"
if grep -q "PASS: KV Exec nkvx" "$host_log"; then
	echo "kv_nkvx_exec_rdma: Exec PASSED over RDMA (surprise!)"
else
	echo "kv_nkvx_exec_rdma: Exec did NOT pass over RDMA (expected: bidirectional fabrics limit)"
fi
exit $rc
