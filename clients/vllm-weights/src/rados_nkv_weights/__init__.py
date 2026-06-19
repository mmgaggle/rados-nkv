# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""rados_nkv_weights — model-weights catalog over an NVMe-KV-on-RADOS namespace.

Two components, mirroring the read/write split:

- **Weights publisher** (write path, :mod:`rados_nkv_weights.publisher`): chunks each
  tensor, Stores each chunk under its content-hash Chunk key (skipping chunks that
  already Exist — dedup), and writes the per-Model-revision Weight manifest.
- **Weights loader** (read path, :mod:`rados_nkv_weights.loader`): Retrieves the
  Weight manifest by its deterministic Manifest key, then Retrieves each tensor's
  Chunk keys in order and reassembles the tensor.

The on-the-wire layout (Arrow IPC manifest + content-hash chunks).

The NVMe-KV transport is pluggable via :class:`rados_nkv_weights.kvclient.KvClient`.
:class:`~rados_nkv_weights.kvclient.InMemoryKvClient` is the dev/test transport;
:class:`rados_nkv_weights.nvmekv_client.NvmeKvClient` is the real NVMe-KV
transport over the SPDK host shim (an optional extra requiring the native
``libradosnkv_kvshim.so`` — see ``native/build.sh``). It is import-guarded and
NOT imported here, so this top-level ``import rados_nkv_weights`` keeps working
with no SPDK / native library present; reach for ``nvmekv_client`` explicitly.
"""

from .config import DEFAULT_MAX_VALUE_LEN, KEY_LEN
from .keys import canonical_revision, chunk_key, manifest_key
from .chunking import iter_chunks
from .manifest import WeightManifest
from .kvclient import KvClient, InMemoryKvClient
from .publisher import publish, PublishStats
from .loader import IntegrityError, load, load_arrays
from .gc import gc, GcStats, GcUnsafeError

__all__ = [
    "KEY_LEN",
    "DEFAULT_MAX_VALUE_LEN",
    "canonical_revision",
    "chunk_key",
    "manifest_key",
    "iter_chunks",
    "WeightManifest",
    "KvClient",
    "InMemoryKvClient",
    "publish",
    "PublishStats",
    "IntegrityError",
    "load",
    "load_arrays",
    "gc",
    "GcStats",
    "GcUnsafeError",
]

__version__ = "0.1.0"
