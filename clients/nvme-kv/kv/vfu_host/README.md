# QEMU-less GPU-initiated NVMe-KV client (`vfu_host`)

A host-native client that drives an SPDK vfio-user NVMe-KV controller **without
QEMU and without `lib/nvme`**. It speaks the vfio-user protocol via SPDK's raw
`lib/vfio_user`, owns its own NVMe admin + IO queues, and — in the GPU variant —
a `gfx1151` HIP kernel builds the NVMe SQE and rings the doorbell directly
(the rocm-xio direct `nvmeBar0Gpu` path, minus the `pci-mmio-bridge`).

Backend-agnostic: the same client drives an in-memory kvdev or the two-tier
`rados-nkv` / `rados-nkvx` target (RADOS + a remote Mercury/RDMA executor)
unchanged — the kvdev abstraction makes the backend transparent.

## Files

| File | Purpose |
|------|---------|
| `nkv_vfu.h` | shared driver: attach, admin/IO queues, controller enable, Store/Retrieve/Exec, SGL |
| `nkv_vfu_host.c` | CPU client (builds via the SPDK `make`) |
| `nkv_vfu_gpu.hip` | GPU client — kernels build the SQE + ring the doorbell |
| `build_gpu.sh` | builds `nkv_vfu_gpu` with `hipcc` + the SPDK static libs |
| `nkvx-up.sh` / `nkvx-down.sh` | bring the two-tier rados-nkvx target up (left running) / down |
| `run_tests.sh` | regression: brings up the target, runs the full op matrix, verifies in RADOS |

## Build

```bash
make -C .            # CPU client (nkv_vfu_host)
bash build_gpu.sh    # GPU client (nkv_vfu_gpu)
```

## Runbook

Prereqs (once per boot): the RDMA loopback network and a Ceph vstart cluster:
```bash
sudo /home/kyle/nkvx-repro/rdma-loopback-validate.sh setup      # 10.110.0.1 / 10.110.0.2 + netns
cd /home/kyle/src/ceph/build && MON=1 OSD=3 ../src/vstart.sh --without-dashboard   # REUSE (no -n!)
```

Bring the target up, run GPU ops, tear down:
```bash
./nkvx-up.sh                              # prints TRADDR=/tmp/nkvx/muser/0
TR=/tmp/nkvx/muser/0

./nkv_vfu_gpu $TR store    <key> <value>          # GPU-initiated KV Store
./nkv_vfu_gpu $TR retrieve <key>                  # GPU-initiated KV Retrieve
./nkv_vfu_gpu $TR exec     <key> <op_id>          # GPU-initiated KV Exec (10=bytecount, 11=identity)
./nkv_vfu_gpu $TR store-batch <prefix> <N>        # N Stores in ONE wavefront + ONE doorbell
./nkv_vfu_gpu $TR exec-batch  <prefix> <op_id> <N># N Execs in ONE wavefront (two-tier)
./nkv_vfu_gpu $TR store-big   <key> <size>        # GPU produces+stores a big value (e.g. 256K, 2M), verify

./nkvx-down.sh
```

RADOS-side verification (object name = hex of the key):
```bash
rados() { LD_LIBRARY_PATH=$CEPH/build/lib $CEPH/build/bin/rados -c $CEPH/build/ceph.conf -p kvpool "$@"; }
H=$(printf '%s' <key> | xxd -p); rados ls | grep $H; rados get $H -
```

Or just run the regression: `./run_tests.sh`.

## Notes / known limits

- The GPU does one-time controller bring-up on the CPU, then the GPU kernel rings
  the doorbell for each op (or one doorbell for a whole wavefront in the `-batch` modes).
- `store-big` (GPU **produces** the bulk via a kernel) is byte-exact through **511 pages
  (~2 MiB)** — confirming the spdk-5co producer↔doorbell coherence cliff is gone on bare
  metal. ≥512 pages currently fail in the **target's** transfer mapping (not the client;
  SGL and PRP fail identically). See `bd memories gpu-store-big-2mb-prp-limit-needs-sgl`.
- Load-bearing constraints baked into the driver: `--iova-mode=va` + VA-as-IOVA, PCI
  INTx-disable, and (for the two-tier front) unlimited memlock for the verbs RDMA channel.
