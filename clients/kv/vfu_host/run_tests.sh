#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Regression for the QEMU-less GPU-initiated NVMe-KV client over the two-tier
# rados-nkvx target. Brings the target up, runs the full GPU op matrix
# (store/retrieve/exec, wavefront batch, two-tier exec, size ladder) with RADOS
# verification, tears down, and reports PASS/FAIL.
#
# Prereqs (per boot): rdma-loopback-validate.sh setup + Ceph vstart (reuse).
# Usage: run_tests.sh        (builds the GPU client if needed, then runs)
set +e
HERE=$(readlink -f "$(dirname "$0")")
WT=$(readlink -f "$HERE/../../../spdk")
CEPH=${CEPH_ROOT:-/home/kyle/src/ceph}
GPU="$HERE/nkv_vfu_gpu"
RADOS() { LD_LIBRARY_PATH="$CEPH/build/lib" "$CEPH/build/bin/rados" -c "$CEPH/build/ceph.conf" -p kvpool "$@"; }
pass=0; fail=0
# check: <desc> <expected-substr> <actual-output>. Runs in the current shell (no
# pipe/subshell) so pass/fail counters accumulate.
check() {
	local d="$1" e="$2" out="$3"
	if echo "$out" | grep -q "$e"; then
		echo "  PASS: $d"; pass=$((pass+1))
	else
		echo "  FAIL: $d"; fail=$((fail+1)); echo "$out" | sed 's/^/      /'
	fi
}

[ -x "$GPU" ] || bash "$HERE/build_gpu.sh" || { echo "build failed"; exit 1; }

echo "== bring up two-tier rados-nkvx target =="
bash "$HERE/nkvx-up.sh" >/tmp/run_tests_up.log 2>&1 || { echo "bringup FAILED:"; tail -15 /tmp/run_tests_up.log; exit 1; }
TR=/tmp/nkvx/muser/0
cleanup() { bash "$HERE/nkvx-down.sh" >/dev/null 2>&1; }
trap cleanup EXIT
g() { "$GPU" "$TR" "$@" 2>&1 | grep -vE '^EAL:'; }

echo "== 1. single store / retrieve / exec (two-tier) =="
check "GPU store"                "STORE ok"         "$(g store rk hello-rados-nkvx)"
check "RADOS object content"     "hello-rados-nkvx" "$(RADOS get "$(printf rk | xxd -p)" - 2>/dev/null)"
check "GPU retrieve"             "hello-rados-nkvx" "$(g retrieve rk)"
check "GPU exec op10 bytecount"  "count=16"         "$(g exec rk 10)"
check "GPU exec op11 identity"   "hello-rados-nkvx" "$(g exec rk 11)"

echo "== 2. wavefront store-batch (32 stores, one doorbell) =="
check "wavefront store-batch x32" "STORE-BATCH ok: 32/32" "$(g store-batch wfb 32)"
n=$(RADOS ls 2>/dev/null | grep -c "^$(printf wfb | xxd -p)")
check "32 batch objects in RADOS" "^32$" "$n"

echo "== 3. wavefront exec-batch (32 two-tier Execs, one doorbell) =="
check "wavefront exec-batch op10 x32" "EXEC-BATCH ok: 32/32" "$(g exec-batch wfb 10 32)"
check "wavefront exec-batch op11 x32" "EXEC-BATCH ok: 32/32" "$(g exec-batch wfb 11 32)"

echo "== 4. GPU-produced size ladder (byte-exact, spdk-5co coherence) =="
# Spans single-region (<=2 MiB) and multi-region segment-list transfers, up to
# the controller's 64 MiB max_io_size (spdk-rcz DMA-map fix + spdk-cvi SGL list).
for sz in 4K 64K 256K 1M 2M 8M 32M 64M; do
  check "store-big $sz byte-exact" "BYTE-EXACT" "$(g store-big "big_$sz" "$sz")"
done

echo "== 5. Large-result Exec scatter (op11 identity, byte-exact) =="
# Verifies the two-tier executor's large result scatters back bit-faithfully
# through the region-bounded SGL DPTR (spdk-w2y).
for sz in 4K 1M 2M 8M 64M; do
  check "exec-big $sz identity byte-exact" "BYTE-EXACT" "$(g exec-big "eb_$sz" 11 "$sz")"
done

echo
echo "===================================================="
echo "RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ] && echo "ALL PASS" || echo "FAILURES PRESENT"
exit $((fail > 0))
