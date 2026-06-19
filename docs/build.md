# Building the stack

Everything links against a built **SPDK** tree, so SPDK is built first. SPDK's
`rados` kvdev backend links **`librados`** from the `ceph` submodule, so build
(or install) Ceph before SPDK if you want the real backend. The leaf components
are independent of each other.

```
   ┌─────────┐
   │  ceph   │  (optional — vstart cluster + librados for SPDK's rados backend)
   └────┬────┘
        ▼ librados
   ┌─────────────┐
   │  spdk       │  (build next — provides libspdk_nvme, kvdev modules,
   └──────┬──────┘   and kv_host_shim.{c,h})
        ┌───────────────┼───────────────┬──────────────────┐
        ▼               ▼               ▼                  ▼
   ┌─────────┐    ┌──────────┐   ┌───────────────┐   ┌──────────┐
   │ rocm-xio│    │  nixl    │   │ rados-nkv-    │   │  qemu    │
   │ nvme-ep │    │RADOS_NKV │   │ weights       │   │ pci-mmio │
   │ (Flow A)│    │ (Flow B) │   │ (Flow B)      │   │ -bridge  │
   └─────────┘    └──────────┘   └───────────────┘   └──────────┘
```

## Orchestrated build (recommended)

A CMake superbuild ([`CMakeLists.txt`](../CMakeLists.txt)) drives every
component's native build in the dependency order above, fronted by a top-level
[`Makefile`](../Makefile):

```bash
make init      # fetch all submodules (init-<component> for just one)
make build     # build everything, in order (build-<component> for just one)
```

Per-component and helper targets:

```bash
make build-spdk          # one component: ceph spdk rocm-xio qemu nixl weights
make init-nixl           # init a single submodule
make vstart   /  stop    # bring a throwaway Ceph cluster up / down
make up / down / status  # the SPDK NVMe-KV target (wraps scripts/rados-nkv)
make vm                  # build the ROCm + rocm-xio guest image (Flow A)
make vm-run              # launch the guest (proven pci-mmio-bridge bring-up)
make vm-run-spdk         # launch wired to the SPDK NVMe-KV target
make vm-vfio-rules       # install VFIO udev rules (may need sudo)
make deps-ceph           # run ceph/install-deps.sh (may need sudo)
make distclean           # remove build/
```

Build a subset by toggling components at configure time (then build):

```bash
make configure CMAKE_ARGS='-DWITH_QEMU=OFF -DWITH_CEPH=OFF'
make build
# re-run `make distclean` before changing toggles on an existing build/
```

Other knobs (CMake cache vars): `JOBS` (parallelism), `SPDK_CONFIGURE_OPTS`
(default `--with-rbd`), `ROCM_XIO_PRESET` (default `release`), `CEPH_CMAKE_OPTS`.
`make build` is self-sufficient — each component target depends on its
`init-<component>`, so a bare `make build` will fetch what it needs — but running
`make init` first makes the submodule fetch an explicit, separate step.

