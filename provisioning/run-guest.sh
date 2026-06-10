#!/usr/bin/env bash
#
# run-guest.sh — launch the provisioned guest via qemu-minimal's run-vm, wired
# for the GPU-initiated NVMe-KV demo (Flow A).
#
# Thin wrapper that points run-vm at our built QEMU (the pci-mmio-bridge fork)
# and our image, then passes the passthrough / vfio-user / mmio-bridge knobs
# straight through from the environment. Everything below is overridable:
#
#   VM_NAME          guest/image name                 (default ceph-gpu)
#   IMAGES           image directory                  (default <repo>/vm-images)
#   QEMU_PATH        qemu binary prefix               (default <repo>/qemu/build/)
#
# Pass through to run-vm (set these to wire the demo):
#   PCI_HOSTDEV      GPU PCI address(es) for VFIO passthrough, e.g. 0000:c5:00.0
#   VFIO_USERDEV     SPDK vfio-user socket(s), e.g. /var/run/muser/domain/kv/0
#   PCI_MMIO_BRIDGE  set non-"none" to attach the pci-mmio-bridge device
#   FILESYSTEM       host dir to 9p-share into the guest
#   SSH_PORT, VMEM, VCPUS, DRY_RUN, ... (see qemu-minimal run-vm)
#
# Example (full Flow A path):
#   scripts/rados-nkv up        # creates /var/run/muser/domain/kv/0
#   PCI_HOSTDEV=0000:c5:00.0 \
#   VFIO_USERDEV=/var/run/muser/domain/kv/0 \
#   PCI_MMIO_BRIDGE=on  make vm-run
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/.." && pwd)"

RUN_VM="${REPO}/qemu-minimal/qemu/run-vm"
[ -x "${RUN_VM}" ] || { echo "run-guest: ${RUN_VM} not found — run 'make init-qemu-minimal'" >&2; exit 1; }

export VM_NAME="${VM_NAME:-ceph-gpu}"
export IMAGES="${IMAGES:-${REPO}/vm-images}"
export QEMU_PATH="${QEMU_PATH:-${REPO}/qemu/build/}"

[ -f "${IMAGES}/${VM_NAME}.qcow2" ] || \
	echo "run-guest: warning — ${IMAGES}/${VM_NAME}.qcow2 not found; run 'make vm' first" >&2

cd "$(dirname "${RUN_VM}")"
exec "${RUN_VM}"
