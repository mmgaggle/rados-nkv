# provisioning — the Flow A guest VM

Build and launch the guest VM for the **GPU-initiated** path: ROCm + rocm-xio
inside the guest, the GPU passed through, and the `pci-mmio-bridge` forwarding
the GPU's doorbell MMIO to an NVMe controller (emulated, or the SPDK vfio-user
NVMe-KV target for the full RADOS path).

Built on the [`qemu-minimal`](../qemu-minimal) submodule (cloud images, `run-vm`,
VFIO udev rules).

## Files

| File | Role |
|------|------|
| `cloud-init/user-data.in` | cloud-config template: packages + the rocm-xio build script (`@BUILD_ROCM_XIO@` is filled in at seed-build time) + `power_state: poweroff`. |
| `cloud-init/meta-data` | NoCloud instance/hostname. |
| `build-rocm-xio.sh` | **In-guest, cloud-init `runcmd`.** Installs the ROCm stack and clones+builds `mmgaggle/rocm-xio@nvme-kv` (`xio-tester`). Single source of truth; embedded into the seed. |
| `build-image.sh` | **Host.** Downloads the Ubuntu cloud image, assembles the NoCloud seed, boots once so cloud-init provisions, and powers off — leaving `vm-images/ceph-gpu.qcow2`. |
| `launch-bridge-vm.sh` | **Host.** The proven bring-up, vendored from `qemu-xio`'s `run-qemu-pci-mmio-bridge`: GPU `vfio-pci` + `pci-mmio-bridge` (`shadow-gpa=0x80000000,shadow-size=8192,poll-interval-ns=1000000`) + emulated NVMe (`ioeventfd=off,dbcs=off`). |
| `run-guest.sh` | **Host.** The SPDK-attached variant: wraps `qemu-minimal`'s `run-vm`, passing `VFIO_USERDEV` (the SPDK NVMe-KV socket), `PCI_HOSTDEV`, and `PCI_MMIO_BRIDGE` through. |

## Make targets

```bash
make vm              # build-image.sh  -> vm-images/ceph-gpu.qcow2
make vm-run          # launch-bridge-vm.sh (proven; emulated NVMe + bridge)
make vm-run-spdk     # run-guest.sh (run-vm wired to the SPDK NVMe-KV target)
make vm-vfio-rules   # qemu-minimal/udev/install-vfio-rules
```

All scripts read env overrides — see each script's header. Common ones: `QEMU_PATH`
(defaults to this repo's built `qemu/`), `IMAGE`/`IMAGES`, `GPU_BDF`,
`ROCM_VERSION`, `VFIO_USERDEV`, `PCI_HOSTDEV`.

## Full Flow A run

```bash
make vm                                   # 1. build the guest image (once)
make vm-vfio-rules                         # 2. VFIO permissions (once, sudo)
# bind the GPU to vfio-pci (host), then:
scripts/rados-nkv up                       # 3. SPDK NVMe-KV target -> /var/run/muser/domain/kv/0
PCI_HOSTDEV=0000:bd:00.0 \
VFIO_USERDEV=/var/run/muser/domain/kv/0 \
PCI_MMIO_BRIDGE=on  make vm-run-spdk        # 4. launch, GPU passed through, NVMe-KV attached
# in the guest: xio-tester nvme-ep --controller /dev/nvme0 --kv-op store ... --pci-mmio-bridge
```

See [`../docs/flow-a-gpu-initiated.md`](../docs/flow-a-gpu-initiated.md) for the
GPU-side commands and the on-the-wire KV encoding.

> **ROCm specifics:** `build-rocm-xio.sh` defaults to ROCm 6.4.1 on Ubuntu noble
> with `HSA_OVERRIDE_GFX_VERSION=11.5.1` (gfx1151 / Strix Halo 8060S). Override
> `ROCM_VERSION` / `GFX_OVERRIDE` for a different target. The dev host itself runs
> Fedora ROCm 7.1.1 — the guest is provisioned independently, so its ROCm version
> is its own knob.
