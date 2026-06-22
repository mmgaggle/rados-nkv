#!/usr/bin/env bash
# Stage a context and build the rados-nkv-client image (the `rkv` Rust CLI).
#
# This is the client counterpart to build-clean.sh. It does NOT rebuild SPDK:
# it reuses the prebuilt SPDK inside the builder image
# (localhost/rados-nkv-builder:<tag>, produced by build-clean.sh). If that
# builder image is missing, run build-clean.sh first.
#
# A dedicated context dir is staged (rather than editing the shared
# .dockerignore) so the base nkv/nkvx image stays lean: the base build keeps
# excluding clients/rkv, while this build's context explicitly includes it.
#
# Env knobs (all optional):
#   REGISTRY   default quay.io/mmgaggle
#   ENGINE     default podman   (rootless build)
#   PUSH       default 0        (1 to push)
#   WITH_GPU   default 0        (1 builds the gpu-native/HIP path)
#   CEPH_RELEASE default devel  (tag prefix + floating :devel)
#   BUILDER    default localhost/rados-nkv-builder:<tag>
#   CTX        default /tmp/nkv-client-ctx
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ctx=${CTX:-/tmp/nkv-client-ctx}
REGISTRY="${REGISTRY:-quay.io/mmgaggle}"
CEPH_RELEASE="${CEPH_RELEASE:-devel}"
ENGINE="${ENGINE:-podman}"
push="${PUSH:-0}"
with_gpu="${WITH_GPU:-0}"

version=$(cat "$repo/VERSION")
tag="${CEPH_RELEASE}_v${version}"
image_name="rados-nkv-client"
image="${REGISTRY}/${image_name}:${tag}"
builder="${BUILDER:-localhost/rados-nkv-builder:${tag}}"

# The build REQUIRES the prebuilt SPDK in the builder image. Fail early and
# clearly if it is absent — we deliberately do NOT recompile SPDK here.
if ! "$ENGINE" image exists "$builder"; then
  cat >&2 <<EOF
ERROR: builder image '$builder' not found.

The client image reuses the prebuilt SPDK from that image; it does not rebuild
SPDK. Build it first:

  PUSH=0 packaging/container/build-clean.sh

(or set BUILDER=<your builder image:tag>), then re-run this script.
EOF
  exit 1
fi

echo "== staging client context at $ctx =="
rm -rf "$ctx"
mkdir -p "$ctx/clients" "$ctx/packaging/container"

# rkv crate (source of the build) — the binary is compiled inside the image, so
# drop any host-side build output to keep the context small.
rsync -a --exclude='target/' "$repo/clients/rkv"/ "$ctx/clients/rkv"/

# vfu_host header that rkv's shim #includes (clients/nvme-kv/kv/vfu_host/
# nkv_vfu.h). The header is also present in the builder image's /src, but stage
# it so the context is self-describing.
rsync -a "$repo/clients/nvme-kv"/ "$ctx/clients/nvme-kv"/

# Packaging (Dockerfile.rkv + entrypoint) and the version file.
rsync -a "$repo/packaging/container/Dockerfile.rkv" "$ctx/packaging/container/"
rsync -a "$repo/packaging/container/rkv-entrypoint.sh" "$ctx/packaging/container/"
cp "$repo/VERSION" "$ctx/VERSION"

echo "== context size: $(du -sh "$ctx" | cut -f1) =="

echo "== building $image (engine=$ENGINE, builder=$builder, WITH_GPU=$with_gpu) =="
"$ENGINE" build \
  -f "$ctx/packaging/container/Dockerfile.rkv" \
  --build-arg "BUILDER=$builder" \
  --build-arg "WITH_GPU=$with_gpu" \
  -t "$image" \
  "$ctx"

# Floating devel tag on the dev line.
if [ "$CEPH_RELEASE" = "devel" ]; then
  "$ENGINE" tag "$image" "${REGISTRY}/${image_name}:devel"
fi

if [ "$push" = "1" ]; then
  echo "== pushing $image =="
  "$ENGINE" push "$image"
  [ "$CEPH_RELEASE" = "devel" ] && "$ENGINE" push "${REGISTRY}/${image_name}:devel"
fi

echo "== done: $image =="
"$ENGINE" images "${REGISTRY}/${image_name}" || true
