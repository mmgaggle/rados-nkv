#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Slice C5a.1 live acceptance: the standalone rados-nkvx executor cold-fills an
# object from a REAL RADOS pool (its own librados, cold path only, ADR-0009) and
# runs the built-in modules over it — proving the two-tier object/cache path the
# design relocates to the executor (docs/design/slice-c-exec-rpc-mercury.md §2).
# This also completes the Slice C4 two-tier e2e with real results across the hop.
#
# Requires a running Ceph cluster with a writable pool. Override via env:
#   CEPH_CONF (default /home/kyle/src/ceph/build/ceph.conf)
#   RADOS     (default /home/kyle/src/ceph/build/bin/rados)
#   KVPOOL    (default kvpool)
# Default transport na+sm://; pass a provider as $1 (e.g. "ofi+tcp://127.0.0.1").
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPDK_ROOT="$(cd "$HERE/../spdk" && pwd)"
MERCURY_PREFIX="${MERCURY_PREFIX:-$SPDK_ROOT/vendor/mercury-install}"
TRANSPORT="${1:-na+sm://}"
CEPH_CONF="${CEPH_CONF:-/home/kyle/src/ceph/build/ceph.conf}"
RADOS="${RADOS:-/home/kyle/src/ceph/build/bin/rados}"
KVPOOL="${KVPOOL:-kvpool}"

export LD_LIBRARY_PATH="$MERCURY_PREFIX/lib:${LD_LIBRARY_PATH:-}"

make -C "$HERE" -f Makefile MERCURY_PREFIX="$MERCURY_PREFIX" >/dev/null || {
	echo "FAIL: build"; exit 1; }

# Seed a known object. Key bytes "nkvxC5a" -> oid = hex of those bytes.
KEY="nkvxC5a"
OID="$(printf '%s' "$KEY" | od -An -tx1 | tr -d ' \n')"
PAYLOAD="hello-from-rados-nkvx-c5a"   # 25 bytes
LEN=${#PAYLOAD}

"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" rm "$OID" >/dev/null 2>&1 || true
printf '%s' "$PAYLOAD" | "$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" put "$OID" /dev/stdin >/dev/null 2>&1 || {
	echo "FAIL: could not seed $KVPOOL/$OID (is Ceph up? CEPH_CONF=$CEPH_CONF)"; exit 1; }

ADDR_FILE="$(mktemp -u /tmp/nkvx-c5a-addr.XXXXXX)"
SVC_LOG="$(mktemp /tmp/nkvx-c5a-svc.XXXXXX)"
SVC_PID=""
cleanup() {
	if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then
		kill "$SVC_PID" 2>/dev/null; wait "$SVC_PID" 2>/dev/null
	fi
	"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" rm "$OID" >/dev/null 2>&1 || true
	rm -f "$ADDR_FILE" "$SVC_LOG"
}
trap cleanup EXIT

echo "== C5a.1 live executor test (transport=$TRANSPORT pool=$KVPOOL key=$KEY oid=$OID len=$LEN) =="

"$HERE/nkvx_service" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" \
	--rados-pool "$KVPOOL" --rados-conf "$CEPH_CONF" --rados-user admin >"$SVC_LOG" 2>&1 &
SVC_PID=$!
for _ in $(seq 1 100); do
	[ -s "$ADDR_FILE" ] && break
	if ! kill -0 "$SVC_PID" 2>/dev/null; then echo "FAIL: executor exited"; cat "$SVC_LOG"; exit 1; fi
	sleep 0.1
done
[ -s "$ADDR_FILE" ] || { echo "FAIL: no executor address"; cat "$SVC_LOG"; exit 1; }

fail=0
run() { # desc, expect-status, expect-result, module
	local desc="$1" est="$2" eres="$3" mod="$4" extra=()
	[ "$eres" != "-" ] && extra=(--expect-result "$eres")
	echo "--- $desc ---"
	"$HERE/nkvx_exec_client" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" \
		--key "$KEY" --runtime 2 --module-ns nkvx --module "$mod" \
		--expect "$est" "${extra[@]}" || fail=1
}

run "bytecount -> length $LEN (8B LE)" 0 8 bytecount
run "identity -> $LEN bytes" 0 "$LEN" identity

echo "--- missing key -> KEY_NOT_EXIST ---"
"$HERE/nkvx_exec_client" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" \
	--key "absent-key-xyz" --runtime 2 --module-ns nkvx --module bytecount --expect -2 || fail=1

echo "--- unknown built-in -> NOT_SUPPORTED ---"
"$HERE/nkvx_exec_client" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" \
	--key "$KEY" --runtime 2 --module-ns nkvx --module no_such_module --expect -7 || fail=1

echo "--- executor log ---"; cat "$SVC_LOG"
if [ "$fail" -ne 0 ]; then echo "RESULT: FAIL"; exit 1; fi
echo "RESULT: PASS"
exit 0
