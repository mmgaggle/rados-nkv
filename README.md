
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
- **vllm-weights** — a model-weights catalog over a shared read-only namespace.

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

A component-by-component breakdown — the in-repo paths, the pinned submodules,
and the documentation index — is in [`docs/architecture.md`](docs/architecture.md).

## Getting the code

Submodules are **pinned to exact commits** but not vendored; populate what you need:

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
make vstart              # throwaway Ceph cluster
make up                  # bring up the SPDK NVMe-KV target
make vm / vm-run         # build + launch the ROCm guest image (Flow A)
```

Build a subset with `make configure CMAKE_ARGS='-DWITH_QEMU=OFF'` then `make build`;
`make help` lists every target. Full details — including the manual per-component
commands — are in [`docs/build.md`](docs/build.md).

## Container image (Podman)

The datapath ships as a single multi-stage image
([`packaging/container/Dockerfile`](packaging/container/Dockerfile)) — one image,
two roles, dispatched at run time on `$NKV_ROLE` (`nkv` = front/target,
`nkvx` = Exec executor). The `builder` stage reproduces the validated host build
(vendored Mercury + wasmtime, SPDK `--with-rbd --with-vfio-user`, `rados-nkvx`)
and `rpmbuild`s the RPMs; the thin `runtime` stage just installs them. Ceph is
**not** built — SPDK links the distro's `librados-devel`.

The build context must carry the `spdk` submodule populated **recursively**:

```bash
git submodule update --init --recursive spdk
podman build -f packaging/container/Dockerfile -t rados-nkv:devel .
```

Prefer the helper, which stamps the ceph-nvmeof-style tag from [`VERSION`](VERSION)
(`<registry>/rados-nkv:devel_v<semver>`, plus a floating `:devel`):

```bash
packaging/container/build.sh              # build only
packaging/container/build.sh --push       # build + push

# knobs (env):
REGISTRY=quay.io/ceph \
CEPH_RELEASE=tentacle_9.2 \   # release label baked into the tag prefix
ENGINE=podman \               # podman | docker
  packaging/container/build.sh
```

Run a role by setting `NKV_ROLE` (there is no safe default):

```bash
podman run --rm -e NKV_ROLE=nkv rados-nkv:devel
```

## License

This repository is **LGPL-3.0** (see [`COPYING`](COPYING)), aligning with the Ceph
project / ceph-nvmeof convention. The SPDK-derived components — [`clients/`](clients)
and [`rados-nkvx/`](rados-nkvx) — are **BSD-3-Clause** (their SPDK origin), which
composes cleanly into the LGPL-3.0 work. Each submodule retains its own upstream
license (SPDK: BSD-3-Clause; Ceph: LGPL-2.1; rocm-xio: MIT; NIXL: Apache-2.0;
QEMU: GPL-2.0). See [`LICENSING.md`](LICENSING.md) for the per-path breakdown.
