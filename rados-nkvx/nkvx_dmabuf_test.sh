#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Slice S2 dma-buf bulk-path acceptance harness (bead spdk-a27): prove the dma-buf
# result_sink path through OUR Mercury + front + executor stack, end-to-end over
# ofi+verbs;ofi_rxm, WITHOUT the GPU. The result_sink is a HOST udmabuf; the
# executor RDMA-WRITEs a large (default 64 MiB) identity Exec result straight into
# it; we mmap the udmabuf and assert the landed bytes are BIT-EXACT (sha256 ==
# the seeded object's sha). This is the same dma-buf path the GPU capstone (S3)
# will use, minus the GPU (a GPU-VRAM vfio-user P2PDMA sink is a different fd from
# the same na_ofi fi_mr_regattr(FI_MR_DMABUF) registration).
#
# TRANSPORT ARG ($1): defaults to "ofi+verbs;ofi_rxm://10.110.0.1" (the on-rig
# verbs rung). It also runs over "ofi+tcp://127.0.0.1" for a dma-buf-MR
# off-rig smoke (tcp does not RDMA, but the dma-buf MR still registers and the
# bytes still land), and na+sm:// is NOT supported (SM cannot RMA a dma-buf).
#
# PRECONDITIONS (verbs rung): raised RLIMIT_MEMLOCK (irdma pins the CQ/QP), the
# E810 up with irdma, AND access to /dev/udmabuf (root:kvm 0660 — the 'kvm' group
# or run with sufficient privilege). This harness prints the preconditions and
# refuses gracefully if memlock is too low or /dev/udmabuf is unreadable. It never
# raises memlock and never runs as root on your behalf.
#
# Requires a rados-backed executor (built-in `identity` copies the object bytes
# through, so a 64 MiB result == the object and is sha-checkable). Env overrides:
#   CEPH_CONF (default /home/kyle/src/ceph/build/ceph.conf)
#   RADOS     (default /home/kyle/src/ceph/build/bin/rados)
#   KVPOOL    (default kvpool)
#   ITERS     (default 3)   OSIZE (default 64 MiB)
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RADOS_DIR="$(cd "$HERE/../spdk/module/kvdev/rados" && pwd)"
SPDK_ROOT="$(cd "$HERE/../spdk" && pwd)"
MERCURY_PREFIX="${MERCURY_PREFIX:-$SPDK_ROOT/vendor/mercury-install}"
TRANSPORT="${1:-ofi+verbs;ofi_rxm://10.110.0.1}"
CEPH_CONF="${CEPH_CONF:-/home/kyle/src/ceph/build/ceph.conf}"
RADOS="${RADOS:-/home/kyle/src/ceph/build/bin/rados}"
KVPOOL="${KVPOOL:-kvpool}"
ITERS="${ITERS:-3}"
OSIZE="${OSIZE:-$((64*1024*1024))}"
# DRV_NETNS: run the FRONT DRIVER inside this netns so it uses a DISTINCT GID from
# the host-side executor (the second E810 port). This is the two-distinct-GID
# setup that avoids the intra-host ofi_rxm loopback deadlock two endpoints on ONE
# GID hit (same approach as the C8 acceptance rig). The executor stays on the host
# (it needs the host route to the Ceph mons; the netns has only the E810 dataplane).
# The driver needs no Ceph. Empty = both on the host GID (works for tcp / na+sm).
DRV_NETNS="${DRV_NETNS:-}"
DRV_TARGET="${DRV_TARGET:-}"		# explicit front --listen address (e.g. the netns GID)

export LD_LIBRARY_PATH="$MERCURY_PREFIX/lib:${LD_LIBRARY_PATH:-}"

IS_VERBS=0
case "$TRANSPORT" in
	*verbs*) IS_VERBS=1 ;;
	na+sm*) echo "REFUSING: na+sm cannot RMA a dma-buf; use verbs (or tcp for a smoke)."; exit 2 ;;
esac

# Front NA provider == executor address prefix (matches the real bridge).
case "$TRANSPORT" in
	*) FRONT_NA="${TRANSPORT%%://*}://" ;;
esac

# ---- /dev/udmabuf precondition (both rungs need the HOST udmabuf sink) ----
if [ ! -e /dev/udmabuf ]; then
	echo "REFUSING: /dev/udmabuf does not exist (load the udmabuf module: modprobe udmabuf)."
	echo "RESULT: SKIP (udmabuf device absent)"
	exit 3
fi
if [ ! -r /dev/udmabuf ] || [ ! -w /dev/udmabuf ]; then
	echo "WARNING: /dev/udmabuf is not read/write for this user (it is root:kvm 0660)."
	echo "  Join the 'kvm' group (and re-login) or run this harness with privilege."
	echo "  The driver will fail at UDMABUF_CREATE if it cannot open the device."
fi

