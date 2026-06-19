#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# End-to-end exercise of the in-process NVMe KV host shim over a vfio-user
# loopback.
#
# Brings up an nvmf target with an in-memory kvdev bound to a KV namespace over
# the VFIOUSER transport, then runs kv_shim_test against the vfio-user socket
# path and greps for the PASS line.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../spdk)
source $rootdir/test/common/autotest_common.sh

nqn="nqn.2026-06.io.spdk:kv-shim-cnode0"
kvdev_name="KvShimMem0"
sock_dir=$(mktemp -d /tmp/kv_shim_test.XXXXXX)
muser_dir="$sock_dir/domain/muser0/0"
rpc_sock="$sock_dir/rpc.sock"
rpc_py="$rootdir/scripts/rpc.py -s $rpc_sock"

mkdir -p "$muser_dir"

cleanup() {
	if [[ -n ${nvmfpid:-} ]]; then
		killprocess $nvmfpid || true
	fi
	rm -rf "$sock_dir"
}
trap cleanup EXIT

# Build the shim test app if needed.
make -C "$testdir" > /dev/null

# Start the target.
$rootdir/build/bin/nvmf_tgt -r "$rpc_sock" -m 0x3 &
nvmfpid=$!
waitforlisten $nvmfpid "$rpc_sock"

# Wire up: VFIOUSER transport, in-memory kvdev, subsystem + KV namespace + listener.
$rpc_py nvmf_create_transport -t VFIOUSER
$rpc_py kvdev_mem_create "$kvdev_name"
$rpc_py nvmf_create_subsystem "$nqn" -s SPDKKV001 -a
$rpc_py nvmf_subsystem_add_kv_ns "$nqn" "$kvdev_name"
$rpc_py nvmf_subsystem_add_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0

# Run the shim test: the listener creates a socket at "$muser_dir/cntrl"; the
# host's vfio-user transport address is the directory that contains it.
host_log="$sock_dir/kv_shim_test.log"
"$testdir/kv_shim_test" "$muser_dir" 2>&1 | tee "$host_log"
rc=${PIPESTATUS[0]}

if [[ $rc -eq 0 ]] && ! grep -q "kv_shim_test: PASS" "$host_log"; then
	echo "kv_shim_test.sh: FAIL (PASS line not found)"
	rc=1
fi

# Tear down.
$rpc_py nvmf_subsystem_remove_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0
$rpc_py nvmf_delete_subsystem "$nqn"
$rpc_py kvdev_mem_delete "$kvdev_name"

if [[ $rc -eq 0 ]]; then
	echo "kv_shim_test.sh: PASS"
else
	echo "kv_shim_test.sh: FAIL (rc=$rc)"
fi
exit $rc
