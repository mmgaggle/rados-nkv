#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# rados-nkvx TB1 end-to-end (ADR-0009): a CPU-issued NVMe-KV Exec runs a built-in
# WASM-seam module in the NEW in-process sandboxed executor, OFF the SPDK reactor,
# against an object cold-filled from RADOS via librados. This is NOT the legacy
# cls/rados_read_op_exec path (that one is kv_rados_exec.sh).
#
# Wires the librados-backed kvdev to a KV namespace, allowlists:
#   op_id 10 -> "nkvx:bytecount"       (result = object length as LE u64; built-in)
#   op_id 11 -> "nkvx:identity"        (result = object bytes copied back; built-in)
#   op_id 12 -> "nkvx:wasm:bytecount"  (result = object length as LE u64; REAL .wasm)
# then runs kv_host nkvx-exec, which Stores a value and issues those Execs and a
# concurrent-Retrieve latency probe proving the reactor is not blocked.
#
# The "nkvx:" binding prefix routes KV Exec to the executor; the text after it
# names the module. A "wasm:<name>" module runs a REAL precompiled <name>.wasm in
# the dlopen-backed wasmtime runtime (ADR-0013) instead of a C built-in; it
# requires an SPDK built --with-wasm, libwasmtime.so installed (see below), and
# SPDK_NKVX_WASM_DIR pointing at the .wasm directory.
#
# Off-reactor proof is DETERMINISTIC (not timing-based): the target logs the
# reactor OS thread id and the worker OS thread id the module actually ran on for
# every Exec ("nkvx: off-reactor proof ... off_reactor=YES"). This script greps
# the target log and asserts those ids differ. No artificial delay is used.
#
# Requires: a vstart Ceph cluster UP with pool 'kvpool', and an SPDK build
# configured --with-rbd --with-vfio-user (and --with-wasm for the op_id 12 real-
# wasm Exec). No OSD-side object class is needed (that's the whole point of
# ADR-0009). libwasmtime.so must be dlopen'able by the target: install it to
# /usr/local/lib (the executor probes that path) or put it on LD_LIBRARY_PATH.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../spdk)
source $rootdir/test/common/autotest_common.sh

: "${CEPH_CONF:=/mnt/ceph/build/ceph.conf}"
export CEPH_CONF
pool_name="${KV_RADOS_POOL:-kvpool}"
cluster_name="nkvx_cluster"
kvdev_name="KvNkvx0"
nqn="nqn.2026-06.io.spdk:kv-nkvx-cnode0"
nsid=1

sock_dir=$(mktemp -d /tmp/kv_nkvx_exec.XXXXXX)
muser_dir="$sock_dir/domain/muser0/0"
rpc_sock="$sock_dir/rpc.sock"
rpc_py="$rootdir/scripts/rpc.py -s $rpc_sock"

mkdir -p "$muser_dir"

# Fail-fast OSD/pool liveness precheck: the executor cold-fills via librados, so
# a dead/wedged OSD would otherwise hang. Bounded probe BEFORE bringing up target.
OSD_PRECHECK_TIMEOUT_S="${OSD_PRECHECK_TIMEOUT_S:-15}"
rados_bin="$(dirname "$CEPH_CONF")/bin/rados"
[[ -x "$rados_bin" ]] || rados_bin="rados"
if ! timeout "$OSD_PRECHECK_TIMEOUT_S" "$rados_bin" -c "$CEPH_CONF" -p "$pool_name" ls > /dev/null 2>&1; then
	echo "kv_nkvx_exec: FAIL (OSD/pool '$pool_name' not reachable within ${OSD_PRECHECK_TIMEOUT_S}s; is the vstart OSD up?)"
	exit 1
fi
echo "OSD liveness precheck: pool '$pool_name' reachable"

cleanup() {
	if [[ -n ${nvmfpid:-} ]]; then
		killprocess $nvmfpid || true
	fi
	rm -rf "$sock_dir"
}
trap cleanup EXIT

# Build the host app if needed.
make -C "$testdir" > /dev/null

# Directory holding precompiled <name>.wasm modules for the real-wasm Exec path
# (op_id 12). The executor reads SPDK_NKVX_WASM_DIR to locate "bytecount.wasm".
export SPDK_NKVX_WASM_DIR="$testdir/wasm"

