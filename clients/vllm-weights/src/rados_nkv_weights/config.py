# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""Catalog-wide constants.

These mirror the NVMe-KV / librados backend limits. The defaults are overridable
so tests and alternate backends can use smaller bounds, but production publishers
should keep the librados cap.
"""

#: Length, in bytes, of every NVMe-KV Key used by this catalog.
#:
#: Both Chunk keys (content hash, :func:`rados_nkv_weights.keys.chunk_key`) and
#: Manifest keys (:func:`rados_nkv_weights.keys.manifest_key`) are exactly this
#: many raw bytes. Each key is a 1-byte structural domain tag (``\x00`` manifest,
#: ``\x01`` chunk) followed by a 15-byte (120-bit) BLAKE2b digest — disjoint
#: keyspaces by construction, with a digest still ample for content addressing.
KEY_LEN = 16

#: Maximum length, in bytes, of a single NVMe-KV Value (a Weight chunk).
#:
#: The librados kvvml backend caps a Value at 64 MiB. Tensors larger
#: than this span multiple Weight chunks listed in order by the Weight manifest
#:. Overridable per call for tests / alternate backends.
DEFAULT_MAX_VALUE_LEN = 64 * 1024 * 1024
