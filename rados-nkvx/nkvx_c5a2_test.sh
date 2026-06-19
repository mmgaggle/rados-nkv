#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Slice C5a.2 live acceptance: the standalone rados-nkvx executor runs a REAL
# wasm module (dlopen'd wasmtime) over an object it cold-fills from RADOS, with
# the deny-by-default sha256 gate (ADR-0010) and the TB4 content-addressed object
# cache. Proves, two-tier and remote:
#   - real wasmtime Exec returns the correct result;
#   - the hash gate rejects a wrong/absent module (INVALID), never running
#     unverified bytes;
#   - cold-fill-once: a 2nd Exec of the same key does ZERO librados object refetch
#     (cold_fills counter unchanged), i.e. the TB4 cache-acceptance holds remote.
#
# Requires Ceph + libwasmtime.so. Env overrides:
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
WASM_MODULE="$HERE/../clients/kv/wasm/bytecount.wasm"   # exports bytecount -> object length (u64 LE)

export LD_LIBRARY_PATH="$MERCURY_PREFIX/lib:${LD_LIBRARY_PATH:-}"

make -C "$HERE" -f Makefile MERCURY_PREFIX="$MERCURY_PREFIX" >/dev/null || { echo "FAIL: build"; exit 1; }
[ -r "$WASM_MODULE" ] || { echo "FAIL: missing $WASM_MODULE"; exit 1; }

SHA="$(sha256sum "$WASM_MODULE" | cut -d' ' -f1)"
WRONG="$(printf '%064d' 0)"
MODOBJ="wasm:bytecount"        # module object name in RADOS (== module_key)
KEY="nkvxC5a2"
OID="$(printf '%s' "$KEY" | od -An -tx1 | tr -d ' \n')"
PAYLOAD="twenty-nine-byte-kv-value!!!!"   # data object
LEN=${#PAYLOAD}

"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" put "$MODOBJ" "$WASM_MODULE" >/dev/null 2>&1 || { echo "FAIL: seed module (Ceph up?)"; exit 1; }
printf '%s' "$PAYLOAD" | "$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" put "$OID" /dev/stdin >/dev/null 2>&1 || { echo "FAIL: seed data"; exit 1; }

ADDR_FILE="$(mktemp -u /tmp/nkvx-c5a2-addr.XXXXXX)"
SVC_LOG="$(mktemp /tmp/nkvx-c5a2-svc.XXXXXX)"
SVC_PID=""
cleanup() {
	if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then kill "$SVC_PID" 2>/dev/null; wait "$SVC_PID" 2>/dev/null; fi
	"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" rm "$MODOBJ" >/dev/null 2>&1 || true
	"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" rm "$OID" >/dev/null 2>&1 || true
	rm -f "$ADDR_FILE" "$SVC_LOG"
}
trap cleanup EXIT

echo "== C5a.2 live wasm test (transport=$TRANSPORT pool=$KVPOOL key=$KEY len=$LEN sha=${SHA:0:12}..) =="

# Fresh executor (empty caches): run the NEGATIVE gate cases first so they cannot
# be masked by a warm/compiled-module cache from a prior positive Exec.
"$HERE/nkvx_service" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" \
	--rados-pool "$KVPOOL" --rados-conf "$CEPH_CONF" --rados-user admin >"$SVC_LOG" 2>&1 &
SVC_PID=$!
for _ in $(seq 1 150); do
	[ -s "$ADDR_FILE" ] && break
	kill -0 "$SVC_PID" 2>/dev/null || { echo "FAIL: executor exited"; cat "$SVC_LOG"; exit 1; }
	sleep 0.1
done
[ -s "$ADDR_FILE" ] || { echo "FAIL: no address"; cat "$SVC_LOG"; exit 1; }

fail=0
cl() { # desc expect [expect-result] -- args...
	local desc="$1" est="$2"; shift 2
	local eres=""; if [ "${1:-}" != "--" ]; then eres="$1"; shift; fi; shift  # drop "--"
	local extra=(); [ -n "$eres" ] && extra=(--expect-result "$eres")
	echo "--- $desc ---"
	"$HERE/nkvx_exec_client" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" \
		--key "$KEY" --runtime 1 --module-ns "$KVPOOL" "$@" --expect "$est" "${extra[@]}" || fail=1
}

cl "wrong sha256 (cold) -> deny-by-default INVALID" -4 -- --module "$MODOBJ" --sha256 "$WRONG"
cl "absent module object -> INVALID"                -4 -- --module "wasm:absent" --sha256 "$SHA"
cl "correct wasm bytecount -> SUCCESS, len $LEN"     0 8 -- --module "$MODOBJ" --sha256 "$SHA"
cl "2nd Exec same key -> SUCCESS (cache hit)"        0 8 -- --module "$MODOBJ" --sha256 "$SHA"

echo "--- executor log ---"; grep -E "wasm '" "$SVC_LOG"

# Cache-acceptance: the data object must be cold-filled exactly once across the
# two successful Execs (the 2nd serves from the TB4 object cache).
if grep -q "cold_fills=2" "$SVC_LOG"; then
	echo "RESULT: FAIL (object cold-filled more than once — TB4 cache not effective remote)"; fail=1
elif ! grep -q "content_hits=1" "$SVC_LOG"; then
	echo "RESULT: FAIL (no object-cache hit on the 2nd Exec)"; fail=1
fi

# Assert the correct Exec returned the true length (bytecount -> object length).
GOT="$("$HERE/nkvx_exec_client" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" \
	--key "$KEY" --runtime 1 --module-ns "$KVPOOL" --module "$MODOBJ" --sha256 "$SHA" --expect 0 2>/dev/null \
	| grep -oE 'u64=[0-9]+' | cut -d= -f2)"
if [ "$GOT" != "$LEN" ]; then echo "RESULT: FAIL (bytecount u64=$GOT, expected $LEN)"; fail=1; fi

[ "$fail" -eq 0 ] && { echo "RESULT: PASS"; exit 0; } || { echo "RESULT: FAIL"; exit 1; }
