# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""Slice a tensor's bytes into Weight chunks bounded by the value cap.

A tensor larger than the librados max value length (64 MiB, see
:data:`rados_nkv_weights.config.DEFAULT_MAX_VALUE_LEN`) spans multiple Weight
chunks listed in order by the Weight manifest. Chunking here is purely
positional (fixed-size slices); content addressing happens later in
:func:`rados_nkv_weights.keys.chunk_key`.
"""

from typing import Iterator

from .config import DEFAULT_MAX_VALUE_LEN


def iter_chunks(data: bytes, max_len: int = DEFAULT_MAX_VALUE_LEN) -> Iterator[bytes]:
    """Yield successive ``<= max_len`` byte slices of ``data``, in order.

    Concatenating the yielded slices reproduces ``data`` exactly. An empty input
    yields nothing. ``max_len`` must be positive.
    """
    if max_len <= 0:
        raise ValueError(f"max_len must be positive, got {max_len}")
    for offset in range(0, len(data), max_len):
        yield data[offset : offset + max_len]
