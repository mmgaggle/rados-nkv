# Flow A — GPU-initiated NVMe Key-Value to Ceph

The hero path: an AMD GPU (gfx1151) issues NVMe **KV Store** then **Retrieve**
from `__device__` code, with the value landing directly in GPU memory and
persisted as a RADOS object — no host CPU in the data loop.

![GPU NVMe passthrough](diagrams/gpu-nvme-passthrough.png)

(Source: [`diagrams/gpu-nvme-passthrough.mmd`](diagrams/gpu-nvme-passthrough.mmd))

## The path, end to end

1. **GPU `__device__` code** (`rocm-xio` `nvme-ep`, `driveEndpointKv()`):
   builds the KV SQE with `kvSqeSetup()`, rings the SQ doorbell, polls the CQ,
   and rings the CQ-head doorbell. With `--memory-mode 8` the value buffer is
   VRAM/P2PDMA, so a Retrieve lands the value straight in GPU memory.
2. **Doorbell MMIO → NVMe BAR** (`qemu` `pci-mmio-bridge`): in the
   passed-through-GPU guest, the GPU writes command descriptors to a shadow ring
   in guest RAM; the bridge device polls it and forwards the MMIO to the host
   SPDK NVMe controller's BAR via `address_space_write`.
3. **SPDK NVMe-KV controller** (`spdk` `ctrlr_kvdev.c`): the CSI=KV namespace
   decodes the key, applies the read-only / KV-Exec gates, and dispatches to the
   `kvdev`.
4. **`kvdev_rados`** (`spdk` `module/kvdev/rados`): Stores/Retrieves the value as
   a RADOS object via async `librados`.
5. **Ceph/RADOS**: the object lands in `pool=kvpool, namespace=kvns`, named by
   the hex image of the key.

## On-the-wire KV encoding (NVMe KV Command Set)

KV opcodes are numerically equal to block Write/Read; the controller routes them
as KV because the **namespace CSI is Key Value (0x1)**.

| Field | Location | Notes |
|---|---|---|
| Opcode | CDW0 | Store `0x01`, Retrieve `0x02` |
| NSID | CDW1 | the KV namespace id |
| Key length | CDW11 bits 7:0 | 1..16 |
| Key bytes 0..7 | CDW2 / CDW3 | flat little-endian image |
| Key bytes 8..15 | CDW14 / CDW15 | flat little-endian image |
| Value size | CDW10 | value len (Store) / host-buf size (Retrieve) |
| Value data | PRP1 / PRP2 (DPTR) | same as a block transfer |
| Retrieve result | CQE **DW0** | TRUE stored value length |

Status `0x87` = key does not exist (reported, not fatal); `0x86` = invalid key
size; `0x85` = invalid value size. Full detail in
[`rocm-xio/docs/nvme-kv.md`](../rocm-xio/docs/nvme-kv.md).

## Running it

> Requires the `spdk`, `rocm-xio`, and `qemu` submodules populated and built —
> see [`build.md`](build.md). The bundled
> [`rocm-xio/examples/stage2_kv_rados_gpu.sh`](../rocm-xio/examples/stage2_kv_rados_gpu.sh)
> is the reference harness; the steps below are its shape.

### 1. Stand up the SPDK NVMe-KV-on-RADOS target

With `nvmf_tgt` running, one command brings up the whole target (see
[`scripts/rados-nkv`](../scripts/rados-nkv) and [`demo-e2e.md`](demo-e2e.md)
Step 1 for the underlying RPC sequence and options):

```bash
scripts/rados-nkv up          # transport + kvdev_rados + subsystem + CSI=KV ns + listener
# → KV target up at /var/run/muser/domain/kv/0
```

The guest Linux NVMe driver enumerates this KV-only controller directly
(`CC.CSS=IOCS`, exposes `/dev/nvme0`) — no block namespace required.

### 2. GPU Store, then Retrieve

