#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 Intel Corporation. All rights reserved.
#
# Slice C2 loopback acceptance: start the standalone nkvx_service executor, wait
# for it to publish its self-address, forward one nkvx_exec from the stand-in
# client, and assert the C2 skeleton reply is NOT_SUPPORTED (design §6 C2:
# "starts, publishes addr, a tcp/sm ping RPC round-trips and returns
# NOT_SUPPORTED"). Default transport is na+sm:// (no network/hardware); pass a
# provider as $1 (e.g. "ofi+tcp://") to additionally exercise the socket path.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPDK_ROOT="$(cd "$HERE/../spdk" && pwd)"
MERCURY_PREFIX="${MERCURY_PREFIX:-$SPDK_ROOT/vendor/mercury-install}"
TRANSPORT="${1:-na+sm://}"

export LD_LIBRARY_PATH="$MERCURY_PREFIX/lib:${LD_LIBRARY_PATH:-}"

ADDR_FILE="$(mktemp -u /tmp/nkvx-addr.XXXXXX)"
SVC_LOG="$(mktemp /tmp/nkvx-svc-log.XXXXXX)"
SVC_PID=""

cleanup() {
	if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then
		kill "$SVC_PID" 2>/dev/null
		wait "$SVC_PID" 2>/dev/null
	fi
	rm -f "$ADDR_FILE" "$SVC_LOG"
}
trap cleanup EXIT

echo "== C2 loopback test (transport=$TRANSPORT) =="

"$HERE/nkvx_service" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" >"$SVC_LOG" 2>&1 &
SVC_PID=$!

# Wait (up to ~5s) for the service to publish its address.
for _ in $(seq 1 50); do
	if [ -s "$ADDR_FILE" ]; then
		break
	fi
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

# Forward one request; expect NOT_SUPPORTED (-7).
"$HERE/nkvx_exec_client" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" --expect -7
CLIENT_RC=$?

echo "--- service log ---"; cat "$SVC_LOG"

if [ "$CLIENT_RC" -ne 0 ]; then
	echo "RESULT: FAIL (client rc=$CLIENT_RC)"
	exit 1
fi

# The handler must have decoded the request and declined (no cluster configured).
if ! grep -q "NOT_SUPPORTED (no cluster)" "$SVC_LOG"; then
	echo "RESULT: FAIL (service did not log the decoded request)"
	exit 1
fi

echo "RESULT: PASS"
exit 0
