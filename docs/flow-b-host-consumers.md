# Flow B — Host-side consumers over the same NVMe-KV target

Two production consumers ride the identical SPDK NVMe-KV-on-RADOS controller as
Flow A, but from a host process over a **vfio-user loopback** (no VM, no
network) — both linking the in-process SPDK **KV host shim**
(`spdk/test/nvmf/kv_shim/kv_host_shim.{c,h}`).

Stand up the target they attach to with [`scripts/rados-nkv up`](../scripts/rados-nkv)
(add `--read-only` for the weights loader namespace, or `--mem` for a no-Ceph dev
loop); the printed `vfu_addr` is what `vfu_addr` / `NvmeKvClient` take below.

![rados-nkv substrate](diagrams/rados-nkv.png)

(Source: [`diagrams/rados-nkv.mmd`](diagrams/rados-nkv.mmd))

## Consumer #1 — NIXL `RADOS_NKV` backend (`nixl@rados-nkv`)

A NIXL South-Bound storage backend
([`nixl/src/plugins/rados-nkv/`](../clients/nixl/src/plugins/rados-nkv)) that maps NIXL
transfers onto the NVMe KV command set — the **llm-d KV-cache offload**
transport.

| NIXL op | Local / remote mem | NVMe KV command |
|---------|--------------------|-----------------|
| `NIXL_WRITE` | `DRAM_SEG` → `OBJ_SEG` | KV **Store** |
| `NIXL_READ` | `OBJ_SEG` → `DRAM_SEG` | KV **Retrieve** |
| `queryMem` (lookup) | `OBJ_SEG` | KV **Exist** (hit/miss mask, no value transfer) |

**Key derivation.** The remote `OBJ_SEG` descriptor's `metaInfo` blob carries a
token sequence. The engine hashes it into the fixed-length NVMe KV key as a
128-bit FNV-1a digest truncated to `min(16, kvkml)` (`radosNkvDeriveKey`, in
`rados_nkv_key.cpp`). An over-length key is **rejected**, never truncated
(truncation would alias distinct keys). `queryMem` distinguishes present
(engaged response) / absent (`std::nullopt`) / transport error
(`NIXL_ERR_BACKEND`) — a miss is never masked as an error or vice versa.

**Backend params** (`nixl_b_params_t`): `vfu_addr` (required — the VFIOUSER
domain directory), `nsid` (optional; `0` auto-selects the first KV namespace),
`init_env` (optional; `false` = the host/agent owns the SPDK env).

### Building & testing

```bash
cd clients/nixl
meson setup build \
    -Dspdk_root=$PWD/../spdk \
    -Dspdk_kv_shim_dir=$PWD/../spdk/test/nvmf/kv_shim \
    -Drados_nkv_build_test=true
ninja -C build
```

- **Unit (no SPDK):** `test/gtest/unit/rados-nkv/test_rados_nkv_key.cpp` covers
  the key derivation; runs in the standard `ninja -C build test` suite.
- **End-to-end (needs SPDK):** the plugin ships two bring-up scripts that stand
  up an `nvmf_tgt` KV namespace over VFIOUSER and run the round-trip test:
  - [`run_roundtrip.sh`](../clients/nixl/src/plugins/rados-nkv/run_roundtrip.sh) — in-memory `kvdev_mem`.
  - [`run_roundtrip_rados.sh`](../clients/nixl/src/plugins/rados-nkv/run_roundtrip_rados.sh) — librados-backed; also asserts the value lands as a Ceph object.

Full reference: [`nixl/src/plugins/rados-nkv/README.md`](../clients/nixl/src/plugins/rados-nkv/README.md).

## Consumer #2 — vllm-weights (`clients/vllm-weights`)

A **model-weights catalog** over one shared, **read-only** NVMe-KV namespace
that acts as a directory — serving immutable model weights to a GPU fleet with
no filesystem, mount, or object-storage credentials in the guest.

### The catalog model

Everything is addressed by raw **16-byte keys**; there is no `List` (deferred on
the librados backend), so discovery is by *deterministic key*.

- **Chunk key** = `blake2b(chunk_bytes, 16)` — a content hash, so identical bytes
  across revisions / precisions / LoRA adapters / models are stored **once**
  (dedup, no refcount table). Each chunk is ≤ the librados max value length.
- **Manifest key** = `blake2b(b"manifest:" + model_revision, 16)` — deterministic,
  so a freshly attached host finds the manifest with no enumeration.
- **Weight manifest** — an Arrow IPC table, one row per tensor, carrying
  `tensor_name`, `shape`, and per precision variant the ordered chunk keys +
  sizes. The safetensors-header analog.

### Read/write split (mirrors the target's per-namespace gate)

- **Publisher** (write path) — runs privileged on an **admin** NVMe-KV
  connection permitted to `Store`. Chunks tensors, Stores each chunk under its
  Chunk key (skipping ones that already `Exist`), writes the manifest.
- **Loader** (read path) — runs unprivileged on every GPU host against a
  **read-only** namespace (`Retrieve`/`Exist` only). Retrieves the manifest by
  Manifest key, then the tensor's chunks, and reassembles.

This is enforced both client-side (`NvmeKvClient.open_loader` exposes only
`retrieve`/`exists`; `open_publisher` adds `store`) **and** target-side (the
read-only namespace in `ctrlr_kvdev.c` rejects `Store`/`Delete`/`Exec`).

### The native transport

`NvmeKvClient` (`rados_nkv_weights.nvmekv_client`) is a stdlib-only ctypes
wrapper over a `.so` built from the **same** SPDK `kv_host_shim.c`:

```bash
cd clients/vllm-weights
SPDK_ROOT=$PWD/../spdk ./native/build.sh   # -> native/libradosnkv_kvshim.so
```

```python
from rados_nkv_weights.nvmekv_client import NvmeKvClient
from rados_nkv_weights import publish, load

with NvmeKvClient.open_publisher(vfu_addr) as kv:   # admin write path
    publish(kv, "org/model@rev", tensors)
with NvmeKvClient.open_loader(vfu_addr) as kv:      # read-only loader path
    weights = load(kv, "org/model@rev", "fp16")
```

`vfu_addr` is the VFIOUSER transport address (the directory holding the
controller socket) of an NVMe-KV target with a KV namespace — the same address
the NIXL plugin's `vfu_addr` takes.

A dev loop with no SPDK/Ceph at all uses the dict-backed `InMemoryKvClient`.

Full reference: [`clients/vllm-weights/README.md`](../clients/vllm-weights/README.md).

## Why both share one substrate

NIXL KV-cache offload and weights distribution are **opposite lifecycles** —
hot, mutable attention cache vs. cold, immutable weights — but they share the
*transport*: the same `kv_host_shim.c`, the same CSI=KV controller, the same
`kvdev_rados` backend, the same `vfu_addr`. The namespace is backend-agnostic,
so both run unchanged against `kvdev_mem` (dev) or `kvdev_rados` (real Ceph).
