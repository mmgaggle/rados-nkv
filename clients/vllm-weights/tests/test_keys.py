# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
from rados_nkv_weights.config import KEY_LEN
from rados_nkv_weights.keys import (
    CHUNK_TAG,
    MANIFEST_TAG,
    canonical_revision,
    chunk_key,
    manifest_key,
)


def test_chunk_key_length():
    assert len(chunk_key(b"")) == KEY_LEN == 16
    assert len(chunk_key(b"some weight bytes")) == 16


def test_chunk_key_deterministic():
    data = b"\x00\x01\x02\x03" * 100
    assert chunk_key(data) == chunk_key(data)


def test_chunk_key_differs_by_content():
    assert chunk_key(b"alpha") != chunk_key(b"beta")
    assert chunk_key(b"alpha") != chunk_key(b"alpha ")


def test_manifest_key_length():
    assert len(manifest_key("meta-llama/Llama-3-8B@main")) == 16


def test_manifest_key_deterministic():
    rev = "meta-llama/Llama-3-8B@abc123"
    assert manifest_key(rev) == manifest_key(rev)


def test_manifest_key_differs_by_revision():
    assert manifest_key("model@v1") != manifest_key("model@v2")


def test_manifest_key_distinct_from_chunk_key_space():
    # The 1-byte domain tag keeps manifest keys from colliding with the
    # content-hash chunk keyspace for the same input string.
    s = "model@v1"
    assert manifest_key(s) != chunk_key(s.encode("utf-8"))


def test_keys_carry_structural_domain_tag():
    # Separation is STRUCTURAL: distinct reserved leading tag bytes, not just an
    # in-preimage prefix. Every chunk key begins \x01, every manifest key \x00.
    assert MANIFEST_TAG == b"\x00"
    assert CHUNK_TAG == b"\x01"
    assert chunk_key(b"anything")[:1] == CHUNK_TAG
    assert manifest_key("model@v1")[:1] == MANIFEST_TAG
    # Disjoint keyspaces by construction: no chunk key can equal any manifest
    # key because their first bytes differ.
    assert chunk_key(b"x")[:1] != manifest_key("x")[:1]


def test_keys_stay_16_bytes_with_tag():
    # 1-byte tag + 15-byte (120-bit) digest == KEY_LEN.
    assert KEY_LEN == 16
    assert len(chunk_key(b"")) == 16
    assert len(chunk_key(b"weights")) == 16
    assert len(manifest_key("model@v1")) == 16


def test_chunk_key_dedup_preserved_under_tag():
    # The tag is a constant, so the chunk key is still a pure function of
    # content — identical bytes still produce identical keys (dedup works).
    data = b"\x10\x20\x30" * 333
    assert chunk_key(data) == chunk_key(data)
    assert chunk_key(b"a") != chunk_key(b"b")


def test_canonical_revision_normalizes_cosmetic_differences():
    assert canonical_revision("model@v1") == "model@v1"
    assert canonical_revision("model@v1/") == "model@v1"
    assert canonical_revision(" model@v1 ") == "model@v1"


def test_canonical_revision_preserves_case_and_internal_slashes():
    # HF ids are case-sensitive and slash-structured.
    assert canonical_revision("Org/Model@v1") == "Org/Model@v1"
    assert canonical_revision("Model@v1") != canonical_revision("model@v1")


def test_manifest_key_canonicalizes_revision():
    # 'model@v1', 'model@v1/', ' model@v1 ' all resolve to the SAME key.
    base = manifest_key("model@v1")
    assert manifest_key("model@v1/") == base
    assert manifest_key(" model@v1 ") == base
    # Case and version stay distinct.
    assert manifest_key("Model@v1") != base
    assert manifest_key("model@v2") != base
