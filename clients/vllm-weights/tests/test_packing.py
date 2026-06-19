# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""Tests for chunk-size policy + small-tensor packing (beads spdk-xee).

Covers: kvvml-derived cap, packing many small tensors into one Value, the cap
never being exceeded, byte-exact per-tensor slicing on load, large tensors still
splitting, and integrity catching a corrupted packed Value.
"""

import pytest

from rados_nkv_weights.config import DEFAULT_MAX_VALUE_LEN
from rados_nkv_weights.keys import chunk_key, manifest_key
from rados_nkv_weights.kvclient import InMemoryKvClient, KvClient
from rados_nkv_weights.loader import IntegrityError, load
from rados_nkv_weights.publisher import publish


def _stored_values(kv: InMemoryKvClient) -> dict:
    """Non-manifest (chunk/packed) Values currently in the catalog."""
    # Manifest keys carry the 0x00 tag; chunk/packed Values carry 0x01.
    return {k: v for k, v in kv._data.items() if k[:1] == b"\x01"}


def test_kvclient_default_max_value_len():
    # The abstract base default is the catalog default; InMemory is configurable.
    assert KvClient.max_value_len.fget(InMemoryKvClient()) == DEFAULT_MAX_VALUE_LEN
    assert InMemoryKvClient().max_value_len == DEFAULT_MAX_VALUE_LEN
    assert InMemoryKvClient(max_value_len=4096).max_value_len == 4096


def test_packing_groups_small_tensors_into_fewer_values():
    kv = InMemoryKvClient(max_value_len=4096)
    # 10 tensors of 100 bytes each: 1000 bytes total, well under one Value.
    tensors = {
        f"t{i}": {"fp16": ("uint8", (100,), bytes([i]) * 100)} for i in range(10)
    }
    stats = publish(kv, "pack@rev", tensors, pack=True)

    values = _stored_values(kv)
    # Far fewer stored Values than tensors (ideally a single packed Value here).
    assert len(values) < 10
    assert stats.values_total < stats.chunks_total
    assert stats.chunks_total == 10  # one slice per tensor

    # And every tensor round-trips byte-exact.
    loaded = load(kv, "pack@rev", "fp16")
    for i in range(10):
        assert loaded[f"t{i}"] == bytes([i]) * 100


def test_packed_value_never_exceeds_cap():
    cap = 1000
    kv = InMemoryKvClient(max_value_len=cap)
    # 25 tensors of 137 bytes -> packing must roll over multiple Values.
    tensors = {
        f"t{i}": {"fp16": ("uint8", (137,), bytes([i % 251]) * 137)}
        for i in range(25)
    }
    publish(kv, "cap@rev", tensors, pack=True)

    for key, value in _stored_values(kv).items():
        assert len(value) <= cap, f"value {key.hex()} exceeds cap: {len(value)}"

    loaded = load(kv, "cap@rev", "fp16")
    for i in range(25):
        assert loaded[f"t{i}"] == bytes([i % 251]) * 137


def test_packed_value_actually_shared_by_multiple_tensors():
    kv = InMemoryKvClient(max_value_len=4096)
    tensors = {
        "a": {"fp16": ("uint8", (50,), b"\xa0" * 50)},
        "b": {"fp16": ("uint8", (50,), b"\xb0" * 50)},
    }
    publish(kv, "share@rev", tensors, pack=True)

    from rados_nkv_weights.manifest import WeightManifest

    m = WeightManifest.from_ipc(kv.retrieve(manifest_key("share@rev")))
    sa = m.chunk_slices("a", "fp16")
    sb = m.chunk_slices("b", "fp16")
    # Same Value key, different offsets within it.
    assert len(sa) == 1 and len(sb) == 1
    assert sa[0][0] == sb[0][0]  # shared key
    assert sa[0][1] != sb[0][1]  # distinct offsets
    assert sa[0] == (sb[0][0], 0, 50)


def test_large_tensor_still_splits_into_multiple_chunks():
    cap = 1024
    kv = InMemoryKvClient(max_value_len=cap)
    big = bytes((i * 13 + 5) % 256 for i in range(cap * 3 + 7))
    tensors = {"big": {"fp16": ("uint8", (len(big),), big)}}

    stats = publish(kv, "big@rev", tensors, pack=True)
    # 3 cap-sized chunks + 1 sub-cap remainder = 4 slices.
    assert stats.chunks_total == 4

    for value in _stored_values(kv).values():
        assert len(value) <= cap

    loaded = load(kv, "big@rev", "fp16")
    assert loaded["big"] == big


def test_kvvml_derived_cap_is_honored_without_explicit_arg():
    cap = 512
    kv = InMemoryKvClient(max_value_len=cap)
    big = b"\x5a" * (cap * 2 + 100)
    tensors = {"t": {"fp16": ("uint8", (len(big),), big)}}

    # No max_value_len passed -> derived from kv.max_value_len.
    publish(kv, "kvvml@rev", tensors, pack=True)
    for value in _stored_values(kv).values():
        assert len(value) <= cap

    assert load(kv, "kvvml@rev", "fp16")["t"] == big


def test_explicit_max_value_len_overrides_kvvml():
    kv = InMemoryKvClient(max_value_len=DEFAULT_MAX_VALUE_LEN)
    big = b"\x33" * 700
    tensors = {"t": {"fp16": ("uint8", (700,), big)}}
    publish(kv, "ov@rev", tensors, max_value_len=256, pack=True)
    for value in _stored_values(kv).values():
        assert len(value) <= 256
    assert load(kv, "ov@rev", "fp16")["t"] == big


def test_integrity_catches_corrupted_packed_value():
    kv = InMemoryKvClient(max_value_len=4096)
    tensors = {
        "a": {"fp16": ("uint8", (40,), b"\x01" * 40)},
        "b": {"fp16": ("uint8", (40,), b"\x02" * 40)},
        "c": {"fp16": ("uint8", (40,), b"\x03" * 40)},
    }
    publish(kv, "intg@rev", tensors, pack=True)

    # Corrupt one packed Value in place, same length (so the only thing that
    # catches it is the whole-Value content-hash check, not a length check).
    for key, value in list(_stored_values(kv).items()):
        kv._data[key] = bytes(len(value))  # all-zero, same length
        break

    with pytest.raises(IntegrityError):
        load(kv, "intg@rev", "fp16")


def test_no_value_exceeds_cap_across_packing_and_splitting():
    cap = 300
    kv = InMemoryKvClient(max_value_len=cap)
    tensors = {
        "small1": {"fp16": ("uint8", (10,), b"\x01" * 10)},
        "small2": {"fp16": ("uint8", (290,), b"\x02" * 290)},
        "big": {"fp16": ("uint8", (cap * 2 + 50,), b"\x03" * (cap * 2 + 50))},
        "small3": {"fp16": ("uint8", (5,), b"\x04" * 5)},
    }
    publish(kv, "mix@rev", tensors, pack=True)
    for value in _stored_values(kv).values():
        assert len(value) <= cap

    loaded = load(kv, "mix@rev", "fp16")
    assert loaded["small1"] == b"\x01" * 10
    assert loaded["small2"] == b"\x02" * 290
    assert loaded["big"] == b"\x03" * (cap * 2 + 50)
    assert loaded["small3"] == b"\x04" * 5


def test_pack_false_keeps_one_value_per_chunk():
    kv = InMemoryKvClient(max_value_len=4096)
    tensors = {
        f"t{i}": {"fp16": ("uint8", (100,), bytes([i]) * 100)} for i in range(5)
    }
    publish(kv, "nopack@rev", tensors, pack=False)
    # One Value per tensor (distinct bytes), no packing.
    assert len(_stored_values(kv)) == 5
    loaded = load(kv, "nopack@rev", "fp16")
    for i in range(5):
        assert loaded[f"t{i}"] == bytes([i]) * 100
