#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Per-namespace read-only KV enforcement over a vfio-user loopback (ADR-0008).
#
# Brings up an nvmf target with an in-memory kvdev bound as a *read-only* KV
# namespace (nvmf_subsystem_add_kv_ns --read-only) exposed over VFIOUSER, then
# runs the kv_ro_host app which asserts the read-only trust split:
#   - KV Store  is REJECTED with sct=0x01 sc=0x82 (Attempted Write to RO Range)
#   - KV Exist  is ALLOWED (returns the normal absent-key status 0x87)
#   - KV Delete is REJECTED with sct=0x01 sc=0x82
#   - KV Exec   is REJECTED with sct=0x01 sc=0x82
#
# It then verifies there is NO regression for a writable KV namespace: a second
# kvdev bound *without* --read-only still accepts a KV Store via the kv_host app.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../spdk)
source $rootdir/test/common/autotest_common.sh

nqn_ro="nqn.2026-06.io.spdk:kv-ro-cnode0"
nqn_rw="nqn.2026-06.io.spdk:kv-rw-cnode0"
kvdev_ro="KvMemRO0"
kvdev_rw="KvMemRW0"
sock_dir=$(mktemp -d /tmp/kv_ro_vfio_user.XXXXXX)
muser_ro="$sock_dir/domain/muser_ro/0"
muser_rw="$sock_dir/domain/muser_rw/0"
rpc_sock="$sock_dir/rpc.sock"
rpc_py="$rootdir/scripts/rpc.py -s $rpc_sock"

mkdir -p "$muser_ro" "$muser_rw"

cleanup() {
	if [[ -n ${nvmfpid:-} ]]; then
		killprocess $nvmfpid || true
	fi
	rm -rf "$sock_dir"
}
trap cleanup EXIT

# Build the host apps if needed.
make -C "$testdir" > /dev/null

# Start the target.
$rootdir/build/bin/nvmf_tgt -r "$rpc_sock" -m 0x3 &
nvmfpid=$!
waitforlisten $nvmfpid "$rpc_sock"

$rpc_py nvmf_create_transport -t VFIOUSER

# Read-only KV namespace.
$rpc_py kvdev_mem_create "$kvdev_ro"
$rpc_py nvmf_create_subsystem "$nqn_ro" -s SPDKKVRO01 -a
$rpc_py nvmf_subsystem_add_kv_ns "$nqn_ro" "$kvdev_ro" --read-only
$rpc_py nvmf_subsystem_add_listener "$nqn_ro" -t VFIOUSER -a "$muser_ro" -s 0

# Writable KV namespace (no --read-only) to prove no regression.
$rpc_py kvdev_mem_create "$kvdev_rw"
$rpc_py nvmf_create_subsystem "$nqn_rw" -s SPDKKVRW01 -a
$rpc_py nvmf_subsystem_add_kv_ns "$nqn_rw" "$kvdev_rw"
$rpc_py nvmf_subsystem_add_listener "$nqn_rw" -t VFIOUSER -a "$muser_rw" -s 0

rc=0

# ---------------------------------------------------------------------------
# Read-only enforcement: Store/Delete/KV-Exec rejected, Exist allowed.
# ---------------------------------------------------------------------------
ro_log="$sock_dir/kv_ro_host.log"
"$testdir/kv_ro_host" "$muser_ro" 2>&1 | tee "$ro_log"
rc=${PIPESTATUS[0]}

if [[ $rc -eq 0 ]]; then
	for needle in \
		"PASS: KV Store rejected on read-only ns (sct=0x01 sc=0x82)" \
		"PASS: KV Exist allowed on read-only ns (absent key) (sct=0x00 sc=0x87)" \
		"PASS: KV Delete rejected on read-only ns (sct=0x01 sc=0x82)" \
		"PASS: KV Exec rejected on read-only ns (sct=0x01 sc=0x82)"; do
		if ! grep -qF "$needle" "$ro_log"; then
			echo "kv_ro_vfio_user: FAIL (missing assertion: $needle)"
			rc=1
		fi
	done
fi

# ---------------------------------------------------------------------------
# No regression: a writable KV namespace still accepts a Store. The kv_host app
# does a full Store+Retrieve round-trip; "allow" mode skips the KV Exec phase
# allowlist dance but still requires the op-IDs to be allowlisted, so allowlist
# them first, matching kv_vfio_user.sh.
# ---------------------------------------------------------------------------
if [[ $rc -eq 0 ]]; then
	$rpc_py nvmf_ns_set_kv_exec_allowlist "$nqn_rw" 1 "1 2"
	rw_log="$sock_dir/kv_host_rw.log"
	"$testdir/kv_host" "$muser_rw" allow 2>&1 | tee "$rw_log"
	rc=${PIPESTATUS[0]}
	if [[ $rc -eq 0 ]] && ! grep -q "KV Store OK" "$rw_log"; then
		echo "kv_ro_vfio_user: FAIL (writable ns did not accept Store)"
		rc=1
	fi
fi

# Tear down.
$rpc_py nvmf_subsystem_remove_listener "$nqn_ro" -t VFIOUSER -a "$muser_ro" -s 0
$rpc_py nvmf_subsystem_remove_listener "$nqn_rw" -t VFIOUSER -a "$muser_rw" -s 0
$rpc_py nvmf_delete_subsystem "$nqn_ro"
$rpc_py nvmf_delete_subsystem "$nqn_rw"
$rpc_py kvdev_mem_delete "$kvdev_ro"
$rpc_py kvdev_mem_delete "$kvdev_rw"

if [[ $rc -eq 0 ]]; then
	echo "kv_ro_vfio_user: PASS"
else
	echo "kv_ro_vfio_user: FAIL (rc=$rc)"
fi
exit $rc
