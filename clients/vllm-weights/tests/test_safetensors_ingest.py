# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""Tests for safetensors / HuggingFace ingest (beads spdk-7s8).

Covers the real ingest path: reading tensors from local ``.safetensors`` files
(single file, sharded directory, index.json), byte-exact + (dtype, shape)
round-trip through publish/load, multi-precision manifest merge into one
revision, dedup on re-publish, and the network-guarded HuggingFace plumbing.

``safetensors`` is pip-installable and works fully offline; the HF test is
``importorskip``'d and uses a monkeypatched ``snapshot_download`` so CI never
touches the network.
"""

import json
import os

import numpy as np
import pytest

from rados_nkv_weights.kvclient import InMemoryKvClient
from rados_nkv_weights.keys import manifest_key
from rados_nkv_weights.loader import load, load_arrays
from rados_nkv_weights.manifest import WeightManifest
from rados_nkv_weights.publisher import (
    publish_huggingface,
    publish_safetensors,
)

# safetensors is a hard requirement for these tests (pip-installable, offline).
save_file = pytest.importorskip("safetensors.numpy").save_file


def _fp16_model():
    return {
        "embed.weight": np.arange(24, dtype=np.float16).reshape(4, 6),
        "layer.0.bias": np.array([1.0, 2.0, 3.0], dtype=np.float32),
        "layer.0.weight": (np.arange(20, dtype=np.float16) * 0.5).reshape(5, 4),
        "scalar": np.array([7.0], dtype=np.float32),  # 1-d single-element tensor
    }


def _int8_model():
    # Same tensor NAMES as the fp16 model, different dtype/bytes per precision.
    return {
        "embed.weight": (np.arange(24, dtype=np.int8)).reshape(4, 6),
        "layer.0.bias": np.array([10, 20, 30], dtype=np.int8),
        "layer.0.weight": np.arange(20, dtype=np.int8).reshape(5, 4),
        "scalar": np.array([5], dtype=np.int8),
    }


def _write(tmp_path, name, tensors):
    path = os.path.join(str(tmp_path), name)
    save_file({k: v for k, v in tensors.items()}, path)
    return path


def _assert_roundtrip(kv, rev, precision, model):
    """Every tensor's bytes AND (dtype, shape) must match byte-for-byte."""
    raw = load(kv, rev, precision)
    arrays = load_arrays(kv, rev, precision)
    assert set(raw) == set(model)
    for name, arr in model.items():
        assert raw[name] == arr.tobytes(), f"bytes mismatch for {name}"
        np.testing.assert_array_equal(arrays[name], arr)
        assert arrays[name].dtype == arr.dtype
        assert arrays[name].shape == arr.shape


def test_single_file_roundtrip_byte_exact(tmp_path):
    model = _fp16_model()
    path = _write(tmp_path, "model.safetensors", model)
    kv = InMemoryKvClient(max_value_len=4096)

    publish_safetensors(kv, "ingest@rev", path, "fp16")
    _assert_roundtrip(kv, "ingest@rev", "fp16", model)


def test_multi_precision_merges_into_same_manifest(tmp_path):
    fp16 = _fp16_model()
    int8 = _int8_model()
    fp16_path = _write(tmp_path, "fp16.safetensors", fp16)
    int8_path = _write(tmp_path, "int8.safetensors", int8)

    kv = InMemoryKvClient(max_value_len=4096)
    rev = "mixed@rev"

    publish_safetensors(kv, rev, fp16_path, "fp16")
    publish_safetensors(kv, rev, int8_path, "int8")  # merge defaults True

    # One manifest, BOTH precisions present.
    m = WeightManifest.from_ipc(kv.retrieve(manifest_key(rev)))
    assert set(m.precisions()) == {"fp16", "int8"}

    # Each precision loads back byte-exact (mixed precision per ADR-0009).
    _assert_roundtrip(kv, rev, "fp16", fp16)
    _assert_roundtrip(kv, rev, "int8", int8)


def test_no_merge_clobbers_other_precisions(tmp_path):
    fp16_path = _write(tmp_path, "fp16.safetensors", _fp16_model())
    int8_path = _write(tmp_path, "int8.safetensors", _int8_model())

    kv = InMemoryKvClient(max_value_len=4096)
    rev = "clobber@rev"

    publish_safetensors(kv, rev, fp16_path, "fp16")
    publish_safetensors(kv, rev, int8_path, "int8", merge=False)

    m = WeightManifest.from_ipc(kv.retrieve(manifest_key(rev)))
    assert m.precisions() == ["int8"]  # fp16 was replaced, not merged


def test_republish_same_precision_is_rejected(tmp_path):
    path = _write(tmp_path, "model.safetensors", _fp16_model())
    kv = InMemoryKvClient(max_value_len=4096)
    publish_safetensors(kv, "dup@rev", path, "fp16")
    # Re-publishing the SAME precision must not silently clobber the manifest's
    # record of it; the manifest builder rejects the duplicate precision column.
    with pytest.raises(ValueError):
        publish_safetensors(kv, "dup@rev", path, "fp16")


