#!/usr/bin/env bash
# rados-nkvx Exec over NVMe/TCP on the E810 (port-to-port loopback via netns).
# Target (root ns) listens on f0 = 10.110.0.1:4420; initiator (netns nvmf_e810,
# f1 = 10.110.0.2) connects over the physical 100GbE cable. Run under sudo -E.
set +e
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../spdk")
source "$rootdir/test/common/autotest_common.sh"

: "${CEPH_CONF:=/home/kyle/src/ceph/build/ceph.conf}"; export CEPH_CONF
pool_name="${KV_RADOS_POOL:-kvpool}"
cluster_name="nkvx_cluster"; kvdev_name="KvNkvxTcp0"
nqn="nqn.2026-06.io.spdk:kv-nkvx-tcp-cnode0"; nsid=1
NS=nvmf_e810; TGT_IP=10.110.0.1; TGT_PORT=4420
WASM_DIR="${SPDK_NKVX_WASM_DIR:-$testdir/wasm}"

sock_dir=$(mktemp -d /tmp/kv_nkvx_tcp.XXXXXX)
rpc_sock="$sock_dir/rpc.sock"; rpc_py="$rootdir/scripts/rpc.py -s $rpc_sock"

rados_bin="$(dirname "$CEPH_CONF")/bin/rados"; [[ -x "$rados_bin" ]] || rados_bin="rados"
if ! timeout 15 "$rados_bin" -c "$CEPH_CONF" -p "$pool_name" ls >/dev/null 2>&1; then
	echo "kv_nkvx_exec_tcp: FAIL (pool '$pool_name' not reachable)"; exit 1
fi
echo "OSD precheck: pool '$pool_name' reachable"

cleanup() { cp "$tgt_log" /tmp/nvmf_tgt_tcp.log 2>/dev/null; [[ -n ${nvmfpid:-} ]] && killprocess "$nvmfpid"; rm -rf "$sock_dir"; }
trap cleanup EXIT

make -C "$testdir" >/dev/null 2>&1

tgt_log="$sock_dir/nvmf_tgt.log"
"$rootdir/build/bin/nvmf_tgt" -r "$rpc_sock" -m 0x3 > "$tgt_log" 2>&1 &
nvmfpid=$!
waitforlisten "$nvmfpid" "$rpc_sock"

$rpc_py nvmf_create_transport -t TCP
$rpc_py kvdev_rados_register_cluster "$cluster_name" --user admin --config-file "$CEPH_CONF"
$rpc_py kvdev_rados_create "$kvdev_name" "$cluster_name" "$pool_name"
$rpc_py nvmf_create_subsystem "$nqn" -s SPDKNKVXTCP -a
$rpc_py nvmf_subsystem_add_kv_ns "$nqn" "$kvdev_name"
$rpc_py nvmf_subsystem_add_listener "$nqn" -t TCP -a "$TGT_IP" -s "$TGT_PORT" -f IPv4
$rpc_py nvmf_ns_set_kv_exec_allowlist "$nqn" "$nsid" "10:nkvx:bytecount 11:nkvx:identity 12:nkvx:wasm:bytecount"
echo "allowlist => $($rpc_py nvmf_ns_get_kv_exec_allowlist "$nqn" "$nsid")"

# initiator in the netns, NVMe/TCP -> traffic crosses the E810 port-to-port cable
host_log="$sock_dir/kv_host.log"
ip netns exec "$NS" env \
	KV_HOST_TRTYPE=tcp KV_HOST_ADDR="$TGT_IP" KV_HOST_PORT="$TGT_PORT" KV_HOST_NQN="$nqn" \
	SPDK_NKVX_WASM_DIR="$WASM_DIR" \
	"$testdir/kv_host" x nkvx-exec 2>&1 | tee "$host_log"
rc=${PIPESTATUS[0]}

if [[ $rc -ne 0 ]] || ! grep -q "PASS: KV Exec nkvx" "$host_log"; then
	echo "kv_nkvx_exec_tcp: FAIL (kv_host rc=$rc / no nkvx PASS)"; rc=1
fi
proof=$(grep -c "nkvx: off-reactor proof" "$tgt_log")
bad=$(grep "nkvx: off-reactor proof" "$tgt_log" | grep -c "off_reactor=NO")
echo "off-reactor proof: lines=$proof bad=$bad"
grep "nkvx: off-reactor proof" "$tgt_log" | head -3
[[ ${proof:-0} -lt 1 || ${bad:-0} -ne 0 ]] && rc=1

if [[ $rc -eq 0 ]]; then echo "kv_nkvx_exec_tcp: PASS"; else echo "kv_nkvx_exec_tcp: FAIL (rc=$rc)"; fi
exit $rc
