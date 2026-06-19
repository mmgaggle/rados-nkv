#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Bead spdk-8od — dedicated regression guards for the TWO cross-process UAF holes
# fixed during the C6b review (bead spdk-5ia). These are TEST-ONLY: the protocol
# logic is unchanged; this harness proves the two fixes hold (and would FAIL if
# either regressed).
#
#   F1 (input-bulk cancel UAF). A large-INPUT / inline-or-small-result Exec has a
#       live input_bulk (the executor is PULLing it) but NO result_sink. The bug:
#       nkvx_call_begin_cancel skipped the phase-2 cancel handshake whenever
#       result_sink == HG_BULK_NULL, ignoring the live input MR — the front released
#       the input buffer/MR without telling the executor (cross-process UAF). FIX:
#       skip phase 2 only when BOTH result_sink AND input_bulk are NULL.
#       This test drives such an Exec, deterministically holds the executor input
#       PULL in flight via the NEW NKVX_TEST_PULL_STALL_TICKS hook, cancels via
#       nkvx_front_cancel_all, and asserts the executor LOGGED a cancel case for the
#       call (phase 2 ACTUALLY RAN — NOT origin-only-skip), the input PULL was
#       skipped post-ack (no late PULL), and the front saw exactly-once ABORTED +
#       drained. REGRESSION SIGNAL: with the bug, no nkvx_cancel RPC reaches the
#       executor at all, so NO "cancel ... case=" / "no late PULL" line appears.
#
#   F2 (same-op_id concurrent-cancel UAF). Two CONCURRENT in-flight Execs from one
#       front sharing the tenant op_id (both 0x4242) but with DISTINCT front-unique
#       client_call_ids. The bug: the executor registry was keyed on op_id (the
#       tenant CDW13, NOT unique among a front's concurrent Execs), so the second
#       cancel aliased the first req and acked the other ALREADY_DONE-as-duplicate
#       early. FIX: key the registry on (origin, client_call_id). This test keeps
#       both Execs in flight via NKVX_TEST_PUSH_STALL_TICKS, cancels both, and
#       asserts the executor handled TWO DISTINCT cancelled call_ids (no cross-abort,
#       no early ALREADY_DONE between the pair) and both front cbs fired exactly-once
#       ABORTED. REGRESSION SIGNAL: with the bug, only ONE distinct call_id reaches a
#       case-b/c cancel and the other is acked ALREADY_DONE (case=a).
#
#   (valgrind) F1 + F2 under valgrind --leak-check=full: 0 errors, 0 lost.
#
# Runs on BOTH na+sm:// and ofi+tcp://127.0.0.1 (pass the provider as $1). Needs a
# rados-backed executor (the CLS "identity" module copies the object bytes through;
# bytecount returns 8 bytes). Requires Ceph. Env overrides: CEPH_CONF, RADOS, KVPOOL.
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

# F1 fixture: a SMALL object (small result -> rides inline, NO result_sink). The
# large payload is the INPUT (registered as input_bulk), supplied by the driver.
F1KEY="nkvxF1"
F1OID="$(printf '%s' "$F1KEY" | od -An -tx1 | tr -d ' \n')"
# F2 fixture: a LARGE object so the result takes the PUSH path -> each Exec registers
# a result_sink and stalls pre-PUSH (keeping both in flight while we cancel them).
F2KEY="nkvxF2"
F2OID="$(printf '%s' "$F2KEY" | od -An -tx1 | tr -d ' \n')"
F2SZ=$((1024*1024))

WORK="$(mktemp -d /tmp/nkvx-c6b-f1f2.XXXXXX)"
ADDR_FILE="$WORK/addr"
SVC_LOG="$WORK/svc.log"
SVC_PID=""

cleanup() {
	if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then kill "$SVC_PID" 2>/dev/null; wait "$SVC_PID" 2>/dev/null; fi
	"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" rm "$F1OID" >/dev/null 2>&1 || true
	"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" rm "$F2OID" >/dev/null 2>&1 || true
	rm -rf "$WORK"
}
trap cleanup EXIT