The rest of this document is the **manual** equivalent: what each `make` target
runs under the hood, and the authoritative per-component build docs
(`spdk/README.md`, `rocm-xio/INSTALL.md`, `nixl/README.md`,
`rados-nkv-weights/README.md`, QEMU's `docs/`). Populate submodules manually with:

```bash
git submodule update --init --recursive   # or per-component, listed in README.md
```

## 0. Ceph — the cluster (optional, for the `rados` backend)

Skip this and use SPDK's in-memory `kvdev_mem` backend for a dev loop that needs
no cluster. For real Ceph, build Ceph once and stand up a throwaway cluster with
`vstart`:

```bash
cd ceph
git submodule update --init        # Ceph's own submodules
./install-deps.sh
./do_cmake.sh -DWITH_RBD=ON
cd build && ninja -j"$(nproc)" vstart    # builds the daemons + librados/librbd
```

This produces `lib/librados.so*` and headers that SPDK's `--with-rbd` configure
links against. Bringing the cluster *up* is a runtime step — see
[`demo-e2e.md`](demo-e2e.md) Step 0 (`MON=1 OSD=3 ../src/vstart.sh -n -d`).

> Already have Ceph installed system-wide (packages, or another cluster reachable
> via `ceph.conf` + keyring)? You can skip building the `ceph` submodule
> entirely — SPDK just needs `librados` to link and a config/keyring at runtime.

## 1. SPDK — the substrate

```bash
cd spdk
git submodule update --init        # SPDK's own submodules (dpdk, isa-l, ...)
./configure --with-rbd             # librbd/librados for the rados kvdev backend
make -j"$(nproc)"
```

`--with-rbd` needs `librados`/`librbd` discoverable — from the `ceph` submodule
build above, system packages, or a Ceph dev install.

This yields `build/lib/libspdk_nvme.a` and friends and the `kvdev_mem` /
`kvdev_rados` modules — the SPDK inputs every other component links against. The
KV client/test harness (incl. `kv_host_shim.{c,h}`) now lives in `clients/` (see
step 1b), no longer inside the SPDK tree.

## 1b. clients — KV host/test harness (Flow B substrate)

The standalone NVMe-KV hosts, the reusable `kv_host_shim`, the `rados-nkv` Rust
CLI, the vfio-user host, and the WASM Exec modules live in `clients/` and build
**against the spdk submodule from step 1** (their Makefiles set
`SPDK_ROOT_DIR := $(CURDIR)/../../spdk`). Build SPDK first, then:

```bash
make -C clients/kv_shim     # kv_shim_test (+ libfied kv_host_shim)
make -C clients/kv          # kv_host, kv_ro_host
make -C clients/kv_rados    # kv_rados_host
make -C clients/kv/vfu_host # nkv_vfu_host (raw vfio-user client)

# Rust CLI (CPU default; gpu-native feature pulls in hipcc/ROCm). Defaults to the
# spdk submodule build; override with NKVX_SPDK_BUILD=<spdk>/build.
cargo build --release --manifest-path clients/kv/rados-nkv/Cargo.toml
```

## 1c. rados-nkvx — the standalone Exec executor (storage side)

The restartable wasmtime-sandbox executor (`rados-nkvx/`) is **non-SPDK by
design** (ADR-0009) and links Mercury + librados, not SPDK libs. It reuses the
Exec RPC contract + wasm core + `wasmtime/include` from the spdk submodule's
`module/kvdev/rados/` (those stay in the fork — the in-tree `kvdev_rados` module
compiles them too). Needs the Mercury install from `$SPDK_ROOT/vendor/`.

```bash
make -C rados-nkvx                              # nkvx_service, nkvx_exec_client
# overrides: SPDK_ROOT=<spdk>  MERCURY_PREFIX=<mercury>  ASAN=1
```

## 2. rocm-xio — Flow A (GPU-initiated)

Needs ROCm/HIP and a supported AMD GPU (gfx1151). See `rocm-xio/INSTALL.md` for
dependencies and supported hardware.

```bash
cd rocm-xio
cmake --preset <preset>            # see CMakePresets.json
cmake --build build -j"$(nproc)"
```

Produces `xio-tester` with the `nvme-ep --kv-op` path. KV is an additive flag —
no extra build options beyond the standard rocm-xio build.

## 3. qemu — the pci-mmio-bridge (Flow A)

Build the `pci-mmio-bridge` QEMU from the
`dev/stephen/pci-mmio-bridge-submit` branch:

```bash
cd qemu
./configure --target-list=x86_64-softmmu --enable-kvm
make -j"$(nproc)"
```

Use this `qemu-system-x86_64` to launch the GPU-passthrough guest with the
`pci-mmio-bridge` device wired to the host SPDK NVMe controller.

## 4. nixl — Flow B (RADOS_NKV backend)

Point the build at the SPDK tree from step 1:

```bash
cd nixl
meson setup build \
    -Dspdk_root=$PWD/../spdk \
    -Dspdk_kv_shim_dir=$PWD/../clients/kv_shim \
    -Drados_nkv_build_test=true
ninja -C build
ninja -C build test               # unit suite incl. key-derivation tests
```

The plugin is skipped automatically if `libspdk_nvme.a` / the shim aren't found.

## 5. rados-nkv-weights — Flow B (weights catalog)

Pure-Python core (pyarrow + numpy); the native NVMe-KV transport is an optional
`.so` built against SPDK:

```bash
cd rados-nkv-weights
python -m venv .venv && . .venv/bin/activate
pip install pyarrow numpy && pip install -e .
python -m pytest tests/ -v        # exercises the in-memory client

# Native transport against the real target (optional):
# NOTE: kv_host_shim moved to clients/kv_shim; native/build.sh must locate the
# shim there (e.g. KV_SHIM_DIR=$PWD/../clients/kv_shim) — see the weights repo.
SPDK_ROOT=$PWD/../spdk ./native/build.sh   # -> native/libradosnkv_kvshim.so
```

## Quick dev loop (no Ceph, no GPU)

You can exercise the host consumers end-to-end against the **in-memory** kvdev:

- NIXL: `nixl/src/plugins/rados-nkv/run_roundtrip.sh` (uses `kvdev_mem`).
- weights: `pytest` against `InMemoryKvClient`.

Bring up real Ceph only when you want `kvdev_rados` (e.g. SPDK's `vstart`-style
RADOS, or an existing cluster's `ceph.conf` + keyring).
