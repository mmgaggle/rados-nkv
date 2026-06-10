# Architecture

The whole demo rests on one idea: expose **Ceph/RADOS through the NVMe Key-Value
command set**. A KV namespace on an SPDK vfio-user controller maps each
`(key, value)` pair to a single RADOS object. Anything that can speak NVMe-KV —
a GPU issuing commands from `__device__` code, or a host process linking an
in-process SPDK shim — becomes a client, with no filesystem, mount, or
object-storage credentials in the data path.

```
       Flow A (GPU-initiated)                Flow B (host-side consumers)
  ┌───────────────────────────────┐    ┌──────────────────────────────────────┐
  │ rocm-xio nvme-ep --kv-op      │    │ NIXL RADOS_NKV     rados-nkv-weights │
  │  (GPU __device__ KV SQE)      │    │  WRITE/READ/query   publisher/loader │
  │            │                  │    │        │                  │          │
  │  qemu pci-mmio-bridge         │    │        └── kv_host_shim.{c,h} ───────┤
  │  (doorbell MMIO → NVMe BAR)   │    │            (in-process SPDK)         │
  └────────────┼──────────────────┘    └────────────────┼─────────────────────┘
               │ vfio-user                              │ vfio-user (loopback)
               ▼                                        ▼
        ┌──────────────────────────────────────────────────────────────┐
        │  SPDK nvmf_tgt                                               │
        │   • lib/nvme/nvme_kv.c, include/spdk/nvme_kv.h  (KV cmd set) │
        │   • lib/nvmf/ctrlr_kvdev.c   (NVMf KV controller, CSI=KV)    │
        │       – key decode · read-only gate · KV-Exec allowlist      │
        │   • lib/kvdev/kvdev.c        (device abstraction)            │
        │   • module/kvdev/mem         (in-memory backend)             │
        │   • module/kvdev/rados       (librados backend)  ──────┐     │
        └────────────────────────────────────────────────────────┼─────┘
                                                                 │ librados
                                                                 ▼
                       Ceph / RADOS:  pool = subsystem,  namespace = tenant,
                                      one object per KV pair
```

See the rendered diagrams in [`diagrams/`](diagrams):
[`rados-nkv`](diagrams/rados-nkv.png) (the host-side loopback substrate) and
[`gpu-nvme-passthrough`](diagrams/gpu-nvme-passthrough.png) (the GPU-initiated
path through the QEMU bridge).

## The substrate — `spdk` (`mmgaggle/spdk@rados-nkv`)

This is the keystone; every other component targets it.

| Area | Path | What it provides |
|------|------|------------------|
| KV command set | `lib/nvme/nvme_kv.c`, `include/spdk/nvme_kv.h` | Encode/decode of the NVMe KV Store/Retrieve/Delete/Exist/List commands. |
| NVMf KV controller | `lib/nvmf/ctrlr_kvdev.c` | Presents a **CSI=Key Value (0x1)** namespace over the NVMf target; decodes the key from the SQE, enforces the per-namespace **read-only** gate and the **KV-Exec** allowlist, and dispatches to a `kvdev`. |
| Device abstraction | `lib/kvdev/kvdev.c`, `include/spdk/kvdev.h` | Backend-agnostic `kvdev` interface (Store/Retrieve/Exist/Delete) the controller calls. |
| In-memory backend | `module/kvdev/mem/` + `kvdev_mem_rpc.c` | A dict-style KV device for dev/test — no Ceph required. |
| RADOS backend | `module/kvdev/rados/` + `kvdev_rados_rpc.c` | Maps each KV pair to a RADOS object via async `librados`: **pool = subsystem, namespace = tenant, one object per KV pair**. |
| Host shim | `test/nvmf/kv_shim/kv_host_shim.{c,h}` | A thin in-process NVMe-KV client (over the vfio-user transport) that host programs link to issue Store/Retrieve/Exist. Used by **both** the NIXL backend and the weights catalog. |

RADOS object mapping (from [`diagrams/rados-nkv.mmd`](diagrams/rados-nkv.mmd)):
the namespace's `kvdev_rados` instance is bound to a Ceph **pool** and RADOS
**namespace**; the 1..16-byte NVMe key becomes the object name (its hex image),
and the value becomes the object's data. `rados -p <pool> -N <ns> stat <hex(key)>`
shows the object.

### Capability gates (security model)

The namespace is **asymmetric** by design and enforced target-side in
`ctrlr_kvdev.c`:

- A **read-only** namespace accepts only `Retrieve`/`Exist`; `Store`/`Delete`
  are rejected (deny-by-default). This is how the weights loader fleet attaches.
- **KV-Exec** is gated behind an allowlist (a separate per-namespace privilege
  boundary), so arbitrary exec is not exposed by default.
