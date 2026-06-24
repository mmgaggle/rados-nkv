#!/usr/bin/env bash
# Stage the OUT-OF-TREE build context and build the rados-nkv datapath image
# (Slice I, spdk-7sr.17.10).
#
# Unlike the former in-tree build (which compiled the whole SPDK fork in-image),
# this stages a PRE-BUILT, UNMODIFIED SPDK base (commit X) plus its DPDK and the
# vendored Mercury, and the image builds only the out-of-tree forwarder
# (target/nkv_tgt) + executor (rados-nkvx). The base SPDK is a LOCAL commit not
# on any remote, so it MUST come from the host's prebuilt tree.
#
# The prebuilt SPDK's shared objects bake absolute host RUNPATHs, and its
# libspdk.so is a GROUP linker script of bare sonames, so the context stages
# those trees and the Dockerfile restores them at the SAME absolute paths.
#
# Env knobs (all optional):
#   REGISTRY      default quay.io/mmgaggle
#   ENGINE        default podman
#   CEPH_RELEASE  default devel       (tag prefix + floating :devel)
#   PUSH          default 0
#   CTX           default /tmp/nkv-ctx
#   SPDK_BASE_DIR prebuilt SPDK X tree  (default below)
#   DPDK_DIR      DPDK build the base links (default below)
#   MERCURY_DIR   vendored Mercury install  (default below)
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ctx=${CTX:-/tmp/nkv-ctx}
REGISTRY="${REGISTRY:-quay.io/mmgaggle}"
CEPH_RELEASE="${CEPH_RELEASE:-devel}"
ENGINE="${ENGINE:-podman}"
push="${PUSH:-0}"

# These three MUST match the ARG defaults in packaging/container/Dockerfile, and
# the image restores each at exactly this absolute path so the prebuilt SPDK's
# RUNPATHs resolve unchanged.
SPDK_BASE_DIR="${SPDK_BASE_DIR:-/home/kyle/src/rados-nkv-wt/slice-a/spdk}"
DPDK_DIR="${DPDK_DIR:-/home/kyle/src/rados-nkv/spdk/dpdk/build}"
MERCURY_DIR="${MERCURY_DIR:-/home/kyle/src/rados-nkv/spdk/vendor/mercury-install}"

version=$(cat "$repo/VERSION")
tag="${CEPH_RELEASE}_v${version}"
image="${REGISTRY}/rados-nkv:${tag}"

# Sanity: the prebuilt base must actually be built (libspdk.so + nkv_tgt deps).
[ -e "$SPDK_BASE_DIR/build/lib/libspdk.so" ] || {
  echo "FAIL: prebuilt SPDK base not found at $SPDK_BASE_DIR/build/lib/libspdk.so" >&2
  echo "      build it --with-shared first, or set SPDK_BASE_DIR=<prebuilt spdk>." >&2
  exit 1; }
ls "$DPDK_DIR"/lib/librte_*.so* >/dev/null 2>&1 || {
  echo "FAIL: DPDK shared libs not found under $DPDK_DIR/lib (set DPDK_DIR)" >&2; exit 1; }
[ -e "$MERCURY_DIR/lib/pkgconfig/mercury.pc" ] || {
  echo "FAIL: vendored Mercury not found at $MERCURY_DIR (set MERCURY_DIR)" >&2; exit 1; }

echo "== staging out-of-tree context at $ctx =="
rm -rf "$ctx"
mkdir -p "$ctx/src"

# 1) The out-of-tree repo sources the image builds (target/, rados-nkvx/,
#    scripts/, packaging/, VERSION). The clients/ + spdk submodule + heavy dirs
#    are NOT needed by the datapath image.
# NOTE: excludes are ROOT-ANCHORED (leading /) so e.g. /spdk only drops the
# top-level submodule and NOT target/include/spdk (which holds our kvdev.h ABI).
rsync -a \
  --exclude='/.git' \
  --exclude='/spdk' \
  --exclude='/ceph' \
  --exclude='/clients' \
  --exclude='/build' \
  --exclude='/vm-images' \
  --exclude='/target/nkv_tgt' --exclude='/target/*.o' \
  --exclude='/rados-nkvx/nkvx_service' --exclude='/rados-nkvx/nkvx_exec_client' \
  --exclude='/rados-nkvx/*.o' \
  "$repo"/ "$ctx/src"/

# 2) The prebuilt SPDK base, DPDK, and Mercury (copied so the context is
#    self-contained; rsync dereferences nothing harmful here).
echo "== staging prebuilt SPDK base ($SPDK_BASE_DIR) =="
mkdir -p "$ctx/spdk-base" "$ctx/dpdk" "$ctx/mercury"
rsync -a "$SPDK_BASE_DIR"/ "$ctx/spdk-base"/
rsync -a "$DPDK_DIR"/      "$ctx/dpdk"/
rsync -a "$MERCURY_DIR"/   "$ctx/mercury"/

# A context-root .containerignore so the staged trees are NOT filtered by the
# repo's .dockerignore (which targets the old in-tree layout). Keep only obvious
# junk out; everything staged above is intentional.
cat > "$ctx/.containerignore" <<'EOF'
**/.git
**/__pycache__
src/clients
src/spdk
src/ceph
EOF

echo "== context size: $(du -sh "$ctx" | cut -f1) =="

echo "== building $image (engine=$ENGINE) =="
"$ENGINE" build \
  -f "$ctx/src/packaging/container/Dockerfile" \
  --build-arg "SPDK_BASE_DIR=$SPDK_BASE_DIR" \
  --build-arg "DPDK_DIR=$DPDK_DIR" \
  --build-arg "MERCURY_DIR=$MERCURY_DIR" \
  -t "$image" \
  "$ctx"

# Floating devel tag on the dev line.
if [ "$CEPH_RELEASE" = "devel" ]; then
  "$ENGINE" tag "$image" "${REGISTRY}/rados-nkv:devel"
fi

if [ "$push" = "1" ]; then
  echo "== pushing $image =="
  "$ENGINE" push "$image"
  [ "$CEPH_RELEASE" = "devel" ] && "$ENGINE" push "${REGISTRY}/rados-nkv:devel"
fi

echo "== done: $image =="
"$ENGINE" images "${REGISTRY}/rados-nkv" || true
