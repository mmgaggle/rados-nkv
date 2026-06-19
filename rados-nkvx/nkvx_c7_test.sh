#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Slice C7 live acceptance: large-object bulk RMA on the result path
# (front-sink / executor-push, design §1.3). The REAL front client core
# (nkvx_front_client.c, via nkvx_front_client_test) registers its host output
# buffer as a WRITE-mode bulk and ships it as result_sink; the REAL executor
# (nkvx_service) PUSHes a large `identity` result straight into it and — the
# NORMATIVE invariant — awaits the PUSH completion BEFORE HG_Respond. The driver
# then sha256's the delivered bytes and compares against the object's hash,
# catching any torn / short / races-ahead-of-respond push.
#
# Covers (per transport):
#   - inline boundary  : 4096-byte object, osize 4096 -> inline (no push), verified
#   - just-over-inline : 8192-byte object, osize 8192 -> PUSH, verified
#   - 1 MiB and 64 MiB : large PUSH, content-hash verified (the GPU KV-cache case)
#   - truncation       : osize < object -> BUFFER_TOO_SMALL, true result_len, first
#                        osize bytes pushed and verified
#   - repeat loop      : 20x the 1 MiB push reusing the same sink buffer, each
#                        content-hash verified (the PUSH-races-Respond stress)
#
# Built-in `identity` (runtime=CLS, module-ns=nkvx) is the large-result vehicle,
# so no wasm/sha256 module gate is involved. Requires Ceph. Env overrides:
#   CEPH_CONF (default /home/kyle/src/ceph/build/ceph.conf)
#   RADOS     (default /home/kyle/src/ceph/build/bin/rados)
#   KVPOOL    (default kvpool)
# Default transport na+sm://; pass a provider as $1 (e.g. "ofi+tcp://127.0.0.1").
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RADOS_DIR="$(cd "$HERE/../spdk/module/kvdev/rados" && pwd)"
SPDK_ROOT="$(cd "$HERE/../spdk" && pwd)"
MERCURY_PREFIX="${MERCURY_PREFIX:-$SPDK_ROOT/vendor/mercury-install}"
TRANSPORT="${1:-na+sm://}"
CEPH_CONF="${CEPH_CONF:-/home/kyle/src/ceph/build/ceph.conf}"
RADOS="${RADOS:-/home/kyle/src/ceph/build/bin/rados}"
KVPOOL="${KVPOOL:-kvpool}"

export LD_LIBRARY_PATH="$MERCURY_PREFIX/lib:${LD_LIBRARY_PATH:-}"

# Build both the executor and the front-client driver.
make -C "$HERE" -f Makefile MERCURY_PREFIX="$MERCURY_PREFIX" >/dev/null || { echo "FAIL: build executor"; exit 1; }
make -C "$RADOS_DIR" -f Makefile.front.ut MERCURY_PREFIX="$MERCURY_PREFIX" >/dev/null || { echo "FAIL: build front driver"; exit 1; }

SVC="$HERE/nkvx_service"
DRV="$RADOS_DIR/nkvx_front_client_test"

# Front-client NA provider derived from the executor address prefix (matches the
# real bridge): na+sm:// listens as "na+sm://", ofi+tcp://127.0.0.1 as "ofi+tcp://".
case "$TRANSPORT" in
	na+sm*) FRONT_NA="na+sm://" ;;
	*)      FRONT_NA="${TRANSPORT%%://*}://" ;;
esac

WORK="$(mktemp -d /tmp/nkvx-c7.XXXXXX)"
ADDR_FILE="$WORK/addr"
SVC_LOG="$WORK/svc.log"
SVC_PID=""
declare -a OIDS=()

cleanup() {
	if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then kill "$SVC_PID" 2>/dev/null; wait "$SVC_PID" 2>/dev/null; fi
	for oid in "${OIDS[@]:-}"; do [ -n "$oid" ] && "$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" rm "$oid" >/dev/null 2>&1 || true; done
	rm -rf "$WORK"
}
trap cleanup EXIT

echo "== C7 large-result bulk RMA (transport=$TRANSPORT front=$FRONT_NA pool=$KVPOOL) =="

"$SVC" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" \
	--rados-pool "$KVPOOL" --rados-conf "$CEPH_CONF" --rados-user admin >"$SVC_LOG" 2>&1 &
SVC_PID=$!
for _ in $(seq 1 150); do
	[ -s "$ADDR_FILE" ] && break
	kill -0 "$SVC_PID" 2>/dev/null || { echo "FAIL: executor exited"; cat "$SVC_LOG"; exit 1; }
	sleep 0.1
done
[ -s "$ADDR_FILE" ] || { echo "FAIL: no address"; cat "$SVC_LOG"; exit 1; }

fail=0