printf 'small-f1-value' | "$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" put "$F1OID" /dev/stdin >/dev/null 2>&1 \
	|| { echo "FAIL: seed F1 object (Ceph up?)"; exit 1; }
head -c "$F2SZ" /dev/urandom > "$WORK/f2.bin"
"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" put "$F2OID" "$WORK/f2.bin" >/dev/null 2>&1 \
	|| { echo "FAIL: seed F2 object"; exit 1; }

# Start the executor. $1 = PUSH stall ticks, $2 = PULL stall ticks (0 disables).
start_svc() {
	NKVX_TEST_PUSH_STALL_TICKS="${1:-0}" NKVX_TEST_PULL_STALL_TICKS="${2:-0}" \
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

stop_svc() {
	if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then kill "$SVC_PID" 2>/dev/null; wait "$SVC_PID" 2>/dev/null; SVC_PID=""; fi
}

echo "== C6b F1/F2 cross-process UAF regression guards (transport=$TRANSPORT front=$FRONT_NA) =="
fail=0

# ---------------------------------------------------------------------------
# F1: large-input / inline-result Exec cancelled while the input PULL is stalled.
# result_sink == NULL but input_bulk != NULL -> the front MUST still run phase 2.
# --osize 64 (<= NKVX_INLINE_MAX) => no result_sink. --input-len 65536 (> 4 KiB)
# => a registered input_bulk. NKVX_TEST_PULL_STALL_TICKS holds the executor PULL in
# flight so the cancel deterministically lands during it.
# ---------------------------------------------------------------------------
echo "--- F1 input-bulk cancel (PULL stall) -> phase-2 handshake ran, no late PULL ---"
start_svc 0 8 || exit 1
"$DRV" --listen "$FRONT_NA" --addr-file "$ADDR_FILE" \
	--key "$F1KEY" --runtime 2 --module-ns nkvx --module identity \
	--osize 64 --input-len $((64*1024)) --cancel-after 0 --iters 3 \
	--expect 0 2>&1 | sed 's/^/  /'
drc=${PIPESTATUS[0]}
[ "$drc" -eq 0 ] || { echo "  FAIL: F1 driver exit $drc"; fail=1; }
kill -0 "$SVC_PID" 2>/dev/null || { echo "  FAIL: executor died during F1"; fail=1; }

# The DISTINGUISHER: phase 2 actually ran for this input-only Exec. With the F1 bug
# (skip phase 2 on result_sink==NULL), NO nkvx_cancel RPC reaches the executor, so
# NONE of these lines appear. Their presence == phase-2-happened, not origin-only.
if grep -Eq "cancel ack op_id=.*case=[bc]|cancel op_id=.*case=[bc]" "$SVC_LOG"; then
	echo "  PASS: executor logged a cancel case for the input-only Exec (phase 2 RAN)"
else
	echo "  FAIL: NO executor cancel-case line — phase 2 was SKIPPED (F1 regression!)"; fail=1
fi
if grep -Eq "no late PULL|input PULL skipped" "$SVC_LOG"; then
	echo "  PASS: executor skipped the input PULL post-cancel (no late PULL)"
else
	echo "  FAIL: no 'no late PULL' / 'input PULL skipped' line (F1 regression?)"; fail=1
fi
stop_svc

# ---------------------------------------------------------------------------
# F2: two CONCURRENT same-op_id Execs (both 0x4242, distinct front call_ids), kept
# in flight via the PUSH stall, both cancelled. The executor must handle TWO DISTINCT
# call_ids — no cross-abort, no early ALREADY_DONE between the pair.
# ---------------------------------------------------------------------------
echo "--- F2 same-op_id concurrent cancel -> two distinct call_ids, no cross-abort ---"
rm -f "$ADDR_FILE"
start_svc 30 0 || exit 1
"$DRV" --listen "$FRONT_NA" --addr-file "$ADDR_FILE" \
	--key "$F2KEY" --runtime 2 --module-ns nkvx --module identity \
	--osize "$F2SZ" --concurrent 2 --cancel-after 4 --expect 0 2>&1 | sed 's/^/  /'
drc=${PIPESTATUS[0]}
[ "$drc" -eq 0 ] || { echo "  FAIL: F2 driver exit $drc"; fail=1; }
kill -0 "$SVC_PID" 2>/dev/null || { echo "  FAIL: executor died during F2"; fail=1; }

# Count DISTINCT call_ids that the executor actually acked a cancel for. With the F2
# bug (registry keyed on op_id), the two same-op_id reqs alias: only ONE distinct
# call_id gets a real (case b/c) cancel+ack and the other is acked ALREADY_DONE
# (case=a). With the fix BOTH distinct call_ids are cancelled independently.
NDISTINCT=$(grep -Eo "cancel ack op_id=[0-9]+ call_id=[0-9]+ case=[bc]" "$SVC_LOG" \
	| grep -oE "call_id=[0-9]+" | sort -u | wc -l)
NDUP=$(grep -Ec "cancel op_id=.* case=a \(duplicate\)" "$SVC_LOG")
echo "    distinct independently-cancelled call_ids: $NDISTINCT ; duplicate-aliased acks: $NDUP"
if [ "$NDISTINCT" -eq 2 ] && [ "$NDUP" -eq 0 ]; then
	echo "  PASS: both same-op_id Execs cancelled independently (no aliasing)"
else
	echo "  FAIL: same-op_id cancels aliased (distinct=$NDISTINCT dup=$NDUP; F2 regression!)"; fail=1
fi
stop_svc

# ---------------------------------------------------------------------------
# valgrind: F1 + F2 cancel paths must be UAF/leak clean.
# ---------------------------------------------------------------------------
if command -v valgrind >/dev/null 2>&1; then
	echo "--- valgrind --leak-check=full over F1 + F2 ---"
	VG_LOG="$WORK/vg.log"

	rm -f "$ADDR_FILE"; start_svc 0 8 || exit 1
	valgrind --leak-check=full --error-exitcode=42 --errors-for-leak-kinds=definite,indirect \
		"$DRV" --listen "$FRONT_NA" --addr-file "$ADDR_FILE" \
		--key "$F1KEY" --runtime 2 --module-ns nkvx --module identity \
		--osize 64 --input-len $((64*1024)) --cancel-after 0 --iters 3 --expect 0 \
		>"$VG_LOG" 2>&1
	vgrc=$?
	stop_svc
	if [ "$vgrc" -eq 0 ]; then
		echo "  PASS: F1 valgrind clean"
		grep -E "definitely lost|indirectly lost|ERROR SUMMARY" "$VG_LOG" | sed 's/^/    /'
	else
		echo "  FAIL: F1 valgrind errors/leaks (exit $vgrc):"; grep -E "lost|ERROR SUMMARY|Invalid" "$VG_LOG" | sed 's/^/    /'; fail=1
	fi

	rm -f "$ADDR_FILE"; start_svc 30 0 || exit 1
	valgrind --leak-check=full --error-exitcode=42 --errors-for-leak-kinds=definite,indirect \
		"$DRV" --listen "$FRONT_NA" --addr-file "$ADDR_FILE" \
		--key "$F2KEY" --runtime 2 --module-ns nkvx --module identity \
		--osize "$F2SZ" --concurrent 2 --cancel-after 4 --expect 0 \
		>"$VG_LOG" 2>&1
	vgrc=$?
	stop_svc
	if [ "$vgrc" -eq 0 ]; then
		echo "  PASS: F2 valgrind clean"
		grep -E "definitely lost|indirectly lost|ERROR SUMMARY" "$VG_LOG" | sed 's/^/    /'
	else
		echo "  FAIL: F2 valgrind errors/leaks (exit $vgrc):"; grep -E "lost|ERROR SUMMARY|Invalid" "$VG_LOG" | sed 's/^/    /'; fail=1
	fi
else
	echo "--- valgrind SKIPPED (valgrind not installed) ---"
fi

echo "--- executor log (tail) ---"; tail -6 "$SVC_LOG" | sed 's/^/  /'
[ "$fail" -eq 0 ] && { echo "RESULT: PASS"; exit 0; } || { echo "RESULT: FAIL"; exit 1; }
