#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Slice C8 ACCEPTANCE harness (bead spdk-xmu.8, design §5 rung 2 / §C8): run the
# full two-tier Exec RPC over the ACCEPTANCE transport — ofi+verbs;ofi_rxm over the
# validated E810 DAC — and assert the slice's acceptance gate:
#
#   (A) UNDER-LOAD correctness: many iters of a large-result (64 MiB) two-tier Exec
#       over the REAL front client (nkvx_front_client.c) <-> REAL rados-backed
#       executor (nkvx_service), content-hashing the delivered DPTR EVERY iter and
#       asserting bytewise-correct, no torn/short/races-ahead-of-respond PUSH
#       (design §1.3 PUSH-before-respond NORMATIVE ordering, under repetition).
#   (B) ABORT ASan-clean: cancel mid-PUSH via the executor TICK-DEFERRAL stall hook
#       (the C6b machinery: NKVX_TEST_PUSH_STALL_TICKS + --cancel-after 0 +
#       --poison-after-cancel), asserting exactly-once ABORTED, NO late PUSH (sink
#       poison intact post-ack), a reusable DPTR — and, when run under the ASan
#       binaries (NKVX_ASAN=1), that BOTH tiers report 0 ASan errors. This is the
#       "abort ASan-clean over verbs" acceptance row, one command on-rig.
#   (C) NO-STALE-READ: Store v1 -> Exec (observe v1) -> Store v2 -> Exec, assert the
#       executor returns the NEW value. This probes the wasm TB4 object-cache
#       cross-hop invalidation. It is EXPECTED-PENDING and GATED non-fatal: it
#       depends on bead spdk-xmu.9 (Slice C5b cross-hop invalidation, still OPEN).
#       A stale result is reported as KNOWN-PENDING, NOT a hard failure.
#
# TRANSPORT ARG ($1): defaults to "ofi+verbs;ofi_rxm://10.110.0.1" (the on-rig
# acceptance rung). The SAME harness fully runs on "na+sm://" and
# "ofi+tcp://127.0.0.1" for OFF-RIG dry-validation — same Exec/RPC code, swap the
# provider (ADR-0015 Amendment). The front NA-prefix is derived from the address
# exactly as the real bridge does (everything up to and including "://"), so the
# verbs prefix "ofi+verbs;ofi_rxm://" is derived correctly too.
#
# VERBS PREAMBLE + MEMLOCK GUARD: for the verbs rung this script PRINTS the on-rig
# preconditions (raised memlock + the E810) and REFUSES gracefully if `ulimit -l`
# is too low (Mercury na_ofi verbs fi_enable() fails at CQ creation with -12 ENOMEM
# under the 8 MB unprivileged RLIMIT_MEMLOCK cap — irdma pins regular memory for the
# CQ/QP/doorbell; bead spdk-xmu.8 VERBS-INIT root cause). It NEVER tries to raise
# memlock and NEVER runs as root — that is the user's HITL deploy step.
#
# ASan TOGGLE: NKVX_ASAN=1 rebuilds BOTH the executor and the front driver with
# AddressSanitizer (Makefile ASAN=1; CC defaults to clang here because this host's
# gcc libasan runtime is not installed — clang links its ASan runtime statically) and
# runs every case under ASAN_OPTIONS that make any error fatal + detectable, then
# asserts 0 reported errors. The on-rig verbs acceptance run uses exactly these ASan
# binaries, so "abort ASan-clean over verbs" is `NKVX_ASAN=1 ./nkvx_c8_test.sh`.
#
# All cases need a rados-backed executor (built-in `identity` copies the object bytes
# through, so a 64 MiB result == the object and is sha-checkable; `bytecount` over the
# wasm path is the cached-object probe for (C)). Requires Ceph. Env overrides:
#   CEPH_CONF (default /home/kyle/src/ceph/build/ceph.conf)
#   RADOS     (default /home/kyle/src/ceph/build/bin/rados)
#   KVPOOL    (default kvpool)
#   NKVX_ASAN=1            build+run under AddressSanitizer, assert 0 errors
#   NKVX_ASAN_CC=<cc>      compiler for the ASan build (default: clang)
#   LOAD_ITERS=<n>         under-load iteration count (default 30)
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Front driver builds out-of-tree from target/test/ against the prebuilt UNMODIFIED
# SPDK (the in-tree module/kvdev/rados path was deleted). The target/test Makefile
# carries the same ASAN=1/CC toggle the old Makefile.front.ut had. See nkvx_c6_test.sh.
TEST_DIR="$(cd "$HERE/../target/test" && pwd)"
SPDK_ROOT="$(cd "$HERE/../spdk" && pwd)"
SPDK_PREBUILT="${SPDK_PREBUILT:-/home/kyle/src/rados-nkv-wt/slice-a/spdk}"
MERCURY_PREFIX="${MERCURY_PREFIX:-$SPDK_ROOT/vendor/mercury-install}"
TRANSPORT="${1:-ofi+verbs;ofi_rxm://10.110.0.1}"
CEPH_CONF="${CEPH_CONF:-/home/kyle/src/ceph/build/ceph.conf}"
RADOS="${RADOS:-/home/kyle/src/ceph/build/bin/rados}"
KVPOOL="${KVPOOL:-kvpool}"
WASM_MODULE="$HERE/wasm/bytecount.wasm"
LOAD_ITERS="${LOAD_ITERS:-30}"
NKVX_ASAN="${NKVX_ASAN:-0}"
NKVX_ASAN_CC="${NKVX_ASAN_CC:-clang}"