```bash
# GPU __device__ code issues the KV Store (value sourced from the GPU write buffer)
xio-tester nvme-ep --controller /dev/nvme0 \
    --kv-op store --key gpukey01 --value-size 4096 --write-io 1 --pci-mmio-bridge

# GPU Retrieve — value lands in the GPU read buffer (VRAM with --memory-mode 8)
xio-tester nvme-ep --controller /dev/nvme0 \
    --kv-op retrieve --key gpukey01 --value-size 4096 --read-io 1 --pci-mmio-bridge
```

### 3. Verify the value reached Ceph

```bash
rados -p kvpool -N kvns stat $(printf 'gpukey01' | xxd -p)
```

### Batched / wavefront KV (multi-key)

`--keys` + `--batch-size B>1` drives the cooperative
`driveEndpointKvWavefront()` path: threads `1..B` each encode one key's
Store/Retrieve into their own value-buffer slot, and thread 0 rings the SQ
doorbell once, polls `B` completions, and rings the CQ doorbell — useful for
fetching multiple weight shards per doorbell ring.

```bash
xio-tester nvme-ep --controller /dev/nvme0 --kv-op retrieve \
    --keys shard0 shard1 shard2 shard3 \
    --batch-size 4 --value-size 4096 --data-buffer-size 16384 --pci-mmio-bridge
```

The value buffer must hold `batch-size * value-size` bytes.

## Status (from `rocm-xio/docs/nvme-kv.md`)

- ✅ rocm-xio KV path implemented; CLI wired; block path untouched.
- ✅ SPDK vfio-user NVMe-KV target (`kvdev_rados`) comes up clean host-side.
- ✅ End-to-end GPU run verified: GPU issues KV Store then Retrieve and the
  value lands directly in GPU memory.
- ⏳ Follow-ups: KV Delete/Exist/List opcodes from the GPU, multi-key manifest
  from a file, per-key value sizes for weight-shard fetch.

## The guest VM (build + launch)

The GPU-side commands above run **inside a guest** with the GPU passed through.
The [`provisioning/`](../provisioning) layer (built on the
[`qemu-minimal`](../qemu-minimal) submodule) builds and launches it:

```bash
make vm                # build the guest image: Ubuntu + ROCm + rocm-xio
                       # (cloud-init installs ROCm and builds xio-tester in-guest)
make vm-vfio-rules     # VFIO udev permissions (once; sudo)
make vm-run            # launch — the proven pci-mmio-bridge bring-up
```

`make vm-run` runs [`provisioning/launch-bridge-vm.sh`](../provisioning/launch-bridge-vm.sh),
the exact QEMU invocation that worked (vendored from qemu-xio's
`run-qemu-pci-mmio-bridge`): `-machine q35,accel=kvm -cpu EPYC`, the GPU via
`vfio-pci`, the `pci-mmio-bridge` (`shadow-gpa=0x80000000,shadow-size=8192,poll-interval-ns=1000000`),
and an emulated NVMe (`ioeventfd=off,dbcs=off`, required by the bridge). It uses
this repo's built `qemu/` (the `pci-mmio-bridge` fork) and `vm-images/ceph-gpu.qcow2`;
`GPU_BDF`, `IMAGE`, `SSH_PORT`, etc. are env-overridable.

For the **full GPU→SPDK→RADOS** path, attach the SPDK vfio-user NVMe-KV target
(Step 1) instead of the emulated NVMe:

```bash
scripts/rados-nkv up                       # -> /var/run/muser/domain/kv/0
PCI_HOSTDEV=0000:bd:00.0 \
VFIO_USERDEV=/var/run/muser/domain/kv/0 \
PCI_MMIO_BRIDGE=on  make vm-run-spdk        # run-vm with the same bridge params
```

## Hardware passthrough reference

For the AMD AI Max 395 / Radeon 8060S iGPU passthrough setup that this flow runs
on (IOMMU, `vfio-pci` binding, ROM), see the
`Proxmox_AMD_AI_Max_395_Radeon_8060s_GPU_Passthrough` notes alongside this tree
(not vendored here) and `make vm-vfio-rules`.
