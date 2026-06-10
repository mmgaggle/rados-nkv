#!/usr/bin/env bash
#
# launch-bridge-vm.sh — the proven QEMU bring-up for the GPU-initiated path.
#
# Vendored from qemu-xio's pci-mmio-guest-code/run-qemu-pci-mmio-bridge (the
# exact invocation that worked), parameterized for this repo: it boots the guest
# with the GPU passed through (vfio-pci), the pci-mmio-bridge device that
# forwards the GPU's doorbell MMIO into guest RAM, and pci-testdev, with the
# proven device parameters preserved verbatim:
#
#   pci-mmio-bridge: shadow-gpa=0x80000000, shadow-size=8192,
#                    poll-interval-ns=1000000
#   nvme:            ioeventfd=off, dbcs=off   (required by the bridge)
#
# This is the emulated-NVMe bridge bring-up. For the full GPU→SPDK→RADOS path,
# attach the SPDK vfio-user NVMe-KV controller instead — see
# provisioning/run-guest.sh (qemu-minimal run-vm with VFIO_USERDEV), and
# docs/flow-a-gpu-initiated.md.
#
# Tunables (env):
#   QEMU_PATH   prefix to qemu binaries   (default <repo>/qemu/build/)
#   IMAGE       guest qcow2               (default <repo>/vm-images/ceph-gpu.qcow2)
#   GPU_BDF     GPU PCI addr for vfio-pci (default 0000:bd:00.0 — Strix Halo 8060S)
#   GPU_BDF2    optional 2nd vfio-pci dev (e.g. GPU audio 0000:bd:00.1; default none)
#   NVME_SIZE_MB  emulated NVMe size      (default 4096)
#   SSH_PORT    host→guest:22 forward     (default 2222)
#   SHARE_DIR   host dir to 9p-share      (default <repo>)
#   DRY_RUN     print the command, do not launch
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/.." && pwd)"

QEMU_PATH="${QEMU_PATH:-${REPO}/qemu/build/}"
QEMU="${QEMU_PATH}qemu-system-x86_64"
IMAGE="${IMAGE:-${REPO}/vm-images/ceph-gpu.qcow2}"
GPU_BDF="${GPU_BDF:-0000:bd:00.0}"
GPU_BDF2="${GPU_BDF2:-none}"
NVME_SIZE_MB="${NVME_SIZE_MB:-4096}"
SSH_PORT="${SSH_PORT:-2222}"
SHARE_DIR="${SHARE_DIR:-${REPO}}"

die() { echo "launch-bridge-vm: $*" >&2; exit 1; }
command -v "${QEMU}" >/dev/null 2>&1 || die "qemu not found: ${QEMU} (build it: make build-qemu, or set QEMU_PATH)"
"${QEMU}" -device help 2>&1 | grep -q pci-mmio-bridge || \
	die "${QEMU} has no pci-mmio-bridge device — wrong QEMU build (need the sbates130272/qemu fork)"
[ -f "${IMAGE}" ] || die "guest image not found: ${IMAGE} (build it: make vm)"

[ -e "/sys/bus/pci/devices/${GPU_BDF}" ] || \
	echo "launch-bridge-vm: WARNING — GPU ${GPU_BDF} not found; is it bound to vfio-pci? (see make vm-vfio-rules)" >&2

# Emulated NVMe backing file (proven bring-up); removed on exit.
NVME_IMG="$(mktemp --suffix=.nvme-img)"
trap 'rm -f "${NVME_IMG}"' EXIT
truncate -s "${NVME_SIZE_MB}M" "${NVME_IMG}"

CMD=( "${QEMU}"
	-machine q35,accel=kvm
	-cpu EPYC
	-m 8G
	-smp 4
	-drive "file=${IMAGE},if=virtio,format=qcow2"
	-netdev "user,id=net0,hostfwd=tcp::${SSH_PORT}-:22"
	-device virtio-net-pci,netdev=net0
	-virtfs "local,path=${SHARE_DIR},mount_tag=hostfs,security_model=none,id=hostfs"
	-device pci-mmio-bridge,id=mmio-bridge,shadow-gpa=0x80000000,shadow-size=8192,poll-interval-ns=1000000,addr=4.0
	-device pci-testdev,membar=1M,membar-backed=on,addr=5.0
	-drive "id=nvme0,file=${NVME_IMG},if=none,format=raw"
	-device nvme,serial=nvme0,drive=nvme0,ioeventfd=off,dbcs=off
	-device "vfio-pci,host=${GPU_BDF},id=vfio0" )

[ "${GPU_BDF2}" != none ] && CMD+=( -device "vfio-pci,host=${GPU_BDF2},id=vfio1" )

CMD+=( -serial mon:stdio -display none
	-trace 'pci_mmio_*'
	-trace 'pci_nvme_admin_*' -trace 'pci_nvme_read_*' -trace 'pci_nvme_write_*'
	-trace 'pci_nvme_mmio_*' -trace pci_nvme_io_cmd -trace pci_nvme_rw
	-trace pci_nvme_enqueue_req -trace pci_nvme_dma_read -trace pci_nvme_dma_write
	-trace pci_nvme_map_addr -trace pci_nvme_map_prp
	-trace 'pci_nvme_vram_p2p_*' )

if [ -n "${DRY_RUN:-}" ]; then
	printf '%q ' "${CMD[@]}"; echo; exit 0
fi

echo "launch-bridge-vm: GPU=${GPU_BDF} bridge shadow-gpa=0x80000000 image=${IMAGE}"
echo "  ssh -p ${SSH_PORT} ubuntu@localhost   ·   Ctrl-A C = monitor, Ctrl-A X = quit"
exec "${CMD[@]}"
