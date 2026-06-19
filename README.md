
# RADOS-NKV

**The full power of RADOS, extended over NVMe key-value, for GPU-initiated and host-side data paths.**

RADOS-NKV exposes Ceph/RADOS through the **NVMe Key-Value command set**. An SPDK
`nvmf` target presents KV namespaces — `Store` / `Retrieve` / `List` / `Delete` /
`Exist`, plus a sandboxed near-data `Exec` — backed by `librados`, with each
key/value persisted as a RADOS object (pool = subsystem, namespace = tenant). There
is no filesystem, no mount, and no object-storage credentials in the data path:
just the NVMe-KV command set over an SPDK vfio-user controller.

One target serves three consumers over the same substrate:

- **GPU-initiated** — an AMD GPU issues KV Store/Retrieve directly from
  `__device__` code, with the value landing in GPU memory (VRAM via P2P-DMA).
- **NIXL `RADOS_NKV`** — an NVMe-KV transfer backend for llm-d KV-cache offload.
- **rados-nkv-weights** — a model-weights catalog over a shared read-only namespace.

```
   GPU __device__ code (gfx1151)                 host-side NIXL / weights
   KV Store / Retrieve  ──┐                        Store/Retrieve/Exist
                          │                                │
            qemu pci-mmio-bridge          in-process SPDK KV host shim
            (doorbell MMIO → BAR)                          │
                          │                                │
                          ▼                                ▼
                 ┌──────────────────────────────────────────────┐
                 │   SPDK nvmf_tgt — NVMe-KV controller (CSI=KV) │
                 │   ctrlr_kvdev → kvdev (mem | rados)           │
                 │   Exec → rados-nkvx wasmtime executor         │
                 └──────────────────────────────────────────────┘
                                       │ librados
                                       ▼
                          Ceph / RADOS  (1 object per KV pair)
```

This repository is the **project home**. It pins the SPDK substrate and every
component as submodules, and carries the integration glue, the standalone Exec
executor, the KV client/test harnesses, and the documentation.

## Components

**In this repository:**

| Path | Role |
|------|------|
| [`clients/`](clients) | NVMe-KV host & test harnesses — the `kv_host_shim` the host consumers link against, the standalone hosts, the `rkv` command-line client (the `rados-nkv` Rust/HIP crate — the ergonomic way to drive the datapath by hand), and the vfio-user host. Builds against the `spdk` submodule. |
| [`rados-nkvx/`](rados-nkvx) | The standalone, restartable **Exec executor** (wasmtime sandbox) that runs near-data Exec modules on the storage host. |
| [`docs/`](docs), [`CMakeLists.txt`](CMakeLists.txt), [`scripts/`](scripts) | Architecture/flow docs, the CMake superbuild + `make` wrapper, and the `rados-nkv` target bring-up helper. |

**Pinned submodules:**