- A privileged **admin** connection is what the weights publisher uses to
  `Store`.

## Flow A components

### `rocm-xio` — GPU-initiated NVMe-KV (`mmgaggle/rocm-xio@nvme-kv`)

rocm-xio is AMD's library for **Accelerator-Initiated IO (XIO)** — GPUs driving
NVMe/RDMA/SDMA directly from `__device__` code. This branch adds a **KV mode**
to the existing `nvme-ep` endpoint as an *additive flag* (`--kv-op`), reusing the
entire endpoint machinery (admin-queue creation, doorbells incl. the
`--pci-mmio-bridge` path, PRP/value-buffer allocation in host RAM or VRAM, CQ
polling). The only delta is the **KV SQE encoding** and reading the returned
value length from the completion (CQE DW0).

Key files: `src/include/nvme-kv.h` (opcodes, status, `kvSqeSetup()`),
`src/endpoints/nvme-ep/nvme-ep.hip` (`driveEndpointKv()` serial +
`driveEndpointKvWavefront()` batched), `docs/nvme-kv.md` (design + usage),
`examples/stage2_kv_rados_gpu.sh` (the e2e harness against an SPDK kvdev_rados
target). See [`flow-a-gpu-initiated.md`](flow-a-gpu-initiated.md).

### `qemu` — the pci-mmio-bridge (`sbates130272/qemu@dev/stephen/pci-mmio-bridge-submit`)

When the GPU is passed through to a guest VM, its doorbell writes are MMIO that
must reach the SPDK NVMe controller's BAR on the host. The `pci-mmio-bridge`
QEMU device polls a guest **shadow ring** (where the GPU writes command
descriptors) and forwards the MMIO via `address_space_write` to the host SPDK
target. This is the mechanism in
[`diagrams/gpu-nvme-passthrough.mmd`](diagrams/gpu-nvme-passthrough.mmd).

## Flow B components

Both host consumers link the SPDK `kv_host_shim` and talk to the same KV
namespace over a **vfio-user loopback** (no VM, no network) — the substrate in
[`diagrams/rados-nkv.mmd`](diagrams/rados-nkv.mmd).

### `nixl` — the `RADOS_NKV` backend (`mmgaggle/nixl@rados-nkv`)

A NIXL South-Bound storage backend (`src/plugins/rados-nkv/`) that maps:

| NIXL op | Local / remote mem | NVMe KV command |
|---------|--------------------|-----------------|
| `NIXL_WRITE` | `DRAM_SEG` → `OBJ_SEG` | KV **Store** |
| `NIXL_READ` | `OBJ_SEG` → `DRAM_SEG` | KV **Retrieve** |
| `queryMem` | `OBJ_SEG` | KV **Exist** (cache hit/miss, no data) |

The `OBJ_SEG` descriptor's `metaInfo` carries a token sequence; the engine
derives the fixed-length NVMe KV key as a 128-bit FNV-1a hash truncated to
`min(16, kvkml)` (`radosNkvDeriveKey`). This is the llm-d KV-cache offload
transport. See [`flow-b-host-consumers.md`](flow-b-host-consumers.md).

### `rados-nkv-weights` — the weights catalog (`github.ibm.com/ceph/rados-nkv-weights@main`)

A **second, independent consumer** of the same substrate that serves *immutable
model weights* to a GPU fleet through one shared, **read-only** KV namespace
that acts as a catalog:

- **Publisher** (privileged, admin connection): chunks each tensor, Stores each
  chunk under its content-hash **Chunk key** (`blake2b(bytes, 16)`) — skipping
  any that already **Exist** (dedup) — and writes the per-revision **Weight
  manifest** (an Arrow IPC table, the safetensors-header analog).
- **Loader** (unprivileged, read-only namespace): Retrieves the manifest by its
  deterministic **Manifest key** (no `List` needed), then Retrieves each
  tensor's chunks in order and reassembles.

The native transport (`NvmeKvClient`) is a ctypes wrapper over the same SPDK
`kv_host_shim.c`. See [`flow-b-host-consumers.md`](flow-b-host-consumers.md).

## Why one substrate, two flows

Both flows prove the same target is a general NVMe-KV-on-RADOS service:

- **Flow A** proves the *device-initiated* path — a GPU with no host CPU in the
  loop can persist/fetch values to Ceph, value landing in VRAM.
- **Flow B** proves the *host-process* path — production consumers (llm-d
  KV-cache offload via NIXL, and model-weights distribution) ride the identical
  controller and `kvdev_rados` backend.

The KV namespace itself is backend-agnostic: every component runs unchanged
against an in-memory `kvdev_mem` (fast dev loop, no Ceph) or the librados-backed
`kvdev_rados` (real Ceph/RADOS).
