#!/usr/bin/env bash
# Stage a PRISTINE build context and build+push the rados-nkv image.
#
# Why this exists: packaging/container/Dockerfile (COPY . /src) assumes a freshly
# `git submodule update --init --recursive` spdk tree with NO build artifacts.
# A developer's in-place-built spdk leaks host build state into the context —
# cmake/meson caches bake absolute /home paths and abort under /src, and partial
# artifact pruning leaves autotools trees (isa-l) half-built. This script copies
# the working tree for everything EXCEPT spdk, and stages spdk as a clean `git
# clone` (with a working .git, which SPDK's own rpm.sh/spec require) whose
# submodules are sourced from the host's local objects. Non-destructive.
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ctx=${CTX:-/tmp/nkv-ctx}
REGISTRY="${REGISTRY:-quay.io/mmgaggle}"
CEPH_RELEASE="${CEPH_RELEASE:-devel}"
ENGINE="${ENGINE:-podman}"
push="${PUSH:-0}"

version=$(cat "$repo/VERSION")
tag="${CEPH_RELEASE}_v${version}"
image="${REGISTRY}/rados-nkv:${tag}"

echo "== staging pristine context at $ctx =="
rm -rf "$ctx"
mkdir -p "$ctx"

# Working-tree copy of everything except spdk (exported clean below) and the
# heavy / generated dirs the image never uses.
rsync -a \
  --exclude='.git' \
  --exclude='spdk' \
  --exclude='vm-images' \
  --exclude='build' \
  --exclude='clients/vllm-weights' \
  --exclude='clients/rkv' \
  --exclude='clients/nixl' \
  --exclude='clients/rocm-xio' \
  "$repo"/ "$ctx"/

# Pristine spdk WITH a working .git: SPDK's own rpm.sh/spec run
# `git submodule update --init`, which is fatal without one. We keep a persistent
# clean clone and source its submodules from the host's already-fetched objects —
# fully offline, and preserving the mmgaggle/libvfio-user fork commit (its
# .gitmodules URL is SSH, which an unattended network init could not reach).
SPDK_CLEAN="${SPDK_CLEAN:-/tmp/nkv-spdk-clean}"
spdk_head=$(git -C "$repo/spdk" rev-parse HEAD)
if [ "$(git -C "$SPDK_CLEAN" rev-parse HEAD 2>/dev/null)" != "$spdk_head" ]; then
  echo "== preparing pristine spdk clone at $SPDK_CLEAN (HEAD $spdk_head) =="
  rm -rf "$SPDK_CLEAN"
  git clone --no-hardlinks "$repo/spdk" "$SPDK_CLEAN"
  git -C "$SPDK_CLEAN" checkout --detach "$spdk_head"
  git -C "$SPDK_CLEAN" submodule init
  # Redirect each submodule URL to the host's local submodule repo (offline).
  git -C "$SPDK_CLEAN" config -f "$SPDK_CLEAN/.gitmodules" --get-regexp '\.path$' \
    | while read -r key path; do
        name=${key#submodule.}; name=${name%.path}
        git -C "$SPDK_CLEAN" config "submodule.${name}.url" "$repo/spdk/$path"
      done
  git -C "$SPDK_CLEAN" -c protocol.file.allow=always submodule update
fi
echo "== syncing pristine spdk into context =="
rsync -a "$SPDK_CLEAN"/ "$ctx/spdk"/

echo "== context size: $(du -sh "$ctx" | cut -f1) =="

echo "== building $image (engine=$ENGINE) =="
"$ENGINE" build -f "$ctx/packaging/container/Dockerfile" -t "$image" "$ctx"

# Floating devel tag on the dev line.
if [ "$CEPH_RELEASE" = "devel" ]; then
  "$ENGINE" tag "$image" "${REGISTRY}/rados-nkv:devel"
fi

# Extract the RPMs built inside the builder stage to a host dir for convenience.
# The runtime stage deletes /tmp/rpms, so grab them from the builder target
# (cached from the build above — no recompile).
echo "== extracting RPMs from builder stage =="
rpmout="$repo/build/rpms"
mkdir -p "$rpmout"
"$ENGINE" build --target builder -f "$ctx/packaging/container/Dockerfile" \
  -t rados-nkv-builder:"$tag" "$ctx"
bcid=$("$ENGINE" create rados-nkv-builder:"$tag")
"$ENGINE" cp "$bcid":/rpms/. "$rpmout"/ || true
"$ENGINE" rm "$bcid" >/dev/null 2>&1 || true
echo "== RPMs at $rpmout =="
ls -1 "$rpmout" || true

if [ "$push" = "1" ]; then
  echo "== pushing $image =="
  "$ENGINE" push "$image"
  [ "$CEPH_RELEASE" = "devel" ] && "$ENGINE" push "${REGISTRY}/rados-nkv:devel"
fi

echo "== done: $image =="
