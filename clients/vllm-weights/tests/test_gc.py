# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""GC / mark-sweep tests.

Headline correctness property: a content-hash chunk SHARED across models is
never swept while ANY referencing model is live — even when another model that
also referenced it is being removed.
"""

import pytest

from rados_nkv_weights.gc import gc, GcStats, GcUnsafeError
from rados_nkv_weights.keys import chunk_key, manifest_key
from rados_nkv_weights.kvclient import InMemoryKvClient
from rados_nkv_weights.loader import load
from rados_nkv_weights.publisher import publish


# Bytes used across the suite. B and C SHARE a tensor (identical bytes) so, with
# pack=False (one Value per content-hash chunk), it dedups to ONE shared chunk.
SHARED = b"\x5c" * 4000          # shared between models B and C
A_ONLY = b"\xaa" * 4000          # unique to A
B_ONLY = b"\xbb" * 4000          # unique to B
C_ONLY = b"\xcc" * 4000          # unique to C


def _publish_abc(kv):
    publish(
        kv, "model@A",
        {"a.only": {"fp16": ("float16", (2000,), A_ONLY)}},
        pack=False,
    )
    publish(
        kv, "model@B",
        {
            "shared.weight": {"fp16": ("float16", (2000,), SHARED)},
            "b.only": {"fp16": ("float16", (2000,), B_ONLY)},
        },
        pack=False,
    )
    publish(
        kv, "model@C",
        {
            "shared.weight": {"fp16": ("float16", (2000,), SHARED)},
            "c.only": {"fp16": ("float16", (2000,), C_ONLY)},
        },
        pack=False,
    )


def test_setup_shares_one_chunk_between_b_and_c():
    # Precondition: SHARED really dedups to a single content-hash chunk key that
    # both B and C reference, so removing C cannot delete it (B is live).
    kv = InMemoryKvClient()
    _publish_abc(kv)
    shared_key = chunk_key(SHARED)
    assert kv.exists(shared_key)


def test_gc_sweeps_only_orphans_keeps_shared_chunk():
    kv = InMemoryKvClient()
    _publish_abc(kv)

    shared_key = chunk_key(SHARED)
    c_only_key = chunk_key(C_ONLY)
    c_mkey = manifest_key("model@C")

    stats = gc(kv, ["model@A", "model@B"])

    assert isinstance(stats, GcStats)
    assert stats.dry_run is False
    assert stats.live_manifests == 2

    # C's UNIQUE chunk and C's manifest were swept...
    assert not kv.exists(c_only_key)
    assert not kv.exists(c_mkey)
    assert c_only_key in stats.swept_keys
    assert c_mkey in stats.swept_keys
    assert stats.swept == 2  # exactly C's unique chunk + C's manifest

    # ...but the B/C-shared chunk is NOT swept (B is live).
    assert kv.exists(shared_key)
    assert shared_key not in stats.swept_keys

    # A and B remain fully intact and still loadable byte-for-byte.
    assert load(kv, "model@A", "fp16")["a.only"] == A_ONLY
    loaded_b = load(kv, "model@B", "fp16")
    assert loaded_b["shared.weight"] == SHARED
    assert loaded_b["b.only"] == B_ONLY

    # C is gone: its manifest was swept.
    from rados_nkv_weights.loader import ManifestNotFoundError
    with pytest.raises(ManifestNotFoundError):
        load(kv, "model@C", "fp16")


def test_dry_run_deletes_nothing_but_reports_same_sweep_set():
    kv = InMemoryKvClient()
    _publish_abc(kv)

    before = set(kv.iter_keys())
    dry = gc(kv, ["model@A", "model@B"], dry_run=True)

    assert dry.dry_run is True
    # Nothing actually deleted.
    assert set(kv.iter_keys()) == before
    assert kv.exists(chunk_key(C_ONLY))
    assert kv.exists(manifest_key("model@C"))

    # But the reported sweep set matches a real run's.
    fresh = InMemoryKvClient()
    _publish_abc(fresh)
    real = gc(fresh, ["model@A", "model@B"])
    assert set(dry.swept_keys) == set(real.swept_keys)
    assert dry.swept == real.swept == 2


def test_shared_chunk_safe_when_one_of_two_sharers_removed():
    # Headline property, isolated: live={B} removes C; the shared chunk stays
    # because B references it; B's own unique chunk + manifest stay too.
    kv = InMemoryKvClient()
    _publish_abc(kv)

    gc(kv, ["model@A", "model@B"])

    assert kv.exists(chunk_key(SHARED))   # shared, B live -> kept
    assert kv.exists(chunk_key(B_ONLY))   # B unique -> kept
    assert load(kv, "model@B", "fp16")["shared.weight"] == SHARED


def test_missing_live_manifest_fails_safe_no_deletion():
    kv = InMemoryKvClient()
    _publish_abc(kv)
    before = set(kv.iter_keys())

    # A live revision with no manifest in the catalog -> incomplete live-set.
    with pytest.raises(GcUnsafeError) as exc:
        gc(kv, ["model@A", "model@B", "ghost@rev"])

    assert "ghost@rev" in exc.value.unreadable
    # Fail-safe: NOTHING was deleted, including C's orphans.
    assert set(kv.iter_keys()) == before
    assert kv.exists(chunk_key(C_ONLY))
    assert kv.exists(manifest_key("model@C"))


def test_unparseable_live_manifest_fails_safe():
    kv = InMemoryKvClient()
    _publish_abc(kv)
    # Corrupt model@A's manifest so it cannot be parsed -> its chunks unknown.
    kv.store(manifest_key("model@A"), b"not-an-arrow-ipc-stream")
    before = set(kv.iter_keys())

    with pytest.raises(GcUnsafeError):
        gc(kv, ["model@A", "model@B"])

    assert set(kv.iter_keys()) == before  # deleted nothing


def test_all_live_sweeps_nothing():
    kv = InMemoryKvClient()
    _publish_abc(kv)
    stats = gc(kv, ["model@A", "model@B", "model@C"])
    assert stats.swept == 0
    assert stats.live_manifests == 3
    # All three still load.
    for rev, key, payload in [
        ("model@A", "a.only", A_ONLY),
        ("model@B", "b.only", B_ONLY),
        ("model@C", "c.only", C_ONLY),
    ]:
        assert load(kv, rev, "fp16")[key] == payload


def test_empty_live_set_sweeps_everything():
    kv = InMemoryKvClient()
    _publish_abc(kv)
    n = len(list(kv.iter_keys()))
    assert n > 0
    stats = gc(kv, [])
    assert stats.swept == n
    assert stats.live_manifests == 0
    assert len(list(kv.iter_keys())) == 0


def test_readonly_client_delete_raises():
    # The read-only loader handle must NOT allow delete (mirrors store).
    from rados_nkv_weights.nvmekv_client import NvmeKvClient, ReadOnlyError

    loader = NvmeKvClient(handle=None, read_only=True)
    with pytest.raises(ReadOnlyError):
        loader.delete(b"\x01" + b"\x00" * 15)


def test_inmemory_delete_and_iter_keys():
    kv = InMemoryKvClient()
    kv.store(b"\x00" * 16, b"v0")
    kv.store(b"\x01" * 16, b"v1")
    assert set(kv.iter_keys()) == {b"\x00" * 16, b"\x01" * 16}
    kv.delete(b"\x00" * 16)
    assert set(kv.iter_keys()) == {b"\x01" * 16}
    # Deleting an absent key is a no-op.
    kv.delete(b"\x02" * 16)
    assert set(kv.iter_keys()) == {b"\x01" * 16}


def test_bytes_swept_accounting():
    kv = InMemoryKvClient()
    _publish_abc(kv)
    stats = gc(kv, ["model@A", "model@B"], dry_run=True)
    # bytes_swept counts C's unique chunk (4000) + C's manifest (non-empty).
    assert stats.bytes_swept >= len(C_ONLY)
