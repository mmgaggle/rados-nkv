#!/usr/bin/env bash
#  Copyright (C) 2026 IBM, Inc.
#
#  This is free software; you can redistribute it and/or modify it under the
#  terms of the GNU Lesser General Public License version 3, as published by
#  the Free Software Foundation.  See file COPYING.
#
#
# build-rocm-xio.sh — in-guest provisioning, run once by cloud-init runcmd.
#
# Installs the ROCm stack and builds rocm-xio's xio-tester (the GPU-initiated
# NVMe-KV `nvme-ep --kv-op` path) inside the guest. It is embedded verbatim into
# the cloud-init user-data by provisioning/build-image.sh (write_files +
# runcmd), so edit it here — it is the single source of truth — and rebuild the
# image with `make vm`.
#
# Idempotent: re-running skips the ROCm install and rebuilds rocm-xio in place.
#
# Tunables (exported into the cloud-init env by build-image.sh):
#   ROCM_VERSION       ROCm release to install                 (default 6.4.1)
#   UBUNTU_CODENAME    apt suite for the AMD repos             (default noble)
#   ROCM_XIO_REPO      rocm-xio git remote                     (default mmgaggle fork)
#   ROCM_XIO_BRANCH    rocm-xio branch (matches the submodule) (default nvme-kv)
#   ROCM_XIO_PRESET    CMake preset                            (default release)
#   GFX_OVERRIDE       HSA_OVERRIDE_GFX_VERSION (only if set)  (default: unset)
#
# ROCm: we use AMD's amdgpu-install .deb only to wire up the repos, then apt
# install the MINIMAL HIP build stack. rocm-xio needs only find_package(hip) +
# hsa-runtime64 — NOT the ROCm math libraries (rocblas/rocfft/composablekernel,
# ~10 GB) that the full hiplibsdk usecase pulls. rocm-hip-runtime-dev gives the
# HIP compiler + runtime + headers + hsa; no kernel driver (the build VM has no
# GPU — the amdgpu/kfd driver is a runtime concern at passthrough time). gfx1151
# (Strix Halo) is native in ROCm 7.x, so no HSA_OVERRIDE unless you ask for it.
set -euxo pipefail

ROCM_VERSION="${ROCM_VERSION:-7.2.4}"
UBUNTU_CODENAME="${UBUNTU_CODENAME:-noble}"
ROCM_XIO_REPO="${ROCM_XIO_REPO:-https://github.com/mmgaggle/rocm-xio.git}"
ROCM_XIO_BRANCH="${ROCM_XIO_BRANCH:-nvme-kv}"
ROCM_XIO_PRESET="${ROCM_XIO_PRESET:-release}"
GFX_OVERRIDE="${GFX_OVERRIDE:-}"
ROCM_XIO_SRC="${ROCM_XIO_SRC:-/opt/rocm-xio}"

export DEBIAN_FRONTEND=noninteractive

# ---- 1. ROCm stack via amdgpu-install (skip if already present) ------------
if [ ! -d /opt/rocm ]; then
	base="https://repo.radeon.com/amdgpu-install/${ROCM_VERSION}/ubuntu/${UBUNTU_CODENAME}"
	# Discover the exact installer .deb (its filename carries a build number).
	deb="$(curl -fsSL "${base}/" \
		| grep -oE 'amdgpu-install_[0-9A-Za-z._~-]+_all\.deb' | sort | tail -1)"
	[ -n "${deb}" ] || { echo "no amdgpu-install deb for ROCm ${ROCM_VERSION}/${UBUNTU_CODENAME}" >&2; exit 1; }
	curl -fsSL -o "/tmp/${deb}" "${base}/${deb}"

	apt-get update
	apt-get install -y "/tmp/${deb}"   # configures the repo.radeon.com apt repos
	apt-get update
	# Minimal HIP build stack (NOT hiplibsdk): rocm-hip-runtime-dev = HIP compiler
	# (rocm-llvm) + runtime + headers + hsa-rocr; rocm-cmake = the CMake modules.
	# --no-install-recommends keeps the ROCm math libraries out. libdrm-dev +
	# libcli11-dev are rocm-xio's other build deps (per its INSTALL.md).
	apt-get install -y --no-install-recommends \
		rocm-hip-runtime-dev rocm-cmake rocminfo libdrm-dev libcli11-dev
fi

# Make the ROCm toolchain discoverable for this script and future logins.
export PATH="/opt/rocm/bin:${PATH}"
echo '/opt/rocm/lib'     > /etc/ld.so.conf.d/rocm.conf
echo '/opt/rocm/lib64'  >> /etc/ld.so.conf.d/rocm.conf
ldconfig
echo 'export PATH=/opt/rocm/bin:$PATH' > /etc/profile.d/rocm.sh
[ -n "${GFX_OVERRIDE}" ] && echo "export HSA_OVERRIDE_GFX_VERSION=${GFX_OVERRIDE}" >> /etc/profile.d/rocm.sh

# ---- 2. rocm-xio (clone the pinned fork/branch, build xio-tester) ----------
if [ ! -d "${ROCM_XIO_SRC}/.git" ]; then
	git clone --branch "${ROCM_XIO_BRANCH}" --depth 1 \
		"${ROCM_XIO_REPO}" "${ROCM_XIO_SRC}"
fi
cd "${ROCM_XIO_SRC}"
git fetch --depth 1 origin "${ROCM_XIO_BRANCH}" && git checkout "${ROCM_XIO_BRANCH}"

cmake --preset "${ROCM_XIO_PRESET}"
cmake --build build --parallel "$(nproc)"

# Surface the tester on PATH (built name per rocm-xio's CMake output).
if [ -x build/xio-tester ]; then
	ln -sf "${ROCM_XIO_SRC}/build/xio-tester" /usr/local/bin/xio-tester
fi

echo "rocm-xio build complete: $(ls -1 ${ROCM_XIO_SRC}/build/xio-tester 2>/dev/null || echo 'xio-tester not found — check the build log')"