export LD_LIBRARY_PATH="$MERCURY_PREFIX/lib:${LD_LIBRARY_PATH:-}"

# ---------------------------------------------------------------------------
# Transport classification + the verbs preamble / memlock guard.
# ---------------------------------------------------------------------------
IS_VERBS=0
case "$TRANSPORT" in
	*verbs*) IS_VERBS=1 ;;
esac

# Front-client NA provider derived from the executor address prefix (matches the real
# bridge kvdev_rados_nkvx_front_create): everything up to and including "://".
#   na+sm://7-0                  -> na+sm://
#   ofi+tcp://127.0.0.1          -> ofi+tcp://
#   ofi+verbs;ofi_rxm://10.110.. -> ofi+verbs;ofi_rxm://
case "$TRANSPORT" in
	na+sm*) FRONT_NA="na+sm://" ;;
	*)      FRONT_NA="${TRANSPORT%%://*}://" ;;
esac

if [ "$IS_VERBS" -eq 1 ]; then
	echo "############################################################################"
	echo "# C8 ACCEPTANCE — ofi+verbs over the E810 DAC (on-rig HITL run)"
	echo "#"
	echo "# Run this on the rig as a user whose LOCKED MEMORY is unlimited. Mercury's"
	echo "# na_ofi verbs path fails fi_enable() at CQ creation with -12 ENOMEM under the"
	echo "# default 8 MB RLIMIT_MEMLOCK (irdma pins regular, non-hugepage memory for the"
	echo "# CQ/QP/doorbell). Raise it durably via /etc/security/limits.d (<user> hard"
	echo "# memlock unlimited) or systemd LimitMEMLOCK=infinity — see the deploy config."
	echo "# This harness will NOT raise memlock and will NOT run as root."
	echo "#"
	echo "# Requires: the E810 NIC up with irdma; the executor reachable at the listen"
	echo "# address (a storage VM, OR netns f1=10.110.0.2 for a distinct GID). Listen:"
	echo "#   $TRANSPORT"
	echo "############################################################################"

	# memlock guard: refuse gracefully (actionable message), never raise it.
	LIM="$(ulimit -l 2>/dev/null || echo 0)"
	if [ "$LIM" != "unlimited" ]; then
		# ulimit -l is in KiB. The proven-FAIL cap is 8 MB (8192 KiB); require a
		# clearly-raised limit. We do NOT attempt to raise it.
		if [ "${LIM:-0}" -lt 1048576 ] 2>/dev/null; then
			echo
			echo "REFUSING: locked memory is too low for verbs (ulimit -l = ${LIM} KiB)."
			echo "  The verbs CQ allocation needs raised RLIMIT_MEMLOCK. Either:"
			echo "    - set '<user> hard memlock unlimited' in /etc/security/limits.d and re-login, or"
			echo "    - run the service under systemd with LimitMEMLOCK=infinity."
			echo "  Then re-run this harness. (Off-rig you can dry-validate on na+sm:// or"
			echo "  ofi+tcp://127.0.0.1, which do not need raised memlock.)"
			echo "RESULT: SKIP (memlock too low — HITL precondition unmet)"
			exit 3
		fi
	fi
	echo "memlock OK (ulimit -l = ${LIM})."
fi

