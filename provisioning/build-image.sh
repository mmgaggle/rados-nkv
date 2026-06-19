#!/usr/bin/env bash
#  Copyright (C) 2026 IBM, Inc.
#
#  This is free software; you can redistribute it and/or modify it under the
#  terms of the GNU Lesser General Public License version 3, as published by
#  the Free Software Foundation.  See file COPYING.
#
#
# build-image.sh — build the ROCm + rocm-xio guest disk image (cloud-init).
#
# Downloads an Ubuntu cloud image, assembles a NoCloud seed from
# cloud-init/user-data.in (embedding build-rocm-xio.sh), and boots the image
# once headless so cloud-init installs ROCm and builds rocm-xio's xio-tester.
# The VM powers itself off when done (power_state), leaving a provisioned
# ${VM_NAME}.qcow2 under ${IMAGES} that `make vm-run` launches with passthrough
# and the SPDK vfio-user NVMe-KV controller.
#
# Tunables (env):
#   VM_NAME   guest / image name            (default ceph-gpu)
#   IMAGES    image + seed directory        (default <repo>/vm-images)
#   RELEASE   Ubuntu release codename       (default noble)
#   SIZE      provisioned disk size         (default 64G)
#   VMEM      provisioning-boot RAM (MB)    (default 8192)
#   VCPUS     provisioning-boot vCPUs       (default 4)
#   QEMU_PATH prefix to qemu binaries       (default: system qemu-system-x86_64)
#   ROCM_VERSION / GFX_OVERRIDE / ROCM_XIO_* — forwarded to build-rocm-xio.sh
#   DRY_RUN   print the provisioning-boot QEMU command and exit (default unset)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/.." && pwd)"

VM_NAME="${VM_NAME:-ceph-gpu}"
IMAGES="${IMAGES:-${REPO}/vm-images}"
RELEASE="${RELEASE:-noble}"
SIZE="${SIZE:-64G}"
VMEM="${VMEM:-8192}"
VCPUS="${VCPUS:-4}"
QEMU_PATH="${QEMU_PATH:-}"
QEMU="${QEMU_PATH}qemu-system-x86_64"

die() { echo "build-image: $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing tool: $1 ($2)"; }

need qemu-img "install qemu-utils"
need cloud-localds "install cloud-image-utils"
need curl "install curl"
command -v "${QEMU}" >/dev/null 2>&1 || die "qemu not found: ${QEMU} (set QEMU_PATH)"

mkdir -p "${IMAGES}"
CLOUDIMG="${IMAGES}/${RELEASE}-server-cloudimg-amd64.img"
DISK="${IMAGES}/${VM_NAME}.qcow2"
SEED="${IMAGES}/${VM_NAME}-seed.img"
CONSOLE_LOG="${IMAGES}/${VM_NAME}-console.log"

if [ -n "${DRY_RUN:-}" ]; then
	echo "DRY_RUN: would download ${CLOUDIMG} (if missing), build the cloud-init seed"
	echo "         from ${HERE}/cloud-init, create ${DISK} (${SIZE}), and boot once"
	echo "         headless to provision (console -> ${CONSOLE_LOG})."
	exit 0
fi

# ---- cloud image -----------------------------------------------------------
if [ ! -f "${CLOUDIMG}" ]; then
	url="https://cloud-images.ubuntu.com/${RELEASE}/current/${RELEASE}-server-cloudimg-amd64.img"
	echo "build-image: downloading ${url}"
	curl -fL -o "${CLOUDIMG}.tmp" "${url}"
	mv "${CLOUDIMG}.tmp" "${CLOUDIMG}"
fi

# ---- per-VM overlay (thin, backed by the cloud image), resized -------------
qemu-img create -f qcow2 -F qcow2 -b "${CLOUDIMG}" "${DISK}" >/dev/null
qemu-img resize "${DISK}" "${SIZE}" >/dev/null

# ---- assemble the cloud-init seed ------------------------------------------
workdir="$(mktemp -d)"
trap 'rm -rf "${workdir}"' EXIT
# Embed build-rocm-xio.sh as base64 (the write_files entry sets encoding: b64).
# This is robust against the YAML whitespace/tab pitfalls of inlining a shell
# script into a block scalar. base64 alphabet has no sed-special chars and no
# '|', so a '|'-delimited substitution is safe.
b64="$(base64 -w0 "${HERE}/build-rocm-xio.sh")"
sed "s|@BUILD_ROCM_XIO_B64@|${b64}|" "${HERE}/cloud-init/user-data.in" \
	> "${workdir}/user-data"

# Guard: a malformed user-data makes cloud-init silently apply an empty config
# (no packages/runcmd/poweroff), so validate YAML up front when we can.
if command -v python3 >/dev/null && python3 -c 'import yaml' 2>/dev/null; then
	python3 -c 'import yaml,sys; yaml.safe_load(open(sys.argv[1]))' "${workdir}/user-data" \
		|| { echo "build-image: generated user-data is not valid YAML" >&2; exit 1; }
fi

cp "${HERE}/cloud-init/meta-data" "${workdir}/meta-data"
cloud-localds "${SEED}" "${workdir}/user-data" "${workdir}/meta-data"

# ---- provisioning boot: cloud-init runs build-rocm-xio.sh, then powers off --
# Headless and non-interactive: serial to a log file, no stdio monitor (so a
# closed stdin can't kill it), -no-reboot so a guest reboot exits QEMU, and a
# timeout backstop in case cloud-init wedges. The guest powers off when done
# (user-data power_state), and QEMU exits with it.
ACCEL=(); [ -w /dev/kvm ] && ACCEL=(-enable-kvm -cpu host)
CMD=( "${QEMU}"
	"${ACCEL[@]}"
	-m "${VMEM}" -smp "${VCPUS}"
	-drive "file=${DISK},format=qcow2,if=virtio"
	-drive "file=${SEED},format=raw,if=virtio"
	-netdev user,id=net0 -device virtio-net-pci,netdev=net0
	-display none -serial "file:${CONSOLE_LOG}" -monitor none -no-reboot )

echo "build-image: provisioning ${DISK} (installs ROCm + builds rocm-xio; can take 20-40 min)"
echo "build-image: guest console -> ${CONSOLE_LOG}"
if timeout "${BOOT_TIMEOUT:-3600}" "${CMD[@]}" </dev/null; then
	echo "build-image: done — ${DISK} provisioned. Launch with: make vm-run"
else
	rc=$?
	if [ "${rc}" = 124 ]; then
		echo "build-image: TIMED OUT after ${BOOT_TIMEOUT:-3600}s — see ${CONSOLE_LOG}" >&2
	else
		echo "build-image: provisioning boot exited rc=${rc} — see ${CONSOLE_LOG}" >&2
	fi
	exit "${rc}"
fi