# Start the target. Capture its log so we can grep the deterministic off-reactor
# proof (reactor_tid vs worker run_tid) the executor emits for each Exec. The
# target inherits SPDK_NKVX_WASM_DIR so the dlopen-backed wasmtime runtime can
# find bytecount.wasm.
tgt_log="$sock_dir/nvmf_tgt.log"
$rootdir/build/bin/nvmf_tgt -r "$rpc_sock" -m 0x3 > "$tgt_log" 2>&1 &
nvmfpid=$!
waitforlisten $nvmfpid "$rpc_sock"

# Wire up: VFIOUSER transport + rados cluster/kvdev + subsystem + KV ns + listener.
$rpc_py nvmf_create_transport -t VFIOUSER
$rpc_py kvdev_rados_register_cluster "$cluster_name" --user admin --config-file "$CEPH_CONF"
$rpc_py kvdev_rados_create "$kvdev_name" "$cluster_name" "$pool_name"
$rpc_py nvmf_create_subsystem "$nqn" -s SPDKNKVX01 -a
$rpc_py nvmf_subsystem_add_kv_ns "$nqn" "$kvdev_name"
$rpc_py nvmf_subsystem_add_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0

# Allowlist the nkvx op-IDs.
#
# Two kinds of binding (ADR-0014 structured model):
#   - BUILT-INS (op 10/11): the legacy "nkvx:<module>" shorthand. The CLI splits
#     each token on the FIRST ':' into op_id and binding, so "10:nkvx:bytecount"
#     yields op_id=10 binding="nkvx:bytecount". This decodes to a cls binding with
#     module_namespace="nkvx"; kvdev_rados_exec routes that namespace to the
#     in-process executor's compiled-in C built-ins (no untrusted bytes -> no
#     sha256 anchor required).
#   - REAL WASM (op 12-15): a STRUCTURED wasm binding (runtime=wasm) carrying the
#     module locator (module_namespace=pool, module_key="wasm:<name>" == the RADOS
#     OBJECT NAME of the .wasm) AND the content sha256 that authorizes the fetched
#     bytes (deny-by-default, ADR-0010). The legacy "nkvx:wasm:<name>" string
#     CANNOT carry a sha256/locator, so the wasm ops use the structured form. The
#     module objects are uploaded to RADOS below so the executor can fetch them.
# op_ids 13/14/15 are the TB2 runaway/over-alloc modules: a compute-runaway
# (fuel cap), a wall-clock-runaway (epoch cap), and an over-allocator (memory
# cap). The host issues each and asserts the command is CONTAINED (aborted),
# never crashing/hanging the target.

# Upload the wasm module objects to RADOS (object name == module_key "wasm:<name>"
# so the executor routes it to the real wasmtime runtime, not a built-in).
for m in bytecount fuel_runaway walltime_runaway overalloc; do
	"$rados_bin" -c "$CEPH_CONF" -p "$pool_name" put "wasm:${m}" "$testdir/wasm/${m}.wasm"
done

# Built-ins via the legacy shorthand; their decode is asserted via get-allowlist.
$rpc_py nvmf_ns_set_kv_exec_allowlist "$nqn" "$nsid" "10:nkvx:bytecount 11:nkvx:identity"

# Real-wasm structured bindings (runtime=wasm + sha256 + locator). The CLI only
# emits the legacy {op_id,binding} form, so the structured entries are merged in
# via a direct RPC call that re-sets the full allowlist (built-ins + wasm).
SHA_BC=$(sha256sum "$testdir/wasm/bytecount.wasm" | cut -d' ' -f1)
SHA_FR=$(sha256sum "$testdir/wasm/fuel_runaway.wasm" | cut -d' ' -f1)
SHA_WR=$(sha256sum "$testdir/wasm/walltime_runaway.wasm" | cut -d' ' -f1)
SHA_OA=$(sha256sum "$testdir/wasm/overalloc.wasm" | cut -d' ' -f1)
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
JSONRPCClient(sock).call("nvmf_ns_set_kv_exec_allowlist",
                         {"nqn": nqn, "nsid": int(nsid), "allowlist": allow})
PY
get_json=$($rpc_py nvmf_ns_get_kv_exec_allowlist "$nqn" "$nsid")
echo "nvmf_ns_get_kv_exec_allowlist => $get_json"

# Run the host: Store + KV Exec(bytecount) + KV Exec(identity) + off-reactor
# latency probe. The module executes in-process OFF the reactor; the host asserts
# the returned bytes and the concurrent-Retrieve latency margin.
host_log="$sock_dir/kv_host.log"
"$testdir/kv_host" "$muser_dir" nkvx-exec 2>&1 | tee "$host_log"
rc=${PIPESTATUS[0]}

