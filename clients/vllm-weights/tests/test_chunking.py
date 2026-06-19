# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
import pytest

from rados_nkv_weights.chunking import iter_chunks


def test_all_chunks_within_max():
    data = bytes(range(256)) * 50  # 12800 bytes
    max_len = 1000
    chunks = list(iter_chunks(data, max_len=max_len))
    assert all(len(c) <= max_len for c in chunks)


def test_reassembly_equals_original():
    data = b"".join(bytes([i % 251]) for i in range(5000))
    chunks = list(iter_chunks(data, max_len=128))
    assert b"".join(chunks) == data


def test_empty_yields_nothing():
    assert list(iter_chunks(b"", max_len=64)) == []


def test_exact_multiple_boundary():
    data = b"x" * 300
    chunks = list(iter_chunks(data, max_len=100))
    assert len(chunks) == 3
    assert all(len(c) == 100 for c in chunks)
    assert b"".join(chunks) == data


def test_one_over_boundary():
    data = b"y" * 301
    chunks = list(iter_chunks(data, max_len=100))
    assert len(chunks) == 4
    assert [len(c) for c in chunks] == [100, 100, 100, 1]
    assert b"".join(chunks) == data


def test_smaller_than_max_single_chunk():
    data = b"tiny"
    chunks = list(iter_chunks(data, max_len=1024))
    assert chunks == [data]


def test_invalid_max_len():
    with pytest.raises(ValueError):
        list(iter_chunks(b"abc", max_len=0))
