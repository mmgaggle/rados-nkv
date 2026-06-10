#!/usr/bin/env bash
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
# Embed build-rocm-xio.sh into user-data (indented 6 spaces under content: |).
workdir="$(mktemp -d)"
trap 'rm -rf "${workdir}"' EXIT
sed 's/^/      /' "${HERE}/build-rocm-xio.sh" > "${workdir}/script.indented"
# Replace the @BUILD_ROCM_XIO@ marker with the indented script.
awk '
  /@BUILD_ROCM_XIO@/ { while ((getline line < script) > 0) print line; next }
  { print }
' script="${workdir}/script.indented" "${HERE}/cloud-init/user-data.in" \
	> "${workdir}/user-data"
cp "${HERE}/cloud-init/meta-data" "${workdir}/meta-data"
cloud-localds "${SEED}" "${workdir}/user-data" "${workdir}/meta-data"

# ---- provisioning boot: cloud-init runs, then powers off -------------------
ACCEL=(); [ -w /dev/kvm ] && ACCEL=(-enable-kvm -cpu host)
CMD=( "${QEMU}"
	"${ACCEL[@]}"
	-m "${VMEM}" -smp "${VCPUS}"
	-drive "file=${DISK},format=qcow2,if=virtio"
	-drive "file=${SEED},format=raw,if=virtio"
	-netdev user,id=net0 -device virtio-net-pci,netdev=net0
	-nographic -serial mon:stdio )

if [ -n "${DRY_RUN:-}" ]; then
	printf '%q ' "${CMD[@]}"; echo; exit 0
fi

echo "build-image: provisioning ${DISK} (installs ROCm + builds rocm-xio; this takes a while)"
"${CMD[@]}"
echo "build-image: done — ${DISK} provisioned. Launch with: make vm-run"
