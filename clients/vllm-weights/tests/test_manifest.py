# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
from rados_nkv_weights.manifest import WeightManifest


def _k(b: bytes) -> bytes:
    # 16-byte stand-in chunk key.
    return (b * 16)[:16]


def test_build_to_ipc_from_ipc_roundtrip():
    m = WeightManifest()
    m.add_tensor(
        "model.embed",
        "fp16",
        "float16",
        (32000, 4096),
        [_k(b"a"), _k(b"b")],
        [100, 50],
    )
    m.add_tensor(
        "model.layers.0.mlp",
        "fp16",
        "float16",
        (4096, 11008),
        [_k(b"c")],
        [200],
    )

    blob = m.to_ipc()
    assert isinstance(blob, bytes) and len(blob) > 0

    r = WeightManifest.from_ipc(blob)
    assert r.tensors() == ["model.embed", "model.layers.0.mlp"]
    assert r.precisions() == ["fp16"]
    assert r.chunk_keys("model.embed", "fp16") == [_k(b"a"), _k(b"b")]
    dtype, shape, sizes = r.tensor_meta("model.embed", "fp16")
    assert dtype == "float16"
    assert shape == (32000, 4096)
    assert sizes == [100, 50]


def test_packed_offsets_roundtrip_through_ipc():
    m = WeightManifest()
    shared = _k(b"p")
    # Two tensors share one packed Value at distinct offsets.
    m.add_tensor("x", "fp16", "float16", (8,), [shared], [16], offsets=[0])
    m.add_tensor("y", "fp16", "float16", (4,), [shared], [8], offsets=[16])

    r = WeightManifest.from_ipc(m.to_ipc())
    assert r.chunk_slices("x", "fp16") == [(shared, 0, 16)]
    assert r.chunk_slices("y", "fp16") == [(shared, 16, 8)]
    # chunk_keys still returns just the keys.
    assert r.chunk_keys("x", "fp16") == [shared]


def test_default_offsets_are_zero():
    m = WeightManifest()
    m.add_tensor("t", "fp16", "float16", (8,), [_k(b"a"), _k(b"b")], [10, 6])
    assert m.chunk_slices("t", "fp16") == [(_k(b"a"), 0, 10), (_k(b"b"), 0, 6)]


def test_per_precision_keys_and_sizes_retrievable():
    m = WeightManifest()
    m.add_tensor("w", "fp16", "float16", (8,), [_k(b"h")], [16])
    m.add_tensor("w", "fp8", "float8_e4m3fn", (8,), [_k(b"q")], [8])

    assert m.chunk_keys("w", "fp16") == [_k(b"h")]
    assert m.chunk_keys("w", "fp8") == [_k(b"q")]
    assert m.tensor_meta("w", "fp16")[2] == [16]
    assert m.tensor_meta("w", "fp8")[2] == [8]


def test_multiple_precisions_coexist_through_ipc():
    m = WeightManifest()
    m.add_tensor("w", "fp16", "float16", (8,), [_k(b"h")], [16])
    m.add_tensor("w", "fp8", "float8_e4m3fn", (8,), [_k(b"q")], [8])
    # A second tensor present in only one precision (null column group elsewhere).
    m.add_tensor("only16", "fp16", "float16", (4,), [_k(b"z")], [8])

    r = WeightManifest.from_ipc(m.to_ipc())
    assert set(r.precisions()) == {"fp16", "fp8"}
    assert r.tensors() == ["w", "only16"]
    assert r.chunk_keys("w", "fp16") == [_k(b"h")]
    assert r.chunk_keys("w", "fp8") == [_k(b"q")]
    assert r.chunk_keys("only16", "fp16") == [_k(b"z")]
    # only16 has no fp8 variant.
    import pytest

    with pytest.raises(KeyError):
        r.chunk_keys("only16", "fp8")