def test_dedup_second_publish_stores_zero_new_values(tmp_path):
    path = _write(tmp_path, "model.safetensors", _fp16_model())
    kv = InMemoryKvClient(max_value_len=4096)

    publish_safetensors(kv, "dedup@rev", path, "fp16")
    calls_after_first = kv.store_calls

    # Publishing the identical bytes into a DIFFERENT revision stores only the
    # new manifest; every chunk/packed Value already Exists.
    stats = publish_safetensors(kv, "dedup@rev2", path, "fp16")
    assert stats.values_stored == 0
    assert stats.values_deduped == stats.values_total
    # Exactly one new store: the second revision's manifest.
    assert kv.store_calls - calls_after_first == 1


def test_directory_of_shards_roundtrip(tmp_path):
    # Split the model across two shard files + a HF-style index.json.
    model = _fp16_model()
    shard_dir = tmp_path / "sharded"
    shard_dir.mkdir()
    a = {"embed.weight": model["embed.weight"], "scalar": model["scalar"]}
    b = {"layer.0.bias": model["layer.0.bias"], "layer.0.weight": model["layer.0.weight"]}
    save_file(a, str(shard_dir / "model-00001-of-00002.safetensors"))
    save_file(b, str(shard_dir / "model-00002-of-00002.safetensors"))
    index = {
        "metadata": {},
        "weight_map": {
            "embed.weight": "model-00001-of-00002.safetensors",
            "scalar": "model-00001-of-00002.safetensors",
            "layer.0.bias": "model-00002-of-00002.safetensors",
            "layer.0.weight": "model-00002-of-00002.safetensors",
        },
    }
    with open(shard_dir / "model.safetensors.index.json", "w") as fh:
        json.dump(index, fh)

    # Publish via the directory (auto-detects the index)...
    kv = InMemoryKvClient(max_value_len=4096)
    publish_safetensors(kv, "shard-dir@rev", str(shard_dir), "fp16")
    _assert_roundtrip(kv, "shard-dir@rev", "fp16", model)

    # ...and via the index.json path directly.
    kv2 = InMemoryKvClient(max_value_len=4096)
    publish_safetensors(
        kv2, "shard-idx@rev", str(shard_dir / "model.safetensors.index.json"), "fp16"
    )
    _assert_roundtrip(kv2, "shard-idx@rev", "fp16", model)


def test_directory_of_loose_shards_without_index(tmp_path):
    model = _fp16_model()
    shard_dir = tmp_path / "loose"
    shard_dir.mkdir()
    save_file(
        {"embed.weight": model["embed.weight"]},
        str(shard_dir / "a.safetensors"),
    )
    save_file(
        {
            "layer.0.bias": model["layer.0.bias"],
            "layer.0.weight": model["layer.0.weight"],
            "scalar": model["scalar"],
        },
        str(shard_dir / "b.safetensors"),
    )
    kv = InMemoryKvClient(max_value_len=4096)
    publish_safetensors(kv, "loose@rev", str(shard_dir), "fp16")
    _assert_roundtrip(kv, "loose@rev", "fp16", model)


def test_huggingface_plumbing_with_mocked_download(tmp_path, monkeypatch):
    """Network-guarded: skip if huggingface_hub is absent; never hit the network.

    We monkeypatch ``snapshot_download`` to return a local directory we wrote
    ourselves, exercising the arg plumbing, the derived model_revision, and the
    delegation to publish_safetensors without any Hub access.
    """
    pytest.importorskip("huggingface_hub")
    import rados_nkv_weights.publisher as pub

    model = _fp16_model()
    snap = tmp_path / "snapshot"
    snap.mkdir()
    save_file(model, str(snap / "model.safetensors"))

    captured = {}

    def fake_snapshot_download(repo_id, revision=None, allow_patterns=None, **kw):
        captured["repo_id"] = repo_id
        captured["revision"] = revision
        captured["allow_patterns"] = allow_patterns
        return str(snap)

    monkeypatch.setattr(
        "huggingface_hub.snapshot_download", fake_snapshot_download
    )

    kv = InMemoryKvClient(max_value_len=4096)
    publish_huggingface(kv, "org/my-model", "fp16", revision="v2")

    # Derived revision is '<repo_id>@<revision>' (canonicalized) and keys the
    # manifest; the model loads back byte-exact.
    assert captured["repo_id"] == "org/my-model"
    assert captured["revision"] == "v2"
    assert "*.safetensors" in captured["allow_patterns"]
    _assert_roundtrip(kv, "org/my-model@v2", "fp16", model)


def test_huggingface_missing_dep_raises(monkeypatch):
    """If huggingface_hub import fails, a clear ImportError points at [publish]."""
    import builtins

    real_import = builtins.__import__

    def blocked_import(name, *args, **kw):
        if name == "huggingface_hub" or name.startswith("huggingface_hub."):
            raise ImportError("blocked for test")
        return real_import(name, *args, **kw)

    monkeypatch.setattr(builtins, "__import__", blocked_import)
    kv = InMemoryKvClient()
    with pytest.raises(ImportError, match="publish"):
        publish_huggingface(kv, "org/m", "fp16")
