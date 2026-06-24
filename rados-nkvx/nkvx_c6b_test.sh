#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Slice C6b acceptance (bead spdk-5ia, design §C6b): the executor-side do-not-PUSH
# cross-process cancellation protocol. Where C6a's cancel was ORIGIN-only and could
# NOT prove "no PUSH lands after teardown" (the cross-process UAF), C6b adds the
# nkvx_cancel RPC + ack handshake so the front releases the result_sink DPTR only
# AFTER the executor confirms (via the ack) it has set do-not-PUSH / HG_Bulk_cancel'd
# any in-flight PUSH and dropped its remote bulk view. This test ASSERTS that
# invariant directly (the assertion C6a disclaimed):
#
#   (cancel-mid-PUSH) submit a large-result (64 MiB) Exec with a registered
#       result_sink, deterministically land the cancel mid-flight via the executor
#       TICK-DEFERRAL stall hook (NKVX_TEST_PUSH_STALL_TICKS), then assert:
#         - the done-cb fired EXACTLY once with ABORTED, outstanding==0;
#         - NO PUSH lands after the ack: the driver poisons the sink post-ack,
#           progresses extra ticks, and asserts the poison is intact
#           (--poison-after-cancel) — the load-bearing no-late-PUSH check;
#         - the executor logged which cancel case (a/b/c) + a "no PUSH after ack"
#           / "PUSH skipped" line;
#         - the DPTR is reusable: a following NON-cancel Exec on the SAME sink
#           delivers byte-correct content (sha256);
#         - the executor stays up.
#   (valgrind) the cancel+ack path under valgrind --leak-check=full: 0 errors,
#       0 definite/indirect lost.
#
# Runs on BOTH na+sm:// and ofi+tcp://127.0.0.1 (pass the provider as $1). The
# (cancel)/(valgrind) cases need a rados-backed executor (the CLS "identity" module
# copies the object bytes through, so the 64 MiB result == the object, sha-checkable).
# Requires Ceph. Env overrides: CEPH_CONF, RADOS, KVPOOL (see nkvx_c6_test.sh).
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

make -C "$HERE" -f Makefile MERCURY_PREFIX="$MERCURY_PREFIX" >/dev/null || { echo "FAIL: build executor"; exit 1; }
make -C "$RADOS_DIR" -f Makefile.front.ut MERCURY_PREFIX="$MERCURY_PREFIX" >/dev/null || { echo "FAIL: build front driver"; exit 1; }

SVC="$HERE/nkvx_service"
DRV="$RADOS_DIR/nkvx_front_client_test"

case "$TRANSPORT" in
	na+sm*) FRONT_NA="na+sm://" ;;
	*)      FRONT_NA="${TRANSPORT%%://*}://" ;;
esac

# A 64 MiB data object: the UAF-critical state is a wide in-flight 64 MiB PUSH into
# the registered result_sink when the cancel lands.
BIGKEY="nkvxC6b"
BIGOID="$(printf '%s' "$BIGKEY" | od -An -tx1 | tr -d ' \n')"
BIGSZ=$((64*1024*1024))

WORK="$(mktemp -d /tmp/nkvx-c6b.XXXXXX)"
ADDR_FILE="$WORK/addr"
SVC_LOG="$WORK/svc.log"
SVC_PID=""

cleanup() {
	if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then kill "$SVC_PID" 2>/dev/null; wait "$SVC_PID" 2>/dev/null; fi
	"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" rm "$BIGOID" >/dev/null 2>&1 || true
	rm -rf "$WORK"
}
trap cleanup EXIT

# seed the 64 MiB object + remember its sha for the DPTR-reuse content check.
head -c "$BIGSZ" /dev/urandom > "$WORK/big.bin"
"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" put "$BIGOID" "$WORK/big.bin" >/dev/null 2>&1 || { echo "FAIL: seed big data (Ceph up?)"; exit 1; }
BIGSHA="$(sha256sum "$WORK/big.bin" | cut -d' ' -f1)"

# Start the executor with the TICK-DEFERRAL stall hook so the cancel deterministically
# lands while the request is registered but the PUSH is not yet on the wire (Case b on
# every transport; Case c is reached on verbs / with a sufficiently large object). The
# stall is a per-req tick countdown decremented in the single-threaded progress loop —
# NOT usleep (which would freeze the loop and block cancel processing).
start_svc() {
	NKVX_TEST_PUSH_STALL_TICKS="${1:-8}" \
	"$SVC" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" \
		--rados-pool "$KVPOOL" --rados-conf "$CEPH_CONF" --rados-user admin >"$SVC_LOG" 2>&1 &
	SVC_PID=$!
	local i
	for i in $(seq 1 150); do
		[ -s "$ADDR_FILE" ] && return 0
		kill -0 "$SVC_PID" 2>/dev/null || { echo "FAIL: executor exited at boot"; cat "$SVC_LOG"; return 1; }
		sleep 0.1
	done
	echo "FAIL: executor published no address"; cat "$SVC_LOG"; return 1
}

