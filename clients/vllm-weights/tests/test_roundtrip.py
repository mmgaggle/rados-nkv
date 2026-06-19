# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
import numpy as np
import pytest

from rados_nkv_weights.kvclient import InMemoryKvClient
from rados_nkv_weights.loader import (
    IntegrityError,
    MissingChunkError,
    ManifestNotFoundError,
    PrecisionNotFoundError,
    load,
    load_arrays,
)
from rados_nkv_weights.publisher import publish


def test_shared_tensor_deduped_across_revisions():
    kv = InMemoryKvClient()

    shared = b"\x11\x22\x33\x44" * 1000  # identical bytes in both revisions
    unique_a = b"\xaa" * 4000
    unique_b = b"\xbb" * 4000

    tensors_a = {
        "shared.weight": {"fp16": ("float16", (2000,), shared)},
        "a.only": {"fp16": ("float16", (2000,), unique_a)},
    }
    tensors_b = {
        "shared.weight": {"fp16": ("float16", (2000,), shared)},
        "b.only": {"fp16": ("float16", (2000,), unique_b)},
    }

    # pack=False keeps one-Value-per-chunk content addressing, so the shared
    # tensor is its own Value and dedups across revisions. (With packing on, the
    # shared tensor would be co-packed with a different neighbour per revision
    # and not dedup — the documented packing trade-off.)
    stats_a = publish(kv, "model@revA", tensors_a, pack=False)
    calls_after_a = kv.store_calls

    stats_b = publish(kv, "model@revB", tensors_b, pack=False)

    # revB's shared.weight chunk(s) already Exist -> deduped, not re-stored.
    assert stats_b.chunks_deduped >= 1
    # revB only stores its unique tensor's chunk(s) + the manifest.
    new_stores = kv.store_calls - calls_after_a
    assert new_stores == stats_b.chunks_stored + 1  # +1 for the manifest

    # Both revisions load back, and the shared tensor matches exactly.
    loaded_a = load(kv, "model@revA", "fp16")
    loaded_b = load(kv, "model@revB", "fp16")
    assert loaded_a["shared.weight"] == shared
    assert loaded_b["shared.weight"] == shared
    assert loaded_a["shared.weight"] == loaded_b["shared.weight"]
    assert loaded_a["a.only"] == unique_a
    assert loaded_b["b.only"] == unique_b

    # Dedup accounting sanity.
    assert stats_a.chunks_deduped == 0


def test_large_tensor_split_into_multiple_chunks_and_reassembles():
    kv = InMemoryKvClient()
    max_len = 1024  # small cap to force splitting without huge buffers

    big = bytes((i * 7 + 3) % 256 for i in range(max_len * 3 + 17))
    tensors = {"big.weight": {"fp16": ("uint8", (len(big),), big)}}

    stats = publish(kv, "big@rev", tensors, max_value_len=max_len)
    assert stats.chunks_total == 4  # 3 full + 1 partial

    loaded = load(kv, "big@rev", "fp16")
    assert loaded["big.weight"] == big


def test_load_arrays_reshapes_via_dtype_shape():
    kv = InMemoryKvClient()
    arr = np.arange(24, dtype=np.float32).reshape(4, 6)
    tensors = {"t": {"fp32": ("float32", (4, 6), arr.tobytes())}}
    publish(kv, "arr@rev", tensors)

    out = load_arrays(kv, "arr@rev", "fp32")
    assert out["t"].dtype == np.float32
    assert out["t"].shape == (4, 6)
    np.testing.assert_array_equal(out["t"], arr)


def test_missing_manifest_raises():
    kv = InMemoryKvClient()
    with pytest.raises(ManifestNotFoundError):
        load(kv, "nonexistent@rev", "fp16")


def test_missing_precision_raises():
    kv = InMemoryKvClient()
    publish(kv, "m@rev", {"t": {"fp16": ("float16", (4,), b"\x00" * 8)}})
    with pytest.raises(PrecisionNotFoundError):
        load(kv, "m@rev", "fp8")