if [[ $rc -eq 0 ]] && ! grep -q "PASS: KV Exec nkvx e2e (off-reactor built-in module ran)" "$host_log"; then
	echo "kv_nkvx_exec: FAIL (nkvx KV Exec e2e did not report PASS)"
	rc=1
fi

# Criterion 2: DETERMINISTIC off-reactor proof. The target logs, per Exec, the
# reactor OS thread id and the worker OS thread id the module body ran on. Assert
# at least one such line exists AND that every proof line reports off_reactor=YES
# (worker tid != reactor tid). Any off_reactor=NO would mean the compute ran on
# the polled reactor -> hard fail.
if [[ $rc -eq 0 ]]; then
	proof_lines=$(grep -c "nkvx: off-reactor proof" "$tgt_log" || true)
	bad_lines=$(grep "nkvx: off-reactor proof" "$tgt_log" | grep -c "off_reactor=NO" || true)
	if [[ "${proof_lines:-0}" -lt 1 ]]; then
		echo "kv_nkvx_exec: FAIL (no off-reactor proof lines in target log)"
		rc=1
	elif [[ "${bad_lines:-0}" -ne 0 ]]; then
		echo "kv_nkvx_exec: FAIL ($bad_lines Exec(s) ran ON the reactor — off_reactor=NO)"
		grep "nkvx: off-reactor proof" "$tgt_log" | grep "off_reactor=NO" || true
		rc=1
	else
		echo "Off-reactor proof: $proof_lines Exec(s), all off_reactor=YES (worker tid != reactor tid)"
		grep "nkvx: off-reactor proof" "$tgt_log" | head -2 || true
	fi
fi

# Criterion 3 (TB2): per-invocation caps contained the runaway/over-alloc modules.
# The target logs a "RESOURCE CAP — aborted" line per cap kill; assert we saw the
# fuel and epoch kills, and that the target process is STILL ALIVE (never crashed).
if [[ $rc -eq 0 ]]; then
	cap_lines=$(grep -c "RESOURCE CAP — aborted\|trapped during execution" "$tgt_log" || true)
	if ! kill -0 "$nvmfpid" 2>/dev/null; then
		echo "kv_nkvx_exec: FAIL (target process died — a cap did NOT contain a runaway)"
		rc=1
	elif [[ "${cap_lines:-0}" -lt 1 ]]; then
		echo "kv_nkvx_exec: FAIL (no cap-containment lines in target log)"
		rc=1
	else
		echo "Cap containment: target survived $cap_lines contained runaway/over-alloc Exec(s)"
		grep "RESOURCE CAP — aborted\|trapped during execution" "$tgt_log" | head -3 || true
	fi
fi

# Criterion 4 (B2, spdk-ii0): once an object is in the executor cache, a repeat
# Exec of that object does NO librados read. All Execs here hit the SAME object
# (key kvkey01 -> one oid); the C built-ins (op 10/11) bypass the executor cache
# and each cold-read, and the first wasm Exec (op 12) cold-fills the cache, so the
# later wasm Execs (op 13/14/15) on that object MUST be served from cache with NO
# librados read ("served from executor cache (pinned, no librados read)"). Assert
# at least one such cache-hit Exec occurred (the B2 fix path fired and skipped the
# read). The "pinned" wording is the spdk-ii0 D2 race-safe probe-and-pin path.
if [[ $rc -eq 0 ]]; then
	cold_reads=$(grep -c "cache miss -> librados cold-fill read" "$tgt_log" || true)
	cache_hits=$(grep -c "served from executor cache (.*no librados read)" "$tgt_log" || true)
	if [[ "${cache_hits:-0}" -ge 1 ]]; then
		echo "B2 no-refetch: $cache_hits cache-hit Exec(s) skipped librados ($cold_reads cold-fill read(s))"
		grep -E "cold-fill read|no librados read" "$tgt_log" | head -8 || true
	else
		echo "kv_nkvx_exec: FAIL (B2: no cache-hit Exec skipped librados; cold_reads=$cold_reads cache_hits=$cache_hits)"
		grep -E "cold-fill read|no librados read" "$tgt_log" | head -8 || true
		rc=1
	fi
fi

# Tear down.
$rpc_py nvmf_subsystem_remove_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0
$rpc_py nvmf_delete_subsystem "$nqn"
$rpc_py kvdev_rados_delete "$kvdev_name"
$rpc_py kvdev_rados_unregister_cluster "$cluster_name"

if [[ $rc -eq 0 ]]; then
	echo "kv_nkvx_exec: PASS"
else
	echo "kv_nkvx_exec: FAIL (rc=$rc)"
fi
exit $rc
