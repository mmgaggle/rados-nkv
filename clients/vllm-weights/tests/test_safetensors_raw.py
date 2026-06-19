# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""Tests for the torch-free safetensors reader (bf16 / fp8 raw path).

numpy cannot represent bfloat16, so the publisher reads such tensors as raw
bytes straight from the safetensors header. These tests build bf16 (and mixed
bf16+fp16) files by hand and assert a byte-exact publish -> load round-trip plus
the streaming digest helper used by `rados-nkv-load --verify`.
"""

import json
import struct

import numpy as np
import pytest

from rados_nkv_weights.kvclient import InMemoryKvClient
from rados_nkv_weights.loader import _verify, load
from rados_nkv_weights.publisher import publish_safetensors
from rados_nkv_weights.safetensors_raw import iter_tensor_digests, read_tensors


def _write_safetensors(path, tensors):
    """tensors: {name: (dtype_code, shape, raw_bytes)} -> write a .safetensors."""
    header, blob, off = {}, bytearray(), 0
    for name, (dt, shape, raw) in tensors.items():
        header[name] = {"dtype": dt, "shape": list(shape), "data_offsets": [off, off + len(raw)]}
        blob += raw
        off += len(raw)
    hb = json.dumps(header).encode()
    with open(path, "wb") as f:
        f.write(struct.pack("<Q", len(hb)))
        f.write(hb)
        f.write(bytes(blob))


def _bf16_bytes(values):
    """float32 array -> bf16 little-endian bytes (truncate the low 16 bits)."""
    u32 = np.asarray(values, dtype="<f4").view("<u4")
    return (u32 >> 16).astype("<u2").tobytes()


def test_read_tensors_bf16_raw_bytes_and_dtype(tmp_path):
    p = str(tmp_path / "m.safetensors")
    raw = _bf16_bytes(np.linspace(-3, 3, 50))
    _write_safetensors(p, {"w": ("BF16", (50,), raw)})

    out = read_tensors(p)
    assert set(out) == {"w"}
    dt, shape, data = out["w"]
    assert dt == "bfloat16"  # recorded name (numpy can't construct it)
    assert shape == (50,)
    assert data == raw  # exact on-disk bytes, no numpy conversion


def test_bf16_publish_load_roundtrip_and_verify(tmp_path):
    p = str(tmp_path / "model.safetensors")
    w = _bf16_bytes(np.linspace(-1, 1, 300))
    b = _bf16_bytes(np.arange(128))
    _write_safetensors(p, {"layer.w": ("BF16", (300,), w), "layer.b": ("BF16", (128,), b)})

    kv = InMemoryKvClient()
    publish_safetensors(kv, "granite-ish@v1", p, "bf16", max_value_len=256)

    loaded = load(kv, "granite-ish@v1", "bf16")
    assert loaded["layer.w"] == w
    assert loaded["layer.b"] == b

    assert _verify(kv, "granite-ish@v1", "bf16", p) == 0


def test_mixed_bf16_and_fp16_in_one_file(tmp_path):
    p = str(tmp_path / "mixed.safetensors")
    bf = _bf16_bytes(np.arange(16))
    f16 = np.arange(16, dtype=np.float16).tobytes()
    _write_safetensors(p, {"bf": ("BF16", (16,), bf), "f16": ("F16", (16,), f16)})

    out = read_tensors(p)
    assert out["bf"] == ("bfloat16", (16,), bf)
    # the fp16 tensor goes through the numpy backend -> canonical "float16"
    assert out["f16"][0] == "float16"
    assert out["f16"][2] == f16


def test_iter_tensor_digests_matches_read_tensors(tmp_path):
    import hashlib

    p = str(tmp_path / "d.safetensors")
    raw = _bf16_bytes(np.linspace(0, 1, 64))
    _write_safetensors(p, {"t": ("BF16", (64,), raw)})

    digests = list(iter_tensor_digests(p))
    assert len(digests) == 1
    name, dt, shape, nbytes, sha = digests[0]
    assert (name, dt, shape, nbytes) == ("t", "bfloat16", (64,), len(raw))
    assert sha == hashlib.sha256(raw).hexdigest()
