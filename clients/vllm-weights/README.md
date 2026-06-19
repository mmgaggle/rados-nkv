# rados-nkv-weights

A **model-weights catalog over an NVMe-KV-on-RADOS namespace**. It serves
immutable model weights to a GPU fleet through *one shared, read-only NVMe-KV
namespace that acts as a catalog* — no filesystem, no mount, no object-storage
credentials. A host attaches the namespace and fetches weights by key.

Design summary: distribute weights via a shared, read-only KV catalog namespace,
addressed by an Arrow per-model manifest + content-hash chunks.

## Two components

| Component | Path | Role |
|-----------|------|------|
| **Weights publisher** (write path) | `rados_nkv_weights.publisher` | Chunks each tensor, Stores each chunk under its content-hash **Chunk key** (skipping any that already **Exist** — dedup), and writes the per-revision **Weight manifest**. Runs privileged, out-of-band, on an **admin NVMe-KV** connection permitted to `Store`. |
| **Weights loader** (read path) | `rados_nkv_weights.loader` | Retrieves the **Weight manifest** by its deterministic **Manifest key**, then Retrieves each tensor's **Chunk keys** in order and reassembles the tensor. Runs unprivileged on every GPU host against a **read-only** namespace (`Retrieve`/`Exist` only). |

## The catalog model

The **Weights catalog** is one NVMe-KV namespace holding many models' weights.
Everything is addressed by raw **16-byte keys**; there is no `List` (it is
deferred on the librados backend), so discovery is by *deterministic key*.

- **Weight chunk** — a slice of one tensor's bytes (at one **Precision
  variant**) no larger than the librados **max value length** (64 MiB cap).
  Tensors exceeding the cap span multiple chunks, listed in order by the
  manifest.
- **Chunk key** — `b"\x01" + blake2b(chunk_bytes, digest_size=15)` (a 1-byte
  domain tag + 15-byte digest, 16 bytes total). A *content hash*: the same bytes
  across **Model revisions** / **Precision variants** / LoRA adapters / models
  hash to the same key and are stored **once** (dedup, no refcount table). See
  `keys.chunk_key`.
- **Manifest key** — `b"\x00" + blake2b(canonical_revision(model_revision), digest_size=15)`.
  A *deterministic* digest of the canonicalized **Model revision** identity, so a
  freshly attached host finds the manifest with **no enumeration**. The distinct
  leading tag byte keeps the manifest and chunk keyspaces structurally disjoint.
  See `keys.manifest_key`.
- **Weight manifest** — an **Arrow IPC** table, one row per tensor, carrying
  `tensor_name`, `shape`, and *per Precision variant `p`* the column group
  `{p}__dtype`, `{p}__keys` (ordered 16-byte Chunk keys), `{p}__sizes`. The
  safetensors-header analog; a host picks one precision column and loads exactly
  that dtype/quantization. See `manifest.WeightManifest`.

### Read/write split

The namespace is asymmetric: loaders attach it **read-only** (`Retrieve`/`Exist`
only; the target rejects `Store`/`Delete`/`Exec`), and the publisher writes via a
privileged **admin NVMe-KV** connection. This mirrors the per-namespace
privilege boundary of the KV-Exec allowlist.

## Quickstart (in-memory client)

```python
from rados_nkv_weights import publish, load, InMemoryKvClient

kv = InMemoryKvClient()  # dev/test transport (see TODO below)

# Publish two revisions that share an identical tensor -> stored once.
shared = b"\x11\x22\x33\x44" * 1000
publish(kv, "my-model@revA", {
    "shared.weight": {"fp16": ("float16", (2000,), shared)},
    "a.only":        {"fp16": ("float16", (2000,), b"\xaa" * 4000)},
})
stats = publish(kv, "my-model@revB", {
    "shared.weight": {"fp16": ("float16", (2000,), shared)},  # deduped
    "b.only":        {"fp16": ("float16", (2000,), b"\xbb" * 4000)},
})
print(stats)  # PublishStats(... chunks_deduped>=1 ...)

# Load a revision/precision back. Tensor bytes reassemble exactly.
tensors = load(kv, "my-model@revB", "fp16")
assert tensors["shared.weight"] == shared

# Or reshape to numpy via the manifest's dtype/shape:
from rados_nkv_weights import load_arrays
arrays = load_arrays(kv, "my-model@revB", "fp16")
```

CLIs (use the in-memory client; demo the flow):

```bash
python -m rados_nkv_weights.publisher my-model@rev path/to/weights.safetensors --precision fp16
python -m rados_nkv_weights.loader my-model@rev fp16 --arrays
```

`tensors` is `{tensor_name: {precision: (dtype:str, shape:tuple[int,...], data:bytes)}}`.

## NVMe-KV transport is pluggable

The transport is the `kvclient.KvClient` ABC (`store` / `retrieve` / `exists`).
Two concrete implementations ship:

- `InMemoryKvClient` (dict-backed, for dev/tests; tracks `store_calls` for dedup
  assertions).
- `nvmekv_client.NvmeKvClient` — the **real NVMe-KV transport** over the SPDK
  in-process host shim. It comes in two flavours against the same
  namespace: a **read-only loader** client (`NvmeKvClient.open_loader`, only
  `retrieve`/`exists`) and an **admin publisher** client
  (`NvmeKvClient.open_publisher`, which adds `store`). The `read_only` flag is
  enforced client-side as defense-in-depth; the target-side read-only namespace
  is a separate concern (spdk-2xl).

### Building the native shim (`[nvmekv]`)

`NvmeKvClient` is import-guarded and depends on a native shared library built
from SPDK's `kv_host_shim.c`. The Python side is stdlib-only (ctypes); build the
`.so` against a built SPDK tree:

```bash
SPDK_ROOT=/path/to/spdk ./native/build.sh   # -> native/libradosnkv_kvshim.so
```

It is found automatically when it sits in `native/` beside the package, or point
`RADOSNKV_KVSHIM_LIB` at the `.so`. Without it, `import rados_nkv_weights` still
works and the pure-Python core is unaffected (the import only fails if you
actually reach for `nvmekv_client` / `_kvshim`).

```python
from rados_nkv_weights.nvmekv_client import NvmeKvClient
from rados_nkv_weights.publisher import publish
from rados_nkv_weights.loader import load

with NvmeKvClient.open_publisher(vfu_addr) as kv:   # admin write path
    publish(kv, "org/model@rev", tensors)
with NvmeKvClient.open_loader(vfu_addr) as kv:      # read-only loader path
    weights = load(kv, "org/model@rev", "fp16")
```

`vfu_addr` is the VFIOUSER transport address (the directory containing the
controller socket) of an NVMe-KV target with a KV namespace.

## Optional dependencies (import-guarded)

The core (`keys`, `chunking`, `manifest`, `kvclient`, `publisher.publish`,
`loader.load`) needs only **pyarrow + numpy + stdlib**. Extras are lazily
imported so they impose no hard dependency:

- `pip install 'rados-nkv-weights[publish]'` — `safetensors`, `huggingface_hub`
  for `publisher.publish_safetensors`.
- A vLLM `--load-format` plugin is sketched as `loader.RadosNkvModelLoaderStub`
  — **a marked stub** with no `vllm`/`torch` dependency.
- `pip install 'rados-nkv-weights[dev]'` — `pytest`.

## Develop / test

```bash
python -m venv .venv && . .venv/bin/activate
pip install pyarrow numpy pytest
pip install -e .
python -m pytest tests/ -v
```

## License

LGPL-3.0-only, matching the rados-nkv umbrella. See [LICENSE](LICENSE).
