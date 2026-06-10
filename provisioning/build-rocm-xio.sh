#!/usr/bin/env bash
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
#   GFX_OVERRIDE       HSA_OVERRIDE_GFX_VERSION for the target (default 11.5.1 / gfx1151)
#
# NOTE: Strix Halo (gfx1151) needs a recent ROCm; if your target GPU or ROCm
# release differs, override ROCM_VERSION / GFX_OVERRIDE. Building rocm-xio needs
# the ROCm/HIP toolchain present but not a GPU, so this runs fine during the
# headless provisioning boot.
set -euxo pipefail

ROCM_VERSION="${ROCM_VERSION:-6.4.1}"
UBUNTU_CODENAME="${UBUNTU_CODENAME:-noble}"
ROCM_XIO_REPO="${ROCM_XIO_REPO:-https://github.com/mmgaggle/rocm-xio.git}"
ROCM_XIO_BRANCH="${ROCM_XIO_BRANCH:-nvme-kv}"
ROCM_XIO_PRESET="${ROCM_XIO_PRESET:-release}"
GFX_OVERRIDE="${GFX_OVERRIDE:-11.5.1}"
ROCM_XIO_SRC="${ROCM_XIO_SRC:-/opt/rocm-xio}"

export DEBIAN_FRONTEND=noninteractive

# ---- 1. ROCm stack (skip if already present) -------------------------------
if [ ! -d /opt/rocm ]; then
	mkdir -p /etc/apt/keyrings
	curl -fsSL https://repo.radeon.com/rocm/rocm.gpg.key \
		| gpg --dearmor -o /etc/apt/keyrings/rocm.gpg

	echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/rocm.gpg] \
https://repo.radeon.com/amdgpu/${ROCM_VERSION}/ubuntu ${UBUNTU_CODENAME} main" \
		> /etc/apt/sources.list.d/amdgpu.list
	echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/rocm.gpg] \
https://repo.radeon.com/rocm/apt/${ROCM_VERSION} ${UBUNTU_CODENAME} main" \
		> /etc/apt/sources.list.d/rocm.list
	printf 'Package: *\nPin: release o=repo.radeon.com\nPin-Priority: 600\n' \
		> /etc/apt/preferences.d/rocm-pin-600

	apt-get update
	# rocm-hip-sdk pulls the HIP compiler + runtime needed to build rocm-xio.
	apt-get install -y rocm-hip-sdk rocm-smi-lib
fi

# Make the ROCm toolchain discoverable for this script and future logins.
export PATH="/opt/rocm/bin:${PATH}"
echo '/opt/rocm/lib'     > /etc/ld.so.conf.d/rocm.conf
echo '/opt/rocm/lib64'  >> /etc/ld.so.conf.d/rocm.conf
ldconfig
cat > /etc/profile.d/rocm.sh <<EOF
export PATH=/opt/rocm/bin:\$PATH
export HSA_OVERRIDE_GFX_VERSION=${GFX_OVERRIDE}
EOF

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
