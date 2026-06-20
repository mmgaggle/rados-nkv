#!/usr/bin/env bash
#  SPDX-License-Identifier: LGPL-3.0
#  Copyright (C) 2026 IBM, Inc.
#
# Build the rados-nkv container image with the ceph-nvmeof-style tag scheme
# (ADR bead spdk-jhk.14 §7): our own semver from VERSION, with the Ceph release
# expressed as a TAG PREFIX, never baked into the version number.
#
#   <registry>/rados-nkv:devel_v<semver>            (the dev line; default)
#   <registry>/rados-nkv:tentacle_<point>_v<semver> (pinned to a Ceph release)
#
# Usage:
#   packaging/container/build.sh [--push]
# Env:
#   REGISTRY       image registry/namespace      (default: quay.io/ceph)
#   CEPH_RELEASE   release label for the prefix   (default: devel; e.g. tentacle_9.2)
#   ENGINE         podman | docker               (default: podman)
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)

REGISTRY="${REGISTRY:-quay.io/ceph}"
CEPH_RELEASE="${CEPH_RELEASE:-devel}"
ENGINE="${ENGINE:-podman}"
push=0
[ "${1:-}" = "--push" ] && push=1

version=$(cat "$repo/VERSION")
tag="${CEPH_RELEASE}_v${version}"
image="${REGISTRY}/rados-nkv:${tag}"

# The build context must carry the spdk submodule populated recursively.
if [ ! -e "$repo/spdk/configure" ]; then
	echo "FAIL: spdk submodule not populated — run:" >&2
	echo "  git submodule update --init --recursive spdk" >&2
	exit 1
fi

echo "== building $image (engine=$ENGINE) =="
"$ENGINE" build \
	-f "$here/Dockerfile" \
	-t "$image" \
	"$repo"

# Also move a floating devel tag on the dev line so consumers can track it.
if [ "$CEPH_RELEASE" = "devel" ]; then
	"$ENGINE" tag "$image" "${REGISTRY}/rados-nkv:devel"
fi

if [ "$push" -eq 1 ]; then
	echo "== pushing $image =="
	"$ENGINE" push "$image"
	[ "$CEPH_RELEASE" = "devel" ] && "$ENGINE" push "${REGISTRY}/rados-nkv:devel"
fi

echo "== done: $image =="