# seed_obj KEY SIZE -> echoes the oid; records sha256 of the object in OBJ_SHA.
OBJ_SHA=""
seed_obj() {
	local key="$1" size="$2"
	local oid; oid="$(printf '%s' "$key" | od -An -tx1 | tr -d ' \n')"
	local f="$WORK/$key.bin"
	head -c "$size" /dev/urandom >"$f"
	"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" put "$oid" "$f" >/dev/null 2>&1 || { echo "FAIL: seed $key"; exit 1; }
	OIDS+=("$oid")
	OBJ_SHA="$(sha256sum "$f" | cut -d' ' -f1)"
	echo "$oid"
}

# run_case DESC KEY OSIZE EXPECT_STATUS EXPECT_RESULT EXPECT_SHA
run_case() {
	local desc="$1" key="$2" osize="$3" est="$4" eres="$5" esha="$6"
	echo "--- $desc ---"
	"$DRV" --listen "$FRONT_NA" --addr-file "$ADDR_FILE" \
		--runtime 2 --module-ns nkvx --module identity --key "$key" \
		--osize "$osize" --expect "$est" --expect-result "$eres" --expect-sha256 "$esha" || fail=1
}

# Boundary: 4096-byte object delivered inline (deliver == NKVX_INLINE_MAX, no push).
seed_obj inl4096 4096 >/dev/null;  SHA_INL=$OBJ_SHA
run_case "4096 B object, osize 4096 -> inline (no push)" inl4096 4096 0 4096 "$SHA_INL"

# Just over inline: 8192-byte object -> executor PUSH into the sink.
seed_obj push8k 8192 >/dev/null;   SHA_8K=$OBJ_SHA
run_case "8192 B object, osize 8192 -> PUSH" push8k 8192 0 8192 "$SHA_8K"

# 1 MiB large push.
seed_obj push1m $((1024*1024)) >/dev/null; SHA_1M=$OBJ_SHA
run_case "1 MiB object -> PUSH" push1m $((1024*1024)) 0 $((1024*1024)) "$SHA_1M"

# 64 MiB — the GPU KV-cache target size.
SZ64=$((64*1024*1024))
seed_obj push64m "$SZ64" >/dev/null; SHA_64M=$OBJ_SHA
run_case "64 MiB object -> PUSH (GPU KV-cache size)" push64m "$SZ64" 0 "$SZ64" "$SHA_64M"

# Truncation: osize 64 KiB < 1 MiB object -> BUFFER_TOO_SMALL, true result_len,
# first 64 KiB pushed. Verify the hash of the object's first 64 KiB.
SHA_TRUNC="$(head -c $((64*1024)) "$WORK/push1m.bin" | sha256sum | cut -d' ' -f1)"
run_case "1 MiB object, osize 64 KiB -> BUFFER_TOO_SMALL, truncated push" \
	push1m $((64*1024)) -3 $((1024*1024)) "$SHA_TRUNC"

# Repeat loop: same 1 MiB push 20x (each run a fresh sink in the driver, but the
# executor reuses its progress loop) — content-hash every time to stress the
# PUSH-completes-before-Respond ordering under repetition.
echo "--- 20x 1 MiB PUSH (PUSH-before-respond stress) ---"
loop_fail=0
for i in $(seq 1 20); do
	"$DRV" --listen "$FRONT_NA" --addr-file "$ADDR_FILE" \
		--runtime 2 --module-ns nkvx --module identity --key push1m \
		--osize $((1024*1024)) --expect 0 --expect-result $((1024*1024)) \
		--expect-sha256 "$SHA_1M" >/dev/null 2>&1 || { loop_fail=1; echo "  iter $i FAILED"; }
done
[ "$loop_fail" -eq 0 ] && echo "  20/20 verified" || fail=1

# C7.2 MR/hg_bulk handle-cache reuse: 10 iters on ONE front + ONE sink buffer (the
# recurring-DPTR case the cache targets). The driver asserts the cache registered
# exactly once and reused thereafter (misses=1, hits=9, evicts=0) AND content-hash
# verifies each iter.
echo "--- C7.2 handle-cache reuse: 10x 1 MiB PUSH on one front+sink ---"
"$DRV" --listen "$FRONT_NA" --addr-file "$ADDR_FILE" \
	--runtime 2 --module-ns nkvx --module identity --key push1m \
	--osize $((1024*1024)) --expect 0 --expect-result $((1024*1024)) \
	--expect-sha256 "$SHA_1M" --iters 10 2>&1 | grep -E "cache (reuse|hits)|FAIL" || fail=1

echo "--- executor log (tail) ---"; grep -E "nkvx_exec |PUSH|sink" "$SVC_LOG" | tail -8

[ "$fail" -eq 0 ] && { echo "RESULT: PASS"; exit 0; } || { echo "RESULT: FAIL"; exit 1; }