| Submodule | Source | Branch | Role |
|-----------|--------|--------|------|
| [`spdk`](spdk) | [`mmgaggle/spdk`](https://github.com/mmgaggle/spdk/tree/devel) | `devel` | **The substrate.** NVMe-KV command set, the `kvdev` device layer with `mem` and `rados` (librados) backends, the NVMf KV controller (`ctrlr_kvdev`, CSI=KV namespaces, read-only & Exec gates), and the in-process **KV host shim**. |
| [`ceph`](ceph) | [`ceph/ceph`](https://github.com/ceph/ceph/tree/tentacle) | `tentacle` | **The storage cluster.** `vstart` a throwaway RADOS cluster for e2e, and provide the `librados` the `rados` kvdev backend links. |
| [`rocm-xio`](clients/rocm-xio) | [`mmgaggle/rocm-xio`](https://github.com/mmgaggle/rocm-xio/tree/nvme-kv) | `nvme-kv` | **GPU-initiated path.** `nvme-ep --kv-op` — GPU `__device__` code builds the KV SQE, rings the doorbell, polls the CQ; the value lands in host RAM or VRAM. |
| [`qemu`](qemu) | [`sbates130272/qemu`](https://github.com/sbates130272/qemu) | `dev/stephen/pci-mmio-bridge-submit` | **GPU↔NVMe bridge.** The `pci-mmio-bridge` device forwards the GPU's doorbell MMIO to the SPDK NVMe BAR. |
| [`qemu-minimal`](qemu-minimal) | [`sbates130272/qemu-minimal`](https://github.com/sbates130272/qemu-minimal) | `main` | **Guest VM tooling.** Cloud-init VM creation + a launcher with GPU `vfio-pci` passthrough, libvfio-user sockets, and the bridge device. |
| [`nixl`](nixl) | [`mmgaggle/nixl`](https://github.com/mmgaggle/nixl/tree/rados-nkv) | `rados-nkv` | **Host consumer.** The `RADOS_NKV` NIXL backend maps `NIXL_WRITE`/`READ`/`queryMem` onto KV Store/Retrieve/Exist. |
| [`rados-nkv-weights`](rados-nkv-weights) | `github.ibm.com/ceph/rados-nkv-weights` | `main` | **Host consumer.** Model-weights catalog (publisher + loader) over a shared read-only NVMe-KV namespace. |

## Documentation

Start with the architecture, then pick a flow:

- [`docs/architecture.md`](docs/architecture.md) — the substrate and every component, layer by layer.
- [`docs/flow-a-gpu-initiated.md`](docs/flow-a-gpu-initiated.md) — **GPU-initiated** NVMe-KV: GPU → qemu bridge → SPDK → RADOS.
- [`docs/flow-b-host-consumers.md`](docs/flow-b-host-consumers.md) — **host-side** NIXL and weights consumers over the same target.
- [`docs/build.md`](docs/build.md) — build order and per-component build commands (incl. `clients/` and `rados-nkvx/`).
- [`docs/demo-e2e.md`](docs/demo-e2e.md) — step-by-step bring-up of the full end-to-end path.

The [`scripts/rados-nkv`](scripts/rados-nkv) helper brings the SPDK NVMe-KV target
up or down with one command (`up` / `down` / `status`). `--mem` selects the
no-Ceph in-memory backend; `--read-only` creates a loader-style namespace.

## Getting the code

Submodules are **pinned to exact commits** but not vendored; populate what you need:

```bash
git clone git@github.com:mmgaggle/rados-nkv.git
cd rados-nkv
git submodule update --init --recursive          # everything (pulls several GB)

# or, per flow:
git submodule update --init spdk clients/rocm-xio qemu qemu-minimal   # Flow A (GPU-initiated)
git submodule update --init spdk nixl rados-nkv-weights       # Flow B (host consumers)
git submodule update --init ceph                              # real RADOS backend (either flow)
```

> Skip `ceph` and use SPDK's in-memory `kvdev_mem` backend for a dev loop that
> needs no cluster. `rados-nkv-weights` is on the internal `github.ibm.com`
> remote; the other submodules are public.

## Building

A CMake superbuild ([`CMakeLists.txt`](CMakeLists.txt)) + top-level
[`Makefile`](Makefile) orchestrate every component's native build in dependency
order (Ceph → SPDK → `clients`/`rados-nkvx` → the rest):

```bash
make init                # fetch all submodules
make build               # build everything (build-<component> for just one)
make vstart              # throwaway Ceph cluster
make up                  # bring up the SPDK NVMe-KV target
make vm / vm-run         # build + launch the ROCm guest image (Flow A)
```

Build a subset with `make configure CMAKE_ARGS='-DWITH_QEMU=OFF'` then `make build`;
`make help` lists every target. Full details — including the manual per-component
commands — are in [`docs/build.md`](docs/build.md).

## License

This repository is **LGPL-3.0** (see [`COPYING`](COPYING)), aligning with the Ceph
project / ceph-nvmeof convention. The SPDK-derived components — [`clients/`](clients)
and [`rados-nkvx/`](rados-nkvx) — are **BSD-3-Clause** (their SPDK origin), which
composes cleanly into the LGPL-3.0 work. Each submodule retains its own upstream
license (SPDK: BSD-3-Clause; Ceph: LGPL-2.1; rocm-xio: MIT; NIXL: Apache-2.0;
QEMU: GPL-2.0). See [`LICENSING.md`](LICENSING.md) for the per-path breakdown.