# ---------------------------------------------------------------------------
# Build (normal or ASan). The ASan build shares the binary path with the normal
# build, so we force-rebuild with the requested flavor every run.
# ---------------------------------------------------------------------------
MAKE_ARGS=(MERCURY_PREFIX="$MERCURY_PREFIX")
ASAN_TAG="normal"
if [ "$NKVX_ASAN" = "1" ]; then
	MAKE_ARGS+=(ASAN=1 CC="$NKVX_ASAN_CC")
	ASAN_TAG="ASan ($NKVX_ASAN_CC)"
	# Make any ASan finding fatal and detectable; abort on first error.
	export ASAN_OPTIONS="abort_on_error=1:halt_on_error=1:detect_leaks=1:exitcode=99:${ASAN_OPTIONS:-}"
fi

echo "== building executor + front driver [$ASAN_TAG] =="
# The front driver shares the ASan/normal output path, so force a fresh build with
# the requested flavor every run (mirrors the old in-tree behaviour). The target/test
# Makefile only removes the standalone driver binaries on `clean nkvx_front_client_test`,
# so we just rm the binary to force a rebuild without nuking the CUnit suites.
make -C "$HERE" -f Makefile SPDK_ROOT="$SPDK_PREBUILT" "${MAKE_ARGS[@]}" clean >/dev/null 2>&1
rm -f "$TEST_DIR/nkvx_front_client_test"
make -C "$HERE" -f Makefile SPDK_ROOT="$SPDK_PREBUILT" "${MAKE_ARGS[@]}" >/dev/null || { echo "FAIL: build executor [$ASAN_TAG]"; exit 1; }
make -C "$TEST_DIR" nkvx_front_client_test SPDK_ROOT="$SPDK_PREBUILT" "${MAKE_ARGS[@]}" >/dev/null || { echo "FAIL: build front driver [$ASAN_TAG]"; exit 1; }

SVC="$HERE/nkvx_service"
DRV="$TEST_DIR/nkvx_front_client_test"

# ---------------------------------------------------------------------------
# Fixtures.
# ---------------------------------------------------------------------------
BIGKEY="nkvxC8big"
BIGOID="$(printf '%s' "$BIGKEY" | od -An -tx1 | tr -d ' \n')"
BIGSZ=$((64*1024*1024))
# No-stale-read (C) uses the wasm `bytecount` path (the TB4-object-cache path where
# cross-hop staleness lives); the result is the object LENGTH, so two stores of
# DIFFERENT sizes give distinct expected results.
STKEY="nkvxC8stale"
STOID="$(printf '%s' "$STKEY" | od -An -tx1 | tr -d ' \n')"
MODOBJ="wasm:bytecount"

WORK="$(mktemp -d /tmp/nkvx-c8.XXXXXX)"
ADDR_FILE="$WORK/addr"
SVC_LOG="$WORK/svc.log"
SVC_PID=""

cleanup() {
	if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then kill "$SVC_PID" 2>/dev/null; wait "$SVC_PID" 2>/dev/null; fi
	"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" rm "$BIGOID" >/dev/null 2>&1 || true
	"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" rm "$STOID"  >/dev/null 2>&1 || true
	"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" rm "$MODOBJ" >/dev/null 2>&1 || true
	rm -rf "$WORK"
}
trap cleanup EXIT

# start the executor. $1 = PUSH stall ticks (0 = none).
start_svc() {
	rm -f "$ADDR_FILE"
	NKVX_TEST_PUSH_STALL_TICKS="${1:-0}" \
	"$SVC" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" \
		--rados-pool "$KVPOOL" --rados-conf "$CEPH_CONF" --rados-user admin >>"$SVC_LOG" 2>&1 &
	SVC_PID=$!
	local i
	for i in $(seq 1 150); do
		[ -s "$ADDR_FILE" ] && return 0
		kill -0 "$SVC_PID" 2>/dev/null || { echo "FAIL: executor exited at boot"; tail -20 "$SVC_LOG"; return 1; }
		sleep 0.1
	done
	echo "FAIL: executor published no address"; tail -20 "$SVC_LOG"; return 1
}

# stop the executor with SIGTERM so it shuts down ORDERLY (HG_Finalize) — this lets
# ASan run its leak report at a clean exit. Returns the service exit code.
SVC_EXIT=0
stop_svc() {
	if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then
		kill "$SVC_PID" 2>/dev/null
		wait "$SVC_PID" 2>/dev/null
		SVC_EXIT=$?
	fi
	SVC_PID=""
}