if [ "$IS_VERBS" -eq 1 ]; then
	echo "############################################################################"
	echo "# S2 dma-buf — ofi+verbs over the E810 DAC (on-rig HITL run)"
	echo "# Needs raised RLIMIT_MEMLOCK (irdma CQ pin) + the E810 up + /dev/udmabuf access."
	echo "# Listen: $TRANSPORT"
	echo "############################################################################"
	LIM="$(ulimit -l 2>/dev/null || echo 0)"
	if [ "$LIM" != "unlimited" ]; then
		if [ "${LIM:-0}" -lt 1048576 ] 2>/dev/null; then
			echo "REFUSING: locked memory too low for verbs (ulimit -l = ${LIM} KiB)."
			echo "  Set '<user> hard memlock unlimited' (limits.d) or systemd LimitMEMLOCK=infinity."
			echo "RESULT: SKIP (memlock too low — HITL precondition unmet)"
			exit 3
		fi
	fi
	echo "memlock OK (ulimit -l = ${LIM})."
fi

echo "== building executor + dma-buf driver =="
make -C "$HERE" -f Makefile MERCURY_PREFIX="$MERCURY_PREFIX" >/dev/null \
	|| { echo "FAIL: build executor"; exit 1; }
make -C "$RADOS_DIR" -f Makefile.dmabuf.ut MERCURY_PREFIX="$MERCURY_PREFIX" >/dev/null \
	|| { echo "FAIL: build dma-buf driver"; exit 1; }

SVC="$HERE/nkvx_service"
DRV="$RADOS_DIR/nkvx_front_dmabuf_test"

BIGKEY="nkvxDmabuf"
BIGOID="$(printf '%s' "$BIGKEY" | od -An -tx1 | tr -d ' \n')"

WORK="$(mktemp -d /tmp/nkvx-dmabuf.XXXXXX)"
ADDR_FILE="$WORK/addr"
SVC_LOG="$WORK/svc.log"
SVC_PID=""

cleanup() {
	if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then kill "$SVC_PID" 2>/dev/null; wait "$SVC_PID" 2>/dev/null; fi
	"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" rm "$BIGOID" >/dev/null 2>&1 || true
	rm -rf "$WORK"
}
trap cleanup EXIT

# Seed the object the identity Exec copies through; its sha is the bit-exact target.
head -c "$OSIZE" /dev/urandom > "$WORK/big.bin"
"$RADOS" -c "$CEPH_CONF" -p "$KVPOOL" put "$BIGOID" "$WORK/big.bin" >/dev/null 2>&1 \
	|| { echo "FAIL: seed $OSIZE-byte object (Ceph up?)"; exit 1; }
BIGSHA="$(sha256sum "$WORK/big.bin" | cut -d' ' -f1)"

rm -f "$ADDR_FILE"
"$SVC" --listen "$TRANSPORT" --addr-file "$ADDR_FILE" \
	--rados-pool "$KVPOOL" --rados-conf "$CEPH_CONF" --rados-user admin >>"$SVC_LOG" 2>&1 &
SVC_PID=$!
for i in $(seq 1 150); do
	[ -s "$ADDR_FILE" ] && break
	kill -0 "$SVC_PID" 2>/dev/null || { echo "FAIL: executor exited at boot"; tail -20 "$SVC_LOG"; exit 1; }
	sleep 0.1
done
[ -s "$ADDR_FILE" ] || { echo "FAIL: executor published no address"; tail -20 "$SVC_LOG"; exit 1; }

# The driver front --listen: DRV_TARGET overrides FRONT_NA (use the netns GID so the
# driver binds its endpoint to the second E810 port). Wrap in `ip netns exec` when
# DRV_NETNS is set (distinct GID -> no rxm loopback deadlock). /dev/udmabuf is global,
# so the driver opens it the same inside a netns.
DRV_LISTEN="${DRV_TARGET:-$FRONT_NA}"
NS_PREFIX=()
if [ -n "$DRV_NETNS" ]; then
	NS_PREFIX=(ip netns exec "$DRV_NETNS")
	echo "== driver runs in netns '$DRV_NETNS' (distinct GID; listen=$DRV_LISTEN) =="
fi

echo "== S2 dma-buf e2e (transport=$TRANSPORT front=$DRV_LISTEN osize=$OSIZE iters=$ITERS) =="
"${NS_PREFIX[@]}" "$DRV" --listen "$DRV_LISTEN" --addr-file "$ADDR_FILE" \
	--key "$BIGKEY" --runtime 1 --module-ns nkvx --module identity \
	--osize "$OSIZE" --iters "$ITERS" --expect-sha256 "$BIGSHA" 2>&1 | sed 's/^/  /'
rc=${PIPESTATUS[0]}

kill "$SVC_PID" 2>/dev/null; wait "$SVC_PID" 2>/dev/null; SVC_PID=""

echo "--- executor log (tail) ---"; tail -6 "$SVC_LOG" | sed 's/^/  /'
if [ "$rc" -eq 0 ]; then
	echo "RESULT: PASS — identity result RDMA-written BIT-EXACT into the HOST udmabuf result_sink over verbs"
	exit 0
else
	echo "RESULT: FAIL (driver exit $rc — see output above)"
	exit 1
fi
