
![rados-nkv](./docs/rados-nkv.png)

# ceph-gpu-initiated

**End-to-end demonstration of GPU-initiated NVMe Key-Value I/O to Ceph/RADOS.**

This repository wires together — as pinned submodules — the full stack that lets
an AMD GPU issue NVMe **Key-Value** Store/Retrieve commands from `__device__`
code, with the value landing directly in GPU memory, and the key/value persisted
as a RADOS object in a Ceph cluster. The same NVMe-KV-on-RADOS target also backs
two host-side consumers: the **NIXL** `RADOS_NKV` transfer backend (llm-d
KV-cache offload) and the **rados-nkv-weights** model-weights catalog.

There is no filesystem, no mount, and no object-storage credentials in the data
path — just the NVMe KV command set over an SPDK vfio-user controller backed by
`librados`.

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
                 └──────────────────────────────────────────────┘
                                       │ librados
                                       ▼
                          Ceph / RADOS  (pool = subsystem,
                          namespace = tenant, 1 object per KV pair)
```

## What's combined here

| Submodule | Source | Branch | Role in the demo |
|-----------|--------|--------|------------------|
| [`ceph`](ceph) | [`ceph/ceph`](https://github.com/ceph/ceph/tree/tentacle) | `tentacle` | **The storage cluster.** Vendored so you can `vstart` a throwaway RADOS cluster for e2e, and so SPDK's `rados` kvdev backend links the matching `librados`. Latest stable release branch. |
| [`spdk`](spdk) | [`mmgaggle/spdk`](https://github.com/mmgaggle/spdk/tree/rados-nkv) | `rados-nkv` | **The substrate.** NVMe-KV command set, the `kvdev` device layer with `mem` and `rados` (librados) backends, the NVMf KV controller (`ctrlr_kvdev`, CSI=KV namespaces, read-only & Exec gates), and the in-process **KV host shim** that the host-side consumers link against. |
| [`rocm-xio`](rocm-xio) | [`mmgaggle/rocm-xio`](https://github.com/mmgaggle/rocm-xio/tree/nvme-kv) | `nvme-kv` | **GPU-initiated path.** `nvme-ep --kv-op store/retrieve` — GPU `__device__` code builds the KV SQE, rings the doorbell, polls the CQ, and the value lands in host RAM or VRAM (`--memory-mode 8`). |
| [`qemu`](qemu) | [`sbates130272/qemu`](https://github.com/sbates130272/qemu) | `dev/stephen/pci-mmio-bridge-submit` | **GPU↔NVMe bridge.** The `pci-mmio-bridge` device polls a guest shadow ring and forwards the GPU's doorbell MMIO to the SPDK NVMe BAR, so the passed-through GPU can drive a vfio-user NVMe controller. |
| [`qemu-minimal`](qemu-minimal) | [`sbates130272/qemu-minimal`](https://github.com/sbates130272/qemu-minimal) | `main` | **Guest VM tooling.** Cloud-init VM creation (`gen-vm`) and a launcher (`run-vm`) with the knobs this demo needs — GPU `vfio-pci` passthrough, libvfio-user sockets, and the `pci-mmio-bridge` device. The provisioning layer here builds on it. |
| [`nixl`](nixl) | [`mmgaggle/nixl`](https://github.com/mmgaggle/nixl/tree/rados-nkv) | `rados-nkv` | **Host consumer #1.** The `RADOS_NKV` NIXL backend maps `NIXL_WRITE`/`NIXL_READ`/`queryMem` onto KV Store/Retrieve/Exist through the SPDK host shim (the llm-d KV-cache offload transport). |
| [`rados-nkv-weights`](rados-nkv-weights) | `github.ibm.com/ceph/rados-nkv-weights` | `main` | **Host consumer #2.** A model-weights catalog (publisher + loader) over a shared read-only NVMe-KV namespace, using the same host shim. Content-hash chunk dedup + Arrow per-model manifest. |

## Documentation

Start with the architecture, then pick a flow:

- [`docs/architecture.md`](docs/architecture.md) — the substrate and every component, layer by layer.
- [`docs/flow-a-gpu-initiated.md`](docs/flow-a-gpu-initiated.md) — **GPU-initiated** NVMe-KV: GPU → qemu bridge → SPDK → RADOS.
- [`docs/flow-b-host-consumers.md`](docs/flow-b-host-consumers.md) — **host-side** NIXL and weights consumers over the same target.
- [`docs/build.md`](docs/build.md) — build order and per-component build commands.
- [`docs/demo-e2e.md`](docs/demo-e2e.md) — step-by-step bring-up of the full end-to-end demo.
- [`docs/diagrams/`](docs/diagrams) — Mermaid + rendered PNGs.

The [`scripts/rados-nkv`](scripts/rados-nkv) helper brings the SPDK NVMe-KV target
up or down with one command (`scripts/rados-nkv up` / `down` / `status`),
wrapping the SPDK JSON-RPC sequence. `--mem` selects the no-Ceph in-memory
backend; `--read-only` creates a loader-style namespace. Run `scripts/rados-nkv help`
for all options.

## Getting the code

The submodules are **pinned to exact commits** but not vendored; populate them
when you're ready to build (this pulls several GB — SPDK and QEMU are large):

```bash
git clone <this repo> ceph-gpu-initiated
cd ceph-gpu-initiated
git submodule update --init --recursive
```

To populate only what you need for a given flow:

```bash
# Flow A (GPU-initiated): spdk + rocm-xio + qemu + the guest VM tooling
git submodule update --init spdk rocm-xio qemu qemu-minimal

# Flow B (host consumers): spdk + nixl + rados-nkv-weights
git submodule update --init spdk nixl rados-nkv-weights

# Real Ceph backend (either flow): add ceph for a vstart cluster
git submodule update --init ceph
```

> **Heads up:** `ceph` is a large repository with its own submodules; expect a
> sizable checkout. Skip it and use SPDK's in-memory `kvdev_mem` backend for a
> dev loop that needs no cluster.

> **Note:** `rados-nkv-weights` is hosted on the internal `github.ibm.com`
> remote; populating it requires access to that host. The other four submodules
> are public.

## Building

A CMake superbuild ([`CMakeLists.txt`](CMakeLists.txt)) + top-level
[`Makefile`](Makefile) orchestrate every component's native build in dependency
order:

```bash
make init                # fetch all submodules
make build               # build everything (build-<component> for just one)
make vstart              # throwaway Ceph cluster
make up                  # bring up the SPDK NVMe-KV target
make vm                  # build the ROCm + rocm-xio guest image (Flow A)
make vm-run              # launch it (proven pci-mmio-bridge bring-up)
```

Build a subset with `make configure CMAKE_ARGS='-DWITH_QEMU=OFF'` then
`make build`; `make help` lists every target. Full details — including the
manual per-component commands — are in [`docs/build.md`](docs/build.md).

## License

Integration content in this repository (docs, scripts) is Apache-2.0. Each
submodule retains its own upstream license (SPDK: BSD-3-Clause; rocm-xio: MIT;
NIXL: Apache-2.0; QEMU: GPL-2.0; rados-nkv-weights: Apache-2.0).
