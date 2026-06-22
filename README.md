
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
> endpoint the front creates under `VFU_DIR` (default `/var/run/muser/domain/kv`),
> where libvfio-user listens on a `cntrl` UNIX socket — so that directory must be
> shared between the front container and the client. Under SELinux add `:z` to the
> bind mount so the container can write it. SPDK also needs hugepages — or
> `NVMF_TGT_ARGS="--no-huge -s 1024"` on a dev box — plus the privileges to set
> up vfio-user; the examples use `--privileged` for brevity (narrow to the
> specific caps + `-v /dev/hugepages` for production). See
> [`docs/demo-e2e.md`](docs/demo-e2e.md).
>
> The mem-backend and `ofi+tcp` flows below run under **rootless** podman; the
> **`ofi+verbs` (RDMA) flow requires rootful podman** — see
> [Host devices, hugepages, and the RDMA NIC](#host-devices-hugepages-and-the-rdma-nic).

### Quick start (podman compose)

[`compose.yaml`](compose.yaml) wires the stack on one host. Run rootful — SPDK
needs hugepages + vfio-user privileges:

```bash
# single-tier, self-contained: nkv (mem backend) + the rkv client
sudo podman-compose up -d
sudo podman-compose exec rkv rkv store kv/hello   # value from stdin
sudo podman-compose exec rkv rkv get   kv/hello
sudo podman-compose down

# two-tier on one host: front + nkvx executor over ofi+tcp, against a Ceph
# cluster (e.g. a vstart build dir via CEPH_DIR)
sudo CEPH_DIR=/path/to/ceph NKV_BACKEND=rados \
     REMOTE_EXECUTOR=ofi+tcp://127.0.0.1:1234 NKVX_LISTEN=ofi+tcp://127.0.0.1:1234 \
     podman-compose --profile two-tier up -d
```

The `rkv` service idles, so you run a series of commands via
`podman-compose exec rkv rkv …` (set `RKV_DAEMON=1` for a warm session that
amortizes the vfio-user attach). For the `ofi+verbs` (RDMA) inter-tier transport,
run rootful and add the RDMA device + memlock flags (see
[Host devices…](#host-devices-hugepages-and-the-rdma-nic)).

> **Gotcha:** podman-compose reuses existing containers across `up`, so a newly
> pulled/built image is *not* picked up by `down`/`up` alone — `podman rm -f` the
> services (or `podman-compose down` then remove them) to force a recreate.

Compose is single-host. For a **cross-host** two-tier split (front on the GPU
host, executor on a separate OSD host) use the per-container commands below — the
same env-knob contract, just on two machines.

### Target with the in-memory backend (no Ceph) — without compose

A dev loop with no cluster — the `kvdev_mem` backend keeps values in the target's
RAM:

```bash
podman run --rm --name nkv --privileged \
  -v /var/run/muser:/var/run/muser:z \
  -e NKV_ROLE=nkv \
  -e BACKEND=mem \
  -e NVMF_TGT_ARGS="--no-huge -s 1024" \
  quay.io/mmgaggle/rados-nkv:devel
```

The KV controller is then reachable at the vfio-user endpoint dir
`/var/run/muser/domain/kv` (libvfio-user's `cntrl` socket). Validated rootless on
a SELinux host; the `:z` relabel on the mount is required.

### Single-tier (rados backend, in-process Exec)

The whole stack in one process: the front reads/writes RADOS via `librados`, and
Exec runs in-process. No `REMOTE_EXECUTOR`, so there is no inter-tier hop. Mount
your Ceph config + keyring:

```bash
podman run --rm --name nkv --privileged \
  --ulimit memlock=-1 \
  -v /var/run/muser:/var/run/muser:z \
  -v /etc/ceph:/etc/ceph:ro,z \
  -v /dev/hugepages:/dev/hugepages \
  -e NKV_ROLE=nkv \
  -e BACKEND=rados \
  -e POOL=kvpool -e CEPH_NS=kvns \
  -e NVMF_TGT_ARGS="-m 0x1 -s 4096" \
  quay.io/mmgaggle/rados-nkv:devel
```

### Two-tier (front + remote executor) — cross-host

Split the tenant edge from the computation: a front local to the GPU/client and a
`rados-nkvx` executor co-located with the OSDs, linked by an RPC over Mercury
(RDMA in production; `ofi+tcp` / `na+sm` for bring-up). See
[`docs/two-tier.md`](docs/two-tier.md) for the topology and transport rules. (For
a single-host two-tier demo, use the compose `two-tier` profile above instead.)

> **The `ofi+verbs` (RDMA) path needs rootful podman.** Rootless podman caps
> memlock at the user's hard limit (commonly 8 MB), too small for RDMA memory
> registration — `HG_Init` fails. Run the verbs front and executor as root
> (`sudo podman`, where `--ulimit memlock=-1` is truly unlimited) with
> `--device /dev/infiniband --net host --security-opt label=disable` (SELinux
> otherwise blocks the `uverbs*`/`rdma_cm` nodes). An `ofi+tcp` bring-up runs
> rootless. See [Host devices…](#host-devices-hugepages-and-the-rdma-nic).

1. **On the OSD host — the executor.** It cold-fills inputs from local RADOS and
   runs the module, publishing its Mercury self-address to `NKVX_ADDR_FILE`:

   ```bash
   sudo podman run --rm --name nkvx \
     --net host --device /dev/infiniband --ulimit memlock=-1 \
     --security-opt label=disable \
     -v /etc/ceph:/etc/ceph:ro \
     -v /var/run/rados-nkvx:/var/run/rados-nkvx \
     -e NKV_ROLE=nkvx \
     -e NKVX_LISTEN="ofi+verbs;ofi_rxm://<roce-ip>:1234" \
     -e NKVX_ADDR_FILE=/var/run/rados-nkvx/addr \
     -e NKVX_RADOS_POOL=kvpool -e NKVX_RADOS_NAMESPACE=kvns \
     -e NKVX_RADOS_CONF=/etc/ceph/ceph.conf -e NKVX_RADOS_USER=admin \
     quay.io/mmgaggle/rados-nkv:devel
   ```

   Bind `NKVX_LISTEN` to the RDMA interface IP (e.g. `10.110.0.1`), not
   `0.0.0.0`. For a non-RDMA bring-up use `NKVX_LISTEN="ofi+tcp://<ip>:1234"`
   (rootless ok); `na+sm://` (the default) is host-local shared memory only.

2. **On the GPU/client host — the front**, pointed at the executor's reachable
   self-address via `REMOTE_EXECUTOR` (use the exact string the executor published
   to `NKVX_ADDR_FILE` / logged at startup):

   ```bash
   sudo podman run --rm --name nkv \
     --net host --device /dev/infiniband --ulimit memlock=-1 \
     --security-opt label=disable \
     -v /var/run/muser:/var/run/muser \
     -v /etc/ceph:/etc/ceph:ro \
     -v /dev/hugepages:/dev/hugepages \
     -e NKV_ROLE=nkv \
     -e BACKEND=rados \
     -e POOL=kvpool -e CEPH_NS=kvns \
     -e REMOTE_EXECUTOR="ofi+verbs;ofi_rxm://10.110.0.1:1234" \
     -e NVMF_TGT_ARGS="-m 0x1 -s 4096" \
     quay.io/mmgaggle/rados-nkv:devel
   ```

The front terminates the tenant's NVMe-KV Exec and forwards it to the executor,
which runs the module next to the data and returns only the result.

### Host devices, hugepages, and the RDMA NIC

Validated end-to-end in containers on a RoCE host (kernel 6.19, `rocep101s0f0`,
podman 5.8). What each path actually needs:

- **Hugepages** (front, unless `NVMF_TGT_ARGS="--no-huge …"`): allocate on the
  host (`echo 2048 > /proc/sys/vm/nr_hugepages` or a boot arg), mount
  `-v /dev/hugepages:/dev/hugepages`, and raise `--ulimit memlock=-1`. SPDK's
  `-s <MiB>` must fit what the host provides. The mem backend with `--no-huge`
  needs neither and runs rootless.
- **SELinux:** bind mounts the container writes (the `muser` endpoint dir,
  `/etc/ceph`) need `:z`. The RDMA char devices need `--security-opt
  label=disable` — relabeling `--device` nodes isn't enough; without it
  `/dev/infiniband/uverbs*` are inaccessible and `ibv_devices` is empty.
- **RDMA NIC** (verbs, both ends): **rootful podman** — rootless caps memlock at
  the user's hard limit (commonly 8 MB), too small for RDMA MR registration, so
  `HG_Init` fails. Use `sudo podman` with `--device /dev/infiniband --net host
  --ulimit memlock=-1 --security-opt label=disable`, and bind the listen/target
  to the RoCE IP. An `ofi+tcp` transport avoids RDMA and runs rootless. (To stay
  rootless on verbs you'd instead raise the memlock hard limit via
  `/etc/security/limits.d` — see `rados-nkvx/deploy/99-nkvx-memlock.conf` — and
  re-login.)
- **SR-IOV:** the RoCE PF advertises VFs (`sriov_numvfs`); a VF per container is
  the production isolation path (each tier its own RDMA function). Not required
  for a single-host bring-up.
- **GPU P2P / dma-buf result delivery** (front, GPU-resident buffers): add the
  GPU devices (`/dev/kfd` + `/dev/dri`, `--group-add video`) and the
  dma-buf-capable kernel bits per [`docs/two-tier.md`](docs/two-tier.md).
- `--privileged` is the blunt instrument used in the simpler examples; for
  production narrow it to the specific `--cap-add` / `--device` / `--security-opt`
  your platform needs.

The `rkv` Rust client ships as a **separate** image
(`quay.io/mmgaggle/rados-nkv-client`, kept out of the lean datapath image); it
attaches to the front's `cntrl` socket over vfio-user.

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