def test_missing_chunk_raises():
    kv = InMemoryKvClient()
    publish(kv, "m@rev", {"t": {"fp16": ("float16", (4,), b"\x01" * 8)}})

    # Evict every non-manifest value to simulate a missing chunk.
    from rados_nkv_weights.keys import manifest_key

    mkey = manifest_key("m@rev")
    for k in list(kv._data.keys()):
        if k != mkey:
            del kv._data[k]

    with pytest.raises(MissingChunkError):
        load(kv, "m@rev", "fp16")


def test_mixed_per_tensor_precision_loads_only_requested_precision():
    # Finding 1: load(p) materializes EXACTLY the tensors published in p.
    kv = InMemoryKvClient()

    a_bytes = b"\xa1" * 32  # tensor A: fp16 only
    b_bytes = b"\xb2" * 16  # tensor B: int4 only
    c_fp16 = b"\xc3" * 24   # tensor C: both precisions (distinct bytes each)
    c_int4 = b"\xc4" * 12

    tensors = {
        "A": {"fp16": ("float16", (16,), a_bytes)},
        "B": {"int4": ("uint8", (16,), b_bytes)},
        "C": {
            "fp16": ("float16", (12,), c_fp16),
            "int4": ("uint8", (12,), c_int4),
        },
    }
    publish(kv, "mixed@rev", tensors)

    loaded_fp16 = load(kv, "mixed@rev", "fp16")
    assert set(loaded_fp16) == {"A", "C"}
    assert loaded_fp16["A"] == a_bytes
    assert loaded_fp16["C"] == c_fp16

    loaded_int4 = load(kv, "mixed@rev", "int4")
    assert set(loaded_int4) == {"B", "C"}
    assert loaded_int4["B"] == b_bytes
    assert loaded_int4["C"] == c_int4


def test_load_arrays_respects_mixed_per_tensor_precision():
    kv = InMemoryKvClient()
    a = np.arange(8, dtype=np.float16)
    b = np.arange(4, dtype=np.uint8)
    tensors = {
        "A": {"fp16": ("float16", (8,), a.tobytes())},
        "B": {"int4": ("uint8", (4,), b.tobytes())},
    }
    publish(kv, "mixedarr@rev", tensors)

    out16 = load_arrays(kv, "mixedarr@rev", "fp16")
    assert set(out16) == {"A"}
    np.testing.assert_array_equal(out16["A"], a)

    out4 = load_arrays(kv, "mixedarr@rev", "int4")
    assert set(out4) == {"B"}
    np.testing.assert_array_equal(out4["B"], b)


def test_corrupted_chunk_raises_integrity_error():
    # Finding 2: same-length garbage under a Chunk key is caught, not returned.
    kv = InMemoryKvClient()
    payload = b"\x07" * 64
    publish(kv, "intg@rev", {"t": {"fp16": ("float16", (32,), payload)}})

    from rados_nkv_weights.keys import manifest_key

    mkey = manifest_key("intg@rev")
    # Overwrite the (single) chunk value with same-length garbage.
    for k, v in list(kv._data.items()):
        if k != mkey and len(v) == len(payload):
            kv._data[k] = b"\xff" * len(payload)  # same length, wrong content

    with pytest.raises(IntegrityError):
        load(kv, "intg@rev", "fp16")


def test_publisher_loader_resolve_same_manifest_despite_formatting():
    # Finding 4: canonicalization makes a publisher and loader that format the
    # revision slightly differently resolve the same manifest.
    kv = InMemoryKvClient()
    publish(kv, "Org/Model@v1", {"t": {"fp16": ("float16", (4,), b"\x00" * 8)}})

    loaded = load(kv, "  Org/Model@v1/  ", "fp16")
    assert loaded["t"] == b"\x00" * 8