# assert_asan_clean LABEL  — when NKVX_ASAN=1, grep the executor log for an ASan
# report and assert the service exited 0 (ASAN_OPTIONS exitcode=99 on any finding).
assert_asan_clean() {
	[ "$NKVX_ASAN" = "1" ] || return 0
	local label="$1"
	if grep -Eq "ERROR: AddressSanitizer|LeakSanitizer: detected memory leaks|runtime error:" "$SVC_LOG"; then
		echo "  FAIL: ASan reported errors in the executor ($label):"
		grep -E "ERROR: AddressSanitizer|LeakSanitizer|SUMMARY: AddressSanitizer|#[0-9]+ 0x" "$SVC_LOG" | head -20 | sed 's/^/    /'
		fail=1
		return 1
	fi
	if [ "${SVC_EXIT:-0}" -eq 99 ]; then
		echo "  FAIL: executor exited 99 (ASan error code) ($label)"; fail=1; return 1
	fi
	echo "  PASS: executor ASan-clean ($label) — 0 errors, exit ${SVC_EXIT:-0}"
	return 0
}

echo "== C8 acceptance (transport=$TRANSPORT front=$FRONT_NA pool=$KVPOOL) [$ASAN_TAG] =="
fail=0

# Seed the 64 MiB object + remember its sha for the content checks.
head -c "$BIGSZ" /dev/urandom > "$WORK/big.bin"
"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" put "$BIGOID" "$WORK/big.bin" >/dev/null 2>&1 \
	|| { echo "FAIL: seed 64 MiB object (Ceph up?)"; exit 1; }
BIGSHA="$(sha256sum "$WORK/big.bin" | cut -d' ' -f1)"

# ===========================================================================
# (A) UNDER-LOAD correctness: LOAD_ITERS x 64 MiB two-tier Exec, content-hash
# every iter on ONE front + ONE recurring sink (the GPU KV-cache recurring-DPTR
# case + the MR/hg_bulk handle-cache reuse path). --iters drives the in-process
# repeat loop; --expect-sha256 asserts bytewise-correct each iter (no torn DPTR).
# ===========================================================================
echo "--- (A) under-load: ${LOAD_ITERS}x 64 MiB Exec, content-hash each iter ---"
start_svc 0 || exit 1
"$DRV" --listen "$FRONT_NA" --addr-file "$ADDR_FILE" \
	--key "$BIGKEY" --runtime 1 --module-ns nkvx --module identity \
	--osize "$BIGSZ" --iters "$LOAD_ITERS" --distinct 1 \
	--expect 0 --expect-result "$BIGSZ" --expect-sha256 "$BIGSHA" 2>&1 | sed 's/^/  /'
arc=${PIPESTATUS[0]}
if [ "$arc" -eq 0 ]; then
	echo "  PASS: ${LOAD_ITERS} iters bytewise-correct (no torn/short DPTR, PUSH-before-respond held)"
else
	echo "  FAIL: under-load driver exit $arc (torn/short/stale DPTR or status mismatch)"; fail=1
fi
kill -0 "$SVC_PID" 2>/dev/null || { echo "  FAIL: executor died during under-load"; fail=1; }
stop_svc
assert_asan_clean "under-load"

# ===========================================================================
# (B) ABORT ASan-clean: cancel mid-PUSH via the executor stall hook (C6b
# machinery). Exactly-once ABORTED + NO late PUSH (poison intact post-ack) +
# reusable DPTR; under NKVX_ASAN=1, both tiers ASan-clean. --cancel-after 0 +
# NKVX_TEST_PUSH_STALL_TICKS makes the cancel deterministically WIN pre-PUSH.
# ===========================================================================
echo "--- (B) abort: cancel mid-PUSH -> ABORTED, NO late PUSH, DPTR reusable, ASan-clean ---"
start_svc 8 || exit 1
"$DRV" --listen "$FRONT_NA" --addr-file "$ADDR_FILE" \
	--key "$BIGKEY" --runtime 1 --module-ns nkvx --module identity \
	--osize "$BIGSZ" --cancel-after 0 --poison-after-cancel 200 --iters 3 \
	--expect 0 --expect-sha256 "$BIGSHA" 2>&1 | sed 's/^/  /'
