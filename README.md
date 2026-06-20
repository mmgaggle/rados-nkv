
![RADOS-NKV: fast KV store for AI](docs/rados-nkv.png)

# RADOS-NKV

Exposes [RADOS](https://ceph.io/assets/pdfs/weil-rados-pdsw07.pdf), a reliable object storage service through the NVMe key-value
command set. Supports host or GPU initiated IO and _computation_. Data bypasses
the CPU with peer-to-peer DMA. A fast KV store for AI.

RADOS-NKV exposes Ceph/RADOS through the **NVMe Key-Value command set**. A
target presents KV namespaces with a sandboxed near-data pluggable execution
runtime with a linear cache (rados-nkvx). Key-value data is mapped 1-1 to
RADOS objects via librados.

* Subsystem -> Pool
* Namespace -> RADOS Namespace
* Key -> RADOS object OID
* Value -> RADOS object data

## RADOS-NKVX

RADOS-NKVX is designed to run close to data and will often be co-located on
OSD hosts. The execution runtime is pluggable, with a WebAssembly reference
implementation.

## Containerized deployment

The datapath ships as a single multi-stage image, the `NKV_ROLE` enviornmental
variable is used by the entrypoint to start either a GPU host side rados-nkv
or a OSD host side rados-nkvx.

```
podman run quay.io/mmgaggle/rados-nkv:tentacle
```

## Getting the code

Clone the repo and pull submodules

```bash
git clone git@github.com:mmgaggle/rados-nkv.git
cd rados-nkv
git submodule update --init --recursive          # everything (pulls several GB)

# or, per flow:
git submodule update --init spdk clients/rocm-xio clients/vm/qemu clients/vm/qemu-minimal   # Flow A (GPU-initiated)
git submodule update --init spdk clients/nixl                         # Flow B (host consumers)
git submodule update --init ceph                              # real RADOS backend (either flow)
```

> Skip `ceph` and use SPDK's in-memory `kvdev_mem` backend for a dev loop that
> needs no cluster. The `vllm-weights` catalog is vendored in-tree under
> `clients/`; all submodules are public.

## Building

A CMake superbuild ([`CMakeLists.txt`](CMakeLists.txt)) + top-level
[`Makefile`](Makefile) orchestrate every component's native build in dependency
order (Ceph → SPDK → `clients`/`rados-nkvx` → the rest):

```bash
make init                # fetch all submodules
make build               # build everything (build-<component> for just one)
make vstart              # throwaway development Ceph cluster
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
