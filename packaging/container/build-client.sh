#!/usr/bin/env bash
# Stage a context and build the rados-nkv-client image (the `rkv` Rust CLI),
# OUT-OF-TREE against the pre-built SPDK base (Slice I, spdk-7sr.17.10).
#
# Unlike the former build (which reused the prebuilt SPDK inside a separate
# builder image), this is SELF-CONTAINED: it stages the SPDK base tree + the
# STATIC DPDK archives the rkv build.rs links, plus the rkv crate and the
# vfu_host header it #includes, then the Dockerfile compiles rkv in-image. rkv
# links SPDK statically, so the runtime image carries no SPDK shared libs.
#
# Env knobs (all optional):
#   REGISTRY      default quay.io/mmgaggle
#   ENGINE        default podman
#   PUSH          default 0
#   WITH_GPU      default 0        (1 builds the gpu-native/HIP path)
#   CEPH_RELEASE  default devel    (tag prefix + floating :devel)
#   CTX           default /tmp/nkv-client-ctx
#   SPDK_BASE_DIR pre-built SPDK X tree      (default below)
#   DPDK_DIR      DPDK build with STATIC librte_*.a (default below)
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ctx=${CTX:-/tmp/nkv-client-ctx}
REGISTRY="${REGISTRY:-quay.io/mmgaggle}"
CEPH_RELEASE="${CEPH_RELEASE:-devel}"
ENGINE="${ENGINE:-podman}"
push="${PUSH:-0}"
with_gpu="${WITH_GPU:-0}"
gpu_arch="${GPU_ARCH:-gfx1151}"

SPDK_BASE_DIR="${SPDK_BASE_DIR:-/home/kyle/src/rados-nkv-wt/slice-a/spdk}"
# The SPDK base was built --with-shared against an EXTERNAL DPDK; its own
# spdk/dpdk/build/lib has no static archives. The rkv build.rs links the STATIC
# DPDK (librte_*.a). Point DPDK_DIR at a build that has them.
DPDK_DIR="${DPDK_DIR:-/home/kyle/src/rados-nkv/spdk/dpdk/build}"

version=$(cat "$repo/VERSION")
base_tag="${CEPH_RELEASE}_v${version}"
suffix=""; [ "$with_gpu" = "1" ] && suffix="-gpu"
tag="${base_tag}${suffix}"
image_name="rados-nkv-client"
image="${REGISTRY}/${image_name}:${tag}"

[ -e "$SPDK_BASE_DIR/build/lib/libspdk_vfio_user.a" ] || {
  echo "FAIL: static SPDK libs not found under $SPDK_BASE_DIR/build/lib (set SPDK_BASE_DIR)" >&2; exit 1; }
ls "$DPDK_DIR"/lib/librte_*.a >/dev/null 2>&1 || {
  echo "FAIL: static DPDK archives (librte_*.a) not found under $DPDK_DIR/lib (set DPDK_DIR)" >&2; exit 1; }

echo "== staging client context at $ctx =="
rm -rf "$ctx"
mkdir -p "$ctx/clients" "$ctx/packaging/container"

# rkv crate (drop host-side build output) + the nvme-kv vfu_host header it
# #includes (clients/nvme-kv/kv/vfu_host/nkv_vfu.h).
rsync -a --exclude='target/' "$repo/clients/rkv"/ "$ctx/clients/rkv"/
rsync -a "$repo/clients/nvme-kv"/ "$ctx/clients/nvme-kv"/

# SPDK base tree at the SAME absolute path inside the staged spdk-base/, then
# overlay the STATIC DPDK archives into its dpdk/build/lib so build.rs (which
# reads <spdk>/dpdk/build/lib/librte_*.a) finds them. The base's shared DPDK
# build dir is empty, so this populates rather than conflicts.
echo "== staging SPDK base + static DPDK =="
rsync -a "$SPDK_BASE_DIR"/ "$ctx/spdk-base"/
mkdir -p "$ctx/spdk-base/dpdk/build/lib" "$ctx/spdk-base/dpdk/build/include"
cp "$DPDK_DIR"/lib/librte_*.a "$ctx/spdk-base/dpdk/build/lib/"
cp -r "$DPDK_DIR"/include/. "$ctx/spdk-base/dpdk/build/include/" 2>/dev/null || true

# Packaging (Dockerfile.rkv + entrypoint) and the version file.
rsync -a "$repo/packaging/container/Dockerfile.rkv" "$ctx/packaging/container/"
rsync -a "$repo/packaging/container/rkv-entrypoint.sh" "$ctx/packaging/container/"
cp "$repo/VERSION" "$ctx/VERSION"

# Context-root ignore so nothing junky enters; the staged trees are intentional.
printf '%s\n' '**/.git' '**/__pycache__' 'clients/rkv/target' > "$ctx/.containerignore"

echo "== context size: $(du -sh "$ctx" | cut -f1) =="

echo "== building $image (engine=$ENGINE, WITH_GPU=$with_gpu) =="
"$ENGINE" build \
  -f "$ctx/packaging/container/Dockerfile.rkv" \
  --build-arg "WITH_GPU=$with_gpu" \
  --build-arg "GPU_ARCH=$gpu_arch" \
  --build-arg "SPDK_BASE_DIR=$SPDK_BASE_DIR" \
  -t "$image" \
  "$ctx"

# Floating devel tag on the dev line (CPU: :devel, GPU: :devel-gpu).
if [ "$CEPH_RELEASE" = "devel" ]; then
  "$ENGINE" tag "$image" "${REGISTRY}/${image_name}:devel${suffix}"
fi

if [ "$push" = "1" ]; then
  echo "== pushing $image =="
  "$ENGINE" push "$image"
  [ "$CEPH_RELEASE" = "devel" ] && "$ENGINE" push "${REGISTRY}/${image_name}:devel${suffix}"
fi

echo "== done: $image =="
"$ENGINE" images "${REGISTRY}/${image_name}" || true