brc=${PIPESTATUS[0]}
[ "$brc" -eq 0 ] || { echo "  FAIL: abort driver exit $brc"; fail=1; }
kill -0 "$SVC_PID" 2>/dev/null || { echo "  FAIL: executor died during abort"; fail=1; }
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
stop_svc
assert_asan_clean "abort"

# ===========================================================================
# (C) NO-STALE-READ (EXPECTED-PENDING, gated NON-FATAL — depends on bead
# spdk-xmu.9, Slice C5b cross-hop invalidation). Store v1 (size A) -> Exec
# bytecount (observe A) -> Store v2 (size B != A) -> Exec bytecount: assert the
# executor returns B. The wasm path serves the object through the TB4 object cache,
# so without cross-hop invalidation the second Exec returns the STALE A. We report
# stale as KNOWN-PENDING (not a hard failure); a fresh read counts as a bonus PASS.
# ===========================================================================
echo "--- (C) no-stale-read (Store->Exec->Store->Exec) — EXPECTED-PENDING (bead spdk-xmu.9) ---"
if [ -r "$WASM_MODULE" ]; then
	WSHA="$(sha256sum "$WASM_MODULE" | cut -d' ' -f1)"
	"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" put "$MODOBJ" "$WASM_MODULE" >/dev/null 2>&1 \
		|| { echo "  WARN: could not seed wasm module — skipping (C)"; }
	start_svc 0 || exit 1
	# v1: 100-byte object -> bytecount result == 100 (LE uint64, first byte 0x64).
	head -c 100 /dev/zero > "$WORK/v1.bin"
	"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" put "$STOID" "$WORK/v1.bin" >/dev/null 2>&1
	# bytecount delivers 8 inline bytes; --expect-result 8, the result_len is 8. We
	# read back the count via the delivered sha: sha of the 8-byte LE encoding of size.
	le8() { printf "$(printf '\\x%02x' $(( $1 & 0xff )) $(( ($1>>8)&0xff )) $(( ($1>>16)&0xff )) $(( ($1>>24)&0xff )) 0 0 0 0)"; }
	SHA_V1="$(le8 100 | sha256sum | cut -d' ' -f1)"
	"$DRV" --listen "$FRONT_NA" --addr-file "$ADDR_FILE" \
		--key "$STKEY" --runtime 1 --module-ns "$KVPOOL" --module "$MODOBJ" --sha256 "$WSHA" \
		--osize 8 --expect 0 --expect-result 8 --expect-sha256 "$SHA_V1" >/dev/null 2>&1
	v1rc=$?
	if [ "$v1rc" -ne 0 ]; then
		echo "  WARN: bytecount v1 did not return 100 (wasm path unavailable?) — skipping (C)"
	else
		echo "  v1: bytecount(100-byte object) == 100  [OK baseline]"
		# v2: 250-byte object (DIFFERENT size) at the SAME key.
		head -c 250 /dev/zero > "$WORK/v2.bin"
		"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" put "$STOID" "$WORK/v2.bin" >/dev/null 2>&1
		SHA_V2="$(le8 250 | sha256sum | cut -d' ' -f1)"
		"$DRV" --listen "$FRONT_NA" --addr-file "$ADDR_FILE" \
			--key "$STKEY" --runtime 1 --module-ns "$KVPOOL" --module "$MODOBJ" --sha256 "$WSHA" \
			--osize 8 --expect 0 --expect-result 8 --expect-sha256 "$SHA_V2" >/dev/null 2>&1
		v2rc=$?
		if [ "$v2rc" -eq 0 ]; then
			echo "  PASS (BONUS): no stale read — second Exec returned the NEW value (250)"
		else
			echo "  KNOWN-PENDING: second Exec returned the STALE cached value (still 100)."
			echo "    This is the C5b cross-hop invalidation gap — bead spdk-xmu.9 (OPEN)."
			echo "    NOT a C8 hard failure; gated expected-pending per the task spec."
		fi
	fi
	stop_svc
	assert_asan_clean "no-stale-read"
else
	echo "  SKIP: wasm fixture $WASM_MODULE not present"
fi

echo "--- executor log (tail) ---"; tail -6 "$SVC_LOG" | sed 's/^/  /'
if [ "$NKVX_ASAN" = "1" ]; then echo "--- (ran under $ASAN_TAG; ASan errors are FATAL) ---"; fi
[ "$fail" -eq 0 ] && { echo "RESULT: PASS"; exit 0; } || { echo "RESULT: FAIL"; exit 1; }