echo "== C6b do-not-PUSH cancellation protocol (transport=$TRANSPORT front=$FRONT_NA) =="
fail=0

# ---------------------------------------------------------------------------
# cancel-mid-PUSH: stall the PUSH, cancel immediately, assert no-late-PUSH +
# exactly-once ABORTED + drained + DPTR reusable (byte-correct sha) + executor up.
# --cancel-after 0 fires the cancel before the stalled PUSH could ever submit, so it
# deterministically WINS -> ABORTED on both transports.
# ---------------------------------------------------------------------------
echo "--- cancel mid-PUSH (stall hook) -> ABORTED, NO late PUSH, DPTR reusable ---"
start_svc 8 || exit 1
"$DRV" --listen "$FRONT_NA" --addr-file "$ADDR_FILE" \
	--key "$BIGKEY" --runtime 1 --module-ns nkvx --module identity \
	--osize "$BIGSZ" --cancel-after 0 --poison-after-cancel 200 --iters 3 \
	--expect 0 --expect-sha256 "$BIGSHA" 2>&1 | sed 's/^/  /'
drc=${PIPESTATUS[0]}
[ "$drc" -eq 0 ] || { echo "  FAIL: driver exit $drc"; fail=1; }
if ! kill -0 "$SVC_PID" 2>/dev/null; then echo "  FAIL: executor died during cancel test"; fail=1; fi

# The executor must log which cancel case it took AND a no-late-PUSH style line.
if grep -Eq "cancel ack op_id=.*case=[abc]|cancel op_id=.*case=[abc]" "$SVC_LOG"; then
	echo "  PASS: executor logged the cancel case"
	grep -Eo "case=[abc]" "$SVC_LOG" | sort | uniq -c | sed 's/^/    /'
else
	echo "  FAIL: no cancel-case (a/b/c) line in executor log"; fail=1
fi
if grep -Eq "no PUSH after ack|PUSH skipped|no late PUSH" "$SVC_LOG"; then
	echo "  PASS: executor logged the do-not-PUSH / no-late-PUSH invariant"
else
	echo "  FAIL: no 'no PUSH after ack' / 'PUSH skipped' line in executor log"; fail=1
fi
if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then kill "$SVC_PID" 2>/dev/null; wait "$SVC_PID" 2>/dev/null; SVC_PID=""; fi

# ---------------------------------------------------------------------------
# valgrind: the cancel+ack path must be UAF/leak clean (the load-bearing check).
# A 1 MiB object keeps valgrind tractable but still > NKVX_INLINE_MAX (PUSH path).
# ---------------------------------------------------------------------------
if command -v valgrind >/dev/null 2>&1; then
	echo "--- valgrind --leak-check=full over the cancel+ack path ---"
	rm -f "$ADDR_FILE"
	start_svc 8 || exit 1
	VG_LOG="$WORK/vg.log"
	VGKEY="nkvxC6bvg"; VGOID="$(printf '%s' "$VGKEY" | od -An -tx1 | tr -d ' \n')"
	head -c $((1024*1024)) /dev/urandom > "$WORK/vg.bin"
	"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" put "$VGOID" "$WORK/vg.bin" >/dev/null 2>&1
	VGSHA="$(sha256sum "$WORK/vg.bin" | cut -d' ' -f1)"
	valgrind --leak-check=full --error-exitcode=42 --errors-for-leak-kinds=definite,indirect \
		"$DRV" --listen "$FRONT_NA" --addr-file "$ADDR_FILE" \
		--key "$VGKEY" --runtime 1 --module-ns nkvx --module identity \
		--osize $((1024*1024)) --cancel-after 0 --poison-after-cancel 50 --iters 3 \
		--expect 0 --expect-sha256 "$VGSHA" \
		>"$VG_LOG" 2>&1
	vgrc=$?
	"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" rm "$VGOID" >/dev/null 2>&1 || true
	if [ "$vgrc" -eq 0 ]; then
		echo "  PASS: valgrind clean (0 definite/indirect lost, 0 errors)"
		grep -E "definitely lost|indirectly lost|ERROR SUMMARY" "$VG_LOG" | sed 's/^/    /'
	else
		echo "  FAIL: valgrind reported errors/leaks (exit $vgrc):"; grep -E "lost|ERROR SUMMARY|Invalid" "$VG_LOG" | sed 's/^/    /'; fail=1
	fi
	if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then kill "$SVC_PID" 2>/dev/null; wait "$SVC_PID" 2>/dev/null; SVC_PID=""; fi
else
	echo "--- valgrind SKIPPED (valgrind not installed) ---"
fi

echo "--- executor log (tail) ---"; tail -6 "$SVC_LOG" | sed 's/^/  /'
[ "$fail" -eq 0 ] && { echo "RESULT: PASS"; exit 0; } || { echo "RESULT: FAIL"; exit 1; }
