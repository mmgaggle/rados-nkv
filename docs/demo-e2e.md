# End-to-end demo walkthrough

This is the full bring-up that ties every submodule together. It assumes all
components are built (see [`build.md`](build.md)) and you have a Ceph cluster
(ie. `vstart`-style RADOS) reachable via `ceph.conf` + keyring.

The single shared anchor across both flows is the **SPDK NVMe-KV-on-RADOS
target**. Stand it up once; then drive it from the GPU (Flow A) and/or from host
consumers (Flow B).

## Step 0 — A throwaway Ceph cluster + pool

Bring up a `vstart` cluster from the `ceph` submodule (built per
[`build.md`](build.md) §0), then create the pool the KV namespace lands in:

```bash
cd ceph/build
MON=1 OSD=3 MGR=1 ../src/vstart.sh -n -d --without-dashboard
export CEPH_CONF=$PWD/ceph.conf            # config + keyring vstart just wrote

bin/ceph osd pool create kvpool
# values land as objects in pool=kvpool, namespace=kvns
```

`vstart.sh` writes a `ceph.conf` and admin keyring into `ceph/build/` — that's
the config/keyring the SPDK `kvdev_rados_register_cluster` call in Step 1 points
at. Tear the cluster down afterward with `../src/stop.sh`.

> Skipping Ceph? Replace the `kvdev_rados_*` RPCs in Step 1 with
> `kvdev_mem_create KvMem0` and drop this step — the in-memory backend needs no
> cluster, and Steps 2–4 are otherwise unchanged.

## Step 1 — SPDK NVMe-KV target (the shared substrate)

Start `nvmf_tgt` from the SPDK build, then bring the KV-on-RADOS target up with
the [`scripts/rados-nkv`](../scripts/rados-nkv) wrapper:

```bash
sudo spdk/build/bin/nvmf_tgt &        # the SPDK target (from the spdk submodule)

scripts/rados-nkv up                  # picks up $CEPH_CONF from Step 0;
                                      # pool=kvpool, namespace=kvns by default
# → KV target up at /var/run/muser/domain/kv/0
```

`vfu_addr` for the host consumers (Steps 3–4) is the listener directory printed
above, `/var/run/muser/domain/kv/0`. Tear it down later with
`scripts/rados-nkv down`.

Under the hood `up` runs the SPDK JSON-RPC sequence — `nvmf_create_transport`
(VFIOUSER), `kvdev_rados_register_cluster` + `kvdev_rados_create`,
`nvmf_create_subsystem`, `nvmf_subsystem_add_kv_ns` (CSI=KV), and
`nvmf_subsystem_add_listener`. Useful variants:

```bash
scripts/rados-nkv up --read-only      # loader-style namespace (Retrieve/Exist only)
scripts/rados-nkv up --mem            # in-memory kvdev — skip Step 0, no Ceph needed
scripts/rados-nkv status              # show subsystems / rados clusters
scripts/rados-nkv help                # all options + env-var defaults
```

> For a no-Ceph dev run, `scripts/rados-nkv up --mem` swaps the `kvdev_rados_*`
> calls for `kvdev_mem_create` — Steps 2–4 are otherwise unchanged.

## Step 2 — Flow A: GPU-initiated Store + Retrieve

Launch the GPU-passthrough guest with the QEMU `pci-mmio-bridge` device wired to
the host SPDK NVMe controller, then inside the guest:

```bash
# GPU __device__ code issues a KV Store, then a KV Retrieve into VRAM
xio-tester nvme-ep --controller /dev/nvme0 \
    --kv-op store    --key gpukey01 --value-size 4096 --write-io 1 --pci-mmio-bridge
xio-tester nvme-ep --controller /dev/nvme0 \
    --kv-op retrieve --key gpukey01 --value-size 4096 --read-io 1 \
    --memory-mode 8 --pci-mmio-bridge          # value lands in GPU VRAM
```

Verify the object reached Ceph from the host:

```bash
rados -p kvpool -N kvns stat $(printf 'gpukey01' | xxd -p)
```

See [`flow-a-gpu-initiated.md`](flow-a-gpu-initiated.md) and the bundled
[`rocm-xio/examples/stage2_kv_rados_gpu.sh`](../clients/rocm-xio/examples/stage2_kv_rados_gpu.sh).

## Step 3 — Flow B: NIXL round-trip over the same target

```bash
cd nixl
# point the agent at the listener dir from Step 1
NIXL_RADOS_NKV_VFU_ADDR=/var/run/muser/domain/kv/0 \
    ./src/plugins/rados-nkv/run_roundtrip_rados.sh
```

The script Stores via `NIXL_WRITE`, Retrieves via `NIXL_READ`, probes with
`queryMem` (KV Exist), and asserts the value is a Ceph object. See
[`flow-b-host-consumers.md`](flow-b-host-consumers.md).

## Step 4 — Flow B: publish + load model weights

```bash
cd clients/vllm-weights && . .venv/bin/activate
python - <<'PY'
from rados_nkv_weights.nvmekv_client import NvmeKvClient
from rados_nkv_weights import publish, load
vfu = "/var/run/muser/domain/kv/0"
with NvmeKvClient.open_publisher(vfu) as kv:            # admin write path
    publish(kv, "demo/model@rev1", {
        "w": {"fp16": ("float16", (1024,), b"\xab" * 2048)},
    })
with NvmeKvClient.open_loader(vfu) as kv:               # read-only loader path
    tensors = load(kv, "demo/model@rev1", "fp16")
    assert tensors["w"] == b"\xab" * 2048
print("weights round-trip OK")
PY
```

## What the demo proves

- A GPU with **no host CPU in the data loop** persists and fetches values to
  Ceph over NVMe-KV, value landing in VRAM (Flow A).
- The **same** controller and `kvdev_rados` backend serve production host
  consumers — llm-d KV-cache offload via NIXL, and model-weights distribution
  (Flow B).
- The KV namespace is backend-agnostic: identical client code runs against
  `kvdev_mem` (dev) and `kvdev_rados` (real Ceph).
- The per-namespace **read-only / admin** capability split is enforced
  target-side, so a loader fleet can multi-attach a catalog it cannot mutate.

## Troubleshooting pointers

- Target won't enumerate as KV: confirm the namespace was added with
  `nvmf_subsystem_add_kv_ns` (CSI=KV), not a block ns.
- Host consumer can't find the namespace: check `vfu_addr` is the listener
  directory and `nsid=0` (auto-select) or the real KV nsid.
- `Store` rejected: the namespace is read-only — use an admin/publisher
  connection.
- GPU doorbell not reaching the BAR: verify the QEMU `pci-mmio-bridge` device is
  attached and `--pci-mmio-bridge` is passed to `xio-tester`.
