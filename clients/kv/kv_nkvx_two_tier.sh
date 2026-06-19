#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# rados-nkvx TWO-TIER end-to-end (Slice C2/C3/C4/C5a, ADR-0015): a CPU-issued
# NVMe-KV Exec terminates on the FRONT (nvmf_tgt + kvdev_rados), which forwards it
# over Mercury to a SEPARATE standalone rados-nkvx executor process; the executor
# cold-fills the object from RADOS via its OWN librados and runs the module
# (built-in or real wasm), returning the result back across the hop to the tenant
# CQE. This is the relocation ADR-0009/0012 deferred and ADR-0015 bound to Mercury.
#
# Contrast with kv_nkvx_exec.sh (single-tier, in-process executor): there the
# module runs OFF the reactor in the SAME process; here it runs in a DIFFERENT
# process reached over the inter-tier RPC. So the proof is the executor's OWN log
# (it ran the Execs), not the front's off-reactor-worker proof.
#
# No coherence dependency: this is a non-mutating Store->Exec flow (a cached key
# is never overwritten), so the cross-hop cache invalidation (Slice C5b) is NOT
# needed here.
#
# Requires: Ceph UP with pool 'kvpool'; SPDK built --with-rbd --with-vfio-user
# --with-wasm --with-mercury; libwasmtime.so dlopen'able; Mercury vendored.
# Runs an ISOLATED nvmf_tgt (--no-huge, non-overlapping core mask, private rpc
# sock + muser dir) so it never disturbs another nvmf_tgt on the box.
set +e

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../spdk")
source "$rootdir/test/common/autotest_common.sh"

: "${CEPH_CONF:=/home/kyle/src/ceph/build/ceph.conf}"; export CEPH_CONF
pool_name="${KV_RADOS_POOL:-kvpool}"
cluster_name="nkvx_tt_cluster"
kvdev_name="KvNkvxTT0"
nqn="nqn.2026-06.io.spdk:kv-nkvx-tt-cnode0"
nsid=1
core_mask="${KV_TT_CORE_MASK:-0x30}"          # cores 4,5 — avoid a co-resident target on 0x6
svc_transport="${KV_TT_TRANSPORT:-na+sm://}"  # inter-tier Mercury provider (host-local)

svc_bin="$rootdir/module/kvdev/rados/nkvx_service/nkvx_service"
mercury_prefix="$rootdir/vendor/mercury-install"
export LD_LIBRARY_PATH="$mercury_prefix/lib:${LD_LIBRARY_PATH:-}"

sock_dir=$(mktemp -d /tmp/kv_nkvx_tt.XXXXXX)
muser_dir="$sock_dir/domain/muser0/0"
rpc_sock="$sock_dir/rpc.sock"
rpc_py="$rootdir/scripts/rpc.py -s $rpc_sock"
addr_file="$sock_dir/executor.addr"
svc_log="$sock_dir/nkvx_service.log"
tgt_log="$sock_dir/nvmf_tgt.log"
mkdir -p "$muser_dir"

rados_bin="$(dirname "$CEPH_CONF")/bin/rados"; [[ -x "$rados_bin" ]] || rados_bin="rados"
if ! timeout 15 "$rados_bin" -c "$CEPH_CONF" -p "$pool_name" ls >/dev/null 2>&1; then
	echo "kv_nkvx_two_tier: FAIL (pool '$pool_name' not reachable; is Ceph up?)"; exit 1
fi
echo "OSD precheck: pool '$pool_name' reachable"

svc_pid=""
cleanup() {
	[[ -n ${nvmfpid:-} ]] && killprocess "$nvmfpid" || true
	if [[ -n "$svc_pid" ]] && kill -0 "$svc_pid" 2>/dev/null; then
		kill "$svc_pid" 2>/dev/null; wait "$svc_pid" 2>/dev/null
	fi
	cp "$tgt_log" /tmp/nvmf_tgt_tt.log 2>/dev/null
	cp "$svc_log" /tmp/nkvx_service_tt.log 2>/dev/null
	rm -rf "$sock_dir"
}
trap cleanup EXIT

[[ -x "$svc_bin" ]] || make -C "$rootdir/module/kvdev/rados/nkvx_service" -f Makefile >/dev/null 2>&1
make -C "$testdir" >/dev/null 2>&1

# 1) Upload the wasm module objects the executor will fetch + verify: bytecount
#    (op 12) plus the caps-runaway trio (op 13-15: fuel/walltime/memory) that
#    kv_host issues and asserts are CONTAINED (ABORTED) by the executor's caps.
for m in bytecount fuel_runaway walltime_runaway overalloc; do
	"$rados_bin" -c "$CEPH_CONF" -p "$pool_name" put "wasm:${m}" "$testdir/wasm/${m}.wasm" >/dev/null 2>&1
done
SHA_BC=$(sha256sum "$testdir/wasm/bytecount.wasm" | cut -d' ' -f1)
SHA_FR=$(sha256sum "$testdir/wasm/fuel_runaway.wasm" | cut -d' ' -f1)
SHA_WR=$(sha256sum "$testdir/wasm/walltime_runaway.wasm" | cut -d' ' -f1)
SHA_OA=$(sha256sum "$testdir/wasm/overalloc.wasm" | cut -d' ' -f1)

# 2) Start the standalone EXECUTOR (its own librados, cold path only) and wait for
#    it to publish its self-address.
"$svc_bin" --listen "$svc_transport" --addr-file "$addr_file" \
	--rados-pool "$pool_name" --rados-conf "$CEPH_CONF" --rados-user admin >"$svc_log" 2>&1 &
