
![RADOS-NKV: fast KV store for AI](docs/rados-nkv.png)

# RADOS-NKV

Exposes [RADOS](https://ceph.io/assets/pdfs/weil-rados-pdsw07.pdf), a reliable object storage service through the NVMe key-value
command set. Supports host or GPU initiated IO and _computation_. Data bypasses
the CPU with peer-to-peer DMA. A fast KV store for AI.

RADOS-NKV exposes Ceph/RADOS through the **NVMe Key-Value command set**. The
target (`rados-nkv`) presents KV namespaces and maps key-value data 1:1 to
RADOS objects via `librados`. A pluggable near-data execution runtime with a
linear cache (`rados-nkvx`) runs computation close to the data.

* Subsystem -> Pool
* Namespace -> RADOS Namespace
* Key -> RADOS object OID
* Value -> RADOS object data

All standard commands are supported: store, exist, retrieve, delete, and list.

Standard KV initiators are limited to 16-byte keys. RADOS-NKV clients support
up to 255-byte canonical keys. Keys of 16 bytes or fewer are carried inline in
the command (standard NVMe-KV). Keys longer than 16 bytes are carried
length-prefixed at the head of the DPTR/SGL payload.


## Execution

A vendor specific Exec command provides computational storage capabilities.
The reference execution runtime is based on Wasmtime for WebAssembly. Compiled
wasm can be stored via the key-value interface, to make it elgible for execution
it must be allowlisted by digest on a per-namespace basis. The Wasmtime
execution runtime also allows memory and fuel limitations.


## Running the containers

The datapath ships as a **single multi-stage image, two roles**, selected by the
`NKV_ROLE` environment variable the entrypoint reads:

- `NKV_ROLE=nkv` — the front/target: starts `nvmf_tgt` and applies the
  NVMe-KV-over-vfio-user config (`rados-nkv up`).
- `NKV_ROLE=nkvx` — the executor: runs the standalone `nkvx_service` (the Mercury
  Exec target), co-located with the OSDs.

Image: `quay.io/mmgaggle/rados-nkv:devel` (floating dev tag) or a version-pinned
`:devel_v<version>` (e.g. `:devel_v0.1.0`).

Front knobs: `BACKEND` (`rados`|`mem`), `POOL`, `CEPH_NS`, `NQN`, `VFU_DIR`,
`CEPH_CONF`, `CEPH_USER`, `READ_ONLY`, `REMOTE_EXECUTOR`, `NVMF_TGT_ARGS`.
Executor knobs: `NKVX_LISTEN`, `NKVX_ADDR_FILE`, `NKVX_RADOS_{POOL,NAMESPACE,CONF,USER}`.
The full list is in `packaging/rpm/rados-nkv{,x}.sysconfig` and `scripts/rados-nkv help`.

> **vfio-user is host-local.** The tenant (GPU / host client) attaches to the
> UNIX socket the front creates under `VFU_DIR` (default
> `/var/run/muser/domain/kv/0`), so that directory must be shared between the
> front container and the client. SPDK also needs hugepages — or
> `NVMF_TGT_ARGS="--no-huge -s 1024"` on a dev box — plus the privileges to set
> up vfio-user; the examples use `--privileged` for brevity (narrow to the
> specific caps + `-v /dev/hugepages` for production). See
> [`docs/demo-e2e.md`](docs/demo-e2e.md).

### Target with the in-memory backend (no Ceph)

A dev loop with no cluster — the `kvdev_mem` backend keeps values in the target's
RAM:

```bash
podman run --rm --name nkv --privileged \
  -v /var/run/muser:/var/run/muser \
  -e NKV_ROLE=nkv \
  -e BACKEND=mem \
  -e NVMF_TGT_ARGS="--no-huge -s 1024" \
  quay.io/mmgaggle/rados-nkv:devel
```

The KV controller is then reachable at the vfio-user socket
`/var/run/muser/domain/kv/0`.

### Single-tier (rados backend, in-process Exec)

The whole stack in one process: the front reads/writes RADOS via `librados`, and
Exec runs in-process. No `REMOTE_EXECUTOR`, so there is no inter-tier hop. Mount
your Ceph config + keyring:

```bash
podman run --rm --name nkv --privileged \
  --ulimit memlock=-1 \
  -v /var/run/muser:/var/run/muser \
  -v /etc/ceph:/etc/ceph:ro \
  -v /dev/hugepages:/dev/hugepages \
  -e NKV_ROLE=nkv \
  -e BACKEND=rados \
  -e POOL=kvpool -e CEPH_NS=kvns \
  -e NVMF_TGT_ARGS="-m 0x1 -s 4096" \
  quay.io/mmgaggle/rados-nkv:devel
```

### Two-tier (front + remote executor)

Split the tenant edge from the computation: a front local to the GPU/client and a
`rados-nkvx` executor co-located with the OSDs, linked by an RPC over Mercury
(RDMA in production; `ofi+tcp` / `na+sm` for bring-up). See
[`docs/two-tier.md`](docs/two-tier.md) for the topology and transport rules.

1. **On the OSD host — the executor.** It cold-fills inputs from local RADOS and
   runs the wasm module, publishing its Mercury self-address to `NKVX_ADDR_FILE`:

   ```bash
   podman run --rm --name nkvx --privileged \
     --net host --ulimit memlock=-1 --device /dev/infiniband \
     -v /etc/ceph:/etc/ceph:ro \
     -v /var/run/rados-nkvx:/var/run/rados-nkvx \
     -e NKV_ROLE=nkvx \
     -e NKVX_LISTEN="ofi+verbs;ofi_rxm://0.0.0.0:1234" \
     -e NKVX_ADDR_FILE=/var/run/rados-nkvx/addr \
     -e NKVX_RADOS_POOL=kvpool -e NKVX_RADOS_NAMESPACE=kvns \
     -e NKVX_RADOS_CONF=/etc/ceph/ceph.conf -e NKVX_RADOS_USER=admin \
     quay.io/mmgaggle/rados-nkv:devel
   ```

   The `verbs` transport needs the RDMA device exposed to the container and raised
   memlock. For a non-RDMA bring-up use `NKVX_LISTEN="ofi+tcp://0.0.0.0:1234"`
   (`na+sm://`, the default, is host-local shared memory only).

2. **On the GPU/client host — the front**, pointed at the executor's reachable
   self-address via `REMOTE_EXECUTOR` (use the exact string the executor published
   to `NKVX_ADDR_FILE` / logged at startup):

   ```bash
   podman run --rm --name nkv --privileged \
     --net host --ulimit memlock=-1 --device /dev/infiniband \
     -v /var/run/muser:/var/run/muser \
     -v /etc/ceph:/etc/ceph:ro \
     -v /dev/hugepages:/dev/hugepages \
     -e NKV_ROLE=nkv \
     -e BACKEND=rados \
     -e POOL=kvpool -e CEPH_NS=kvns \
     -e REMOTE_EXECUTOR="ofi+verbs;ofi_rxm://10.0.0.20:1234" \
     -e NVMF_TGT_ARGS="-m 0x1 -s 4096" \
     quay.io/mmgaggle/rados-nkv:devel
   ```

The front terminates the tenant's NVMe-KV Exec and forwards it to the executor,
which runs the module next to the data and returns only the result.

### Host devices, hugepages, and the RDMA NIC

The flags above are a **starting point, not yet validated end-to-end in a
container** — the exact set depends on your host (RDMA driver, IOMMU, hugepage
setup), so expect to tune them:

- **Hugepages** (front, unless `NVMF_TGT_ARGS="--no-huge …"`): allocate them on
  the host (e.g. `echo 2048 > /proc/sys/vm/nr_hugepages` or a boot arg), mount
  `-v /dev/hugepages:/dev/hugepages`, and raise `--ulimit memlock=-1`. SPDK's
  `-s <MiB>` must fit what the host provides.
- **RDMA NIC** (verbs transport, both ends): pass the verbs char devices —
  `--device /dev/infiniband` (or the specific `/dev/infiniband/uverbs*` +
  `/dev/infiniband/rdma_cm`), run `--net host` so the device's GIDs/ports are
  reachable, and `--ulimit memlock=-1` for registered memory. On some stacks you
  also need `--device /dev/dri` / `--group-add` for the right device group.
- **GPU P2P / dma-buf result delivery** (front, GPU-resident buffers): add the
  GPU devices (`/dev/kfd` + `/dev/dri` for ROCm) and the dma-buf-capable kernel
  bits per [`docs/two-tier.md`](docs/two-tier.md).
- `--privileged` is the blunt instrument used here; for production narrow it to
  the specific `--cap-add` / `--device` / `--security-opt` your platform needs.

Validating these container invocations on the target RDMA host is tracked
separately — treat the commands above as the documented contract for the
entrypoint's env knobs, with host plumbing to be confirmed per deployment.

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
