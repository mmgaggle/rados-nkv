#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Slice spdk-7sr.17.18 ("B") loopback acceptance: start the standalone nkvx_service
# with the in-memory backend, then STORE + RETRIEVE values across the inline/large
# boundary and assert byte-exactness. A value over NKVX_INLINE_MAX (4096) drives the
# new zero-copy value_bulk RDMA-PULL on store (front registers the value READ-mode;
# the executor PULLs it before the mem-backend write) and the result_sink PUSH on
# retrieve. Default transport is na+sm://; pass a provider as $1 (e.g. "ofi+tcp://")
# to additionally exercise the socket path.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPDK_ROOT="$(cd "$HERE/../spdk" && pwd)"
MERCURY_PREFIX="${MERCURY_PREFIX:-$SPDK_ROOT/vendor/mercury-install}"
TRANSPORT="${1:-na+sm://}"

export LD_LIBRARY_PATH="$MERCURY_PREFIX/lib:${LD_LIBRARY_PATH:-}"

ADDR_FILE="$(mktemp -u /tmp/nkvx-store-addr.XXXXXX)"
SVC_LOG="$(mktemp /tmp/nkvx-store-svc-log.XXXXXX)"
SVC_PID=""

cleanup() {
	if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then
		kill "$SVC_PID" 2>/dev/null
		wait "$SVC_PID" 2>/dev/null
	fi
	rm -f "$ADDR_FILE" "$SVC_LOG"
}
trap cleanup EXIT

echo "== B store round-trip test (transport=$TRANSPORT) =="

# --mem-object opens the in-memory backend (seed key is unused; the test stores its
# own keys). No Ceph cluster on the data path.
"$HERE/nkvx_service" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" \
	--mem-object seed=x >"$SVC_LOG" 2>&1 &
SVC_PID=$!

for _ in $(seq 1 50); do
	[ -s "$ADDR_FILE" ] && break
	if ! kill -0 "$SVC_PID" 2>/dev/null; then
		echo "FAIL: nkvx_service exited before publishing an address"
		echo "--- service log ---"; cat "$SVC_LOG"
		exit 1
	fi
	sleep 0.1
done
if [ ! -s "$ADDR_FILE" ]; then
	echo "FAIL: timed out waiting for the service address file"
	echo "--- service log ---"; cat "$SVC_LOG"
	exit 1
fi
echo "service addr: $(cat "$ADDR_FILE")"

# Sizes straddle the inline boundary (NKVX_INLINE_MAX = 4096):
#   0       empty value
#   4096    inline boundary (no PULL/PUSH)
#   4097    just over -> value_bulk PULL on store, result_sink PUSH on retrieve
#   65536   64 KiB large
#   1048576 1 MiB (the handoff acceptance size)
SIZES="0 4096 4097 65536 1048576"
fail=0
i=0
for sz in $SIZES; do
	i=$((i + 1))
	echo "--- store+retrieve value_len=$sz ---"
	if ! "$HERE/nkvx_kv_store_test" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" \
		--key "b$i" --value-len "$sz"; then
		echo "RESULT: FAIL at value_len=$sz"
		fail=1
	fi
done

echo "--- service log (tail) ---"; tail -20 "$SVC_LOG"

if [ "$fail" -ne 0 ]; then
	echo "RESULT: FAIL"
	exit 1
fi
echo "RESULT: PASS"
exit 0
