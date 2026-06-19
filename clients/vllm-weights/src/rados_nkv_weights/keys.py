# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""NVMe-KV key derivation for the Weights catalog.

Two kinds of 16-byte raw keys live in the catalog namespace, kept apart by a
**structural** 1-byte domain tag that is the first byte of every key:

- **Chunk key** — ``b"\\x01" + blake2b(chunk, digest_size=15)``: a *content hash*
  of a Weight chunk's bytes. Identical bytes across Model revisions / Precision
  variants / models hash to the same key, so they are stored once (dedup via
  Exist-before-Store). The tag byte is a constant, so the key is still a pure
  function of the chunk content — dedup is preserved.
- **Manifest key** — ``b"\\x00" + blake2b(canonical_revision(rev), digest_size=15)``:
  a *deterministic* digest of the canonicalized Model revision identity, so a
  freshly-attached host finds the Weight manifest with **no enumeration** (List
  is deferred on the librados backend).

Reserving distinct leading tag bytes (``\\x00`` manifest, ``\\x01`` chunk) makes
the two keyspaces disjoint by construction — not merely by an in-preimage
prefix. The remaining 15 bytes (120-bit digest) are collision-safe for content
addressing. Every key stays exactly :data:`rados_nkv_weights.config.KEY_LEN`
(16) raw :class:`bytes`.
"""

import hashlib
import unicodedata

from .config import KEY_LEN

#: Domain tag (first key byte) for a Manifest key.
MANIFEST_TAG = b"\x00"
#: Domain tag (first key byte) for a Chunk key.
CHUNK_TAG = b"\x01"
#: blake2b digest size for the tagged body (KEY_LEN minus the 1-byte tag).
_DIGEST_SIZE = KEY_LEN - 1


def canonical_revision(s: str) -> str:
    """Canonicalize a Model revision string so publisher and loader resolve the
    same Manifest key despite cosmetic formatting differences.

    Applies Unicode NFC normalization, strips surrounding whitespace, and strips
    a single trailing ``'/'``. Case and internal slashes are **preserved** —
    HuggingFace ids are case-sensitive and slash-structured, so ``'Model@v1'``
    and ``'model@v1'`` (and ``'a/b'`` vs ``'a//b'``) stay distinct.
    """
    out = unicodedata.normalize("NFC", s).strip()
    if out.endswith("/"):
        out = out[:-1]
    return out


def chunk_key(chunk: bytes) -> bytes:
    """Return the content-addressed Chunk key for a Weight chunk's bytes.

    ``CHUNK_TAG + blake2b(chunk, digest_size=15)`` — a constant tag byte followed
    by a 15-byte (120-bit) content digest, :data:`KEY_LEN` (16) bytes total.
    Because the tag is constant and the digest is purely a function of the chunk
    content, two identical chunks — even across different models — produce the
    same key, which is what makes Exist-before-Store dedup work without a
    refcount table.
    """
    return CHUNK_TAG + hashlib.blake2b(chunk, digest_size=_DIGEST_SIZE).digest()


def manifest_key(model_revision: str) -> bytes:
    """Return the deterministic Manifest key for a Model revision.

    ``MANIFEST_TAG + blake2b(canonical_revision(rev), digest_size=15)`` — a
    constant tag byte (distinct from the Chunk-key tag) followed by a 15-byte
    digest of the *canonicalized* (:func:`canonical_revision`) revision string,
    :data:`KEY_LEN` (16) bytes total. Deterministic so any host can compute the
    key from the Model revision string alone and Retrieve the Weight manifest
    directly — no List required. The distinct leading tag byte makes the manifest
    keyspace structurally disjoint from the content-hash Chunk keyspace.
    """
    canon = canonical_revision(model_revision)
    digest = hashlib.blake2b(
        canon.encode("utf-8"), digest_size=_DIGEST_SIZE
    ).digest()
    return MANIFEST_TAG + digest