svc_pid=$!
for _ in $(seq 1 100); do
	[[ -s "$addr_file" ]] && break
	kill -0 "$svc_pid" 2>/dev/null || { echo "kv_nkvx_two_tier: FAIL (executor exited)"; cat "$svc_log"; exit 1; }
	sleep 0.1
done
[[ -s "$addr_file" ]] || { echo "kv_nkvx_two_tier: FAIL (executor published no address)"; cat "$svc_log"; exit 1; }
exec_addr="$(cat "$addr_file")"
echo "executor up at: $exec_addr"

# 3) Start an ISOLATED front nvmf_tgt and wire the two-tier kvdev to the executor.
#    --no-huge + a non-overlapping core mask + a distinct DPDK shm-id (-i) keep it
#    fully isolated from any other nvmf_tgt on the box.
"$rootdir/build/bin/nvmf_tgt" -r "$rpc_sock" -m "$core_mask" --no-huge -s 512 -i 7 >"$tgt_log" 2>&1 &
nvmfpid=$!
waitforlisten "$nvmfpid" "$rpc_sock"

$rpc_py nvmf_create_transport -t VFIOUSER
$rpc_py kvdev_rados_register_cluster "$cluster_name" --user admin --config-file "$CEPH_CONF"
$rpc_py kvdev_rados_create "$kvdev_name" "$cluster_name" "$pool_name" --remote-executor "$exec_addr"
$rpc_py nvmf_create_subsystem "$nqn" -s SPDKNKVXTT -a
$rpc_py nvmf_subsystem_add_kv_ns "$nqn" "$kvdev_name"
$rpc_py nvmf_subsystem_add_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0

# Allowlist: built-ins (10/11) + the real-wasm structured binding (12).
$rpc_py nvmf_ns_set_kv_exec_allowlist "$nqn" "$nsid" "10:nkvx:bytecount 11:nkvx:identity" >/dev/null
PYTHONPATH="$rootdir/python" python3 - "$rpc_sock" "$nqn" "$nsid" "$pool_name" \
	"$SHA_BC" "$SHA_FR" "$SHA_WR" "$SHA_OA" <<'PY'
import sys
from spdk.rpc.client import JSONRPCClient
sock, nqn, nsid, pool, sbc, sfr, swr, soa = sys.argv[1:9]
allow = [
    {"op_id": 10, "binding": "nkvx:bytecount"},
    {"op_id": 11, "binding": "nkvx:identity"},
    {"op_id": 12, "runtime": "wasm", "module_namespace": pool, "module_key": "wasm:bytecount",       "sha256": sbc, "caps": 0},
    {"op_id": 13, "runtime": "wasm", "module_namespace": pool, "module_key": "wasm:fuel_runaway",     "sha256": sfr, "caps": 0},
    {"op_id": 14, "runtime": "wasm", "module_namespace": pool, "module_key": "wasm:walltime_runaway", "sha256": swr, "caps": 0},
    {"op_id": 15, "runtime": "wasm", "module_namespace": pool, "module_key": "wasm:overalloc",        "sha256": soa, "caps": 0},
]
JSONRPCClient(sock).call("nvmf_ns_set_kv_exec_allowlist", {"nqn": nqn, "nsid": int(nsid), "allowlist": allow})
PY
echo "allowlist => $($rpc_py nvmf_ns_get_kv_exec_allowlist "$nqn" "$nsid")"

# 4) Run the tenant: Store + KV Exec(s). The front forwards each Exec over Mercury
#    to the executor; results return to the tenant CQE.
host_log="$sock_dir/kv_host.log"
# --no-huge tenant (the box may carry no hugepages; a co-resident target runs
# --no-huge too). kv_host gets its own pid-based DPDK prefix, so no collision.
KV_HOST_NO_HUGE=1 KV_HOST_MEM_MB="${KV_TT_HOST_MEM_MB:-1024}" \
	"$testdir/kv_host" "$muser_dir" nkvx-exec 2>&1 | tee "$host_log"
rc=${PIPESTATUS[0]}

# 5) Assertions.
#    (a) the tenant saw correct results.
if [[ $rc -ne 0 ]] || ! grep -q "PASS: KV Exec nkvx" "$host_log"; then
	echo "kv_nkvx_two_tier: FAIL (kv_host rc=$rc / no nkvx PASS)"; rc=1
fi
#    (b) two-tier proof: the EXECUTOR process ran the Execs (the front forwarded).
exec_runs=$(grep -c "nkvx_service: nkvx_exec .* -> status=0" "$svc_log")
wasm_runs=$(grep -c "nkvx_executor: wasm 'bytecount' done status=0" "$svc_log")
echo "two-tier proof: executor status=0 Execs=$exec_runs (wasm=$wasm_runs)"
if [[ ${exec_runs:-0} -lt 1 ]]; then
	echo "kv_nkvx_two_tier: FAIL (executor ran no Execs — the front did not forward)"; rc=1
fi
#    (c) the front did NOT run the in-process off-reactor worker (it relocated).
if grep -q "nkvx: off-reactor proof" "$tgt_log"; then
	echo "kv_nkvx_two_tier: NOTE (front emitted in-process off-reactor proof — single-tier path was taken)"
	rc=1
fi

if [[ $rc -eq 0 ]]; then echo "kv_nkvx_two_tier: PASS"; else echo "kv_nkvx_two_tier: FAIL"; fi
exit $rc
