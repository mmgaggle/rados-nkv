# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""The pluggable NVMe-KV transport (ADR-0008).

The Weights catalog is one shared NVMe-KV namespace. This module defines the
abstract :class:`KvClient` seam plus an in-memory implementation for dev/tests.

Per ADR-0008 the namespace has an asymmetric read/write split:

- **Weights loader** (read path) attaches the namespace **read-only** and uses
  only :meth:`KvClient.retrieve` / :meth:`KvClient.exists` (NVMe-KV
  ``Retrieve`` / ``Exist``). The target rejects ``Store``/``Delete``/``Exec``.
- **Weights publisher** (write path) uses a privileged **admin NVMe-KV**
  connection permitted to :meth:`KvClient.store` (NVMe-KV ``Store``).

A production client therefore comes in two flavours wrapping the same namespace:
a read-only loader client and an admin publisher client. Implementing those
against a real NVMe-KV target is a **documented TODO**; the only concrete client
shipped today is :class:`InMemoryKvClient`.

All keys are raw 16-byte :class:`bytes`
(:data:`rados_nkv_weights.config.KEY_LEN`); all values are :class:`bytes`.
"""

import abc
from typing import Dict, Iterable, Optional

from .config import DEFAULT_MAX_VALUE_LEN


class KvClient(abc.ABC):
    """Abstract NVMe-KV transport for the Weights catalog namespace.

    Production subclasses (TODO) wrap a real NVMe-KV namespace: a read-only
    handle for the Weights loader (:meth:`retrieve`/:meth:`exists`) and an admin
    handle for the Weights publisher (:meth:`store`).
    """

    @property
    def max_value_len(self) -> int:
        """Max length, in bytes, of a single Value this namespace accepts.

        The NVMe-KV namespace reports this as ``kvvml`` (KV value max length).
        The Weights publisher uses it as the chunk/packing cap so no stored
        Value exceeds what the device accepts. The abstract base returns the
        catalog default (:data:`config.DEFAULT_MAX_VALUE_LEN`); real clients
        (e.g. :class:`~rados_nkv_weights.nvmekv_client.NvmeKvClient`) override
        this with the device-advertised value.
        """
        return DEFAULT_MAX_VALUE_LEN

    @abc.abstractmethod
    def store(self, key: bytes, value: bytes) -> None:
        """Store ``value`` under ``key`` (NVMe-KV ``Store``; publisher only).

        Production note: only the admin/publisher namespace handle may call
        this; the loader's read-only handle would reject it at the target.
        """

    @abc.abstractmethod
    def retrieve(self, key: bytes) -> Optional[bytes]:
        """Return the Value for ``key`` (NVMe-KV ``Retrieve``), or ``None`` if
        the key is absent."""

    @abc.abstractmethod
    def exists(self, key: bytes) -> bool:
        """Return whether ``key`` is present (NVMe-KV ``Exist``).

        Used by the publisher for Exist-before-Store dedup (ADR-0009).
        """

    @abc.abstractmethod
    def delete(self, key: bytes) -> None:
        """Delete ``key`` (NVMe-KV ``Delete``; publisher/admin only).

        Production note: like :meth:`store`, only the admin/publisher namespace
        handle may call this; the loader's read-only handle rejects it (the
        target-side read-only namespace forbids ``Delete`` per ADR-0008). Used
        by garbage collection (:mod:`rados_nkv_weights.gc`) to sweep orphaned
        chunks/manifests. Deleting an absent key is a no-op.
        """

    @abc.abstractmethod
    def iter_keys(self) -> Iterable[bytes]:
        """Enumerate every key present in the namespace (admin enumeration).

        Used by garbage collection (:mod:`rados_nkv_weights.gc`) to find
        orphaned keys: everything enumerated that is not in the live-set is a
        sweep candidate. NVMe-KV ``List`` is deferred on the librados backend
        (ADR-0009), so a real client may not be able to provide this over the
        wire — see :meth:`NvmeKvClient.iter_keys` for the production strategy.
        """


class InMemoryKvClient(KvClient):
    """A dict-backed :class:`KvClient` for development and tests.

    Not a real NVMe-KV transport — there is no namespace, no privilege split,
    and no persistence. It records :attr:`store_calls` (the number of successful
    :meth:`store` invocations) so tests can assert dedup behaviour: a chunk that
    already Exists is skipped by the publisher and never reaches :meth:`store`.

    ``max_value_len`` is configurable so tests can exercise the publisher's
    kvvml-derived chunking/packing cap with a small bound (it defaults to
    :data:`config.DEFAULT_MAX_VALUE_LEN`).
    """

    def __init__(self, max_value_len: int = DEFAULT_MAX_VALUE_LEN) -> None:
        self._data: Dict[bytes, bytes] = {}
        #: Count of successful store() calls — used to assert dedup skips.
        self.store_calls: int = 0
        self._max_value_len = int(max_value_len)

    @property
    def max_value_len(self) -> int:
        """The configured Value-length cap this dev client reports as kvvml."""
        return self._max_value_len

    def store(self, key: bytes, value: bytes) -> None:
        if not isinstance(key, (bytes, bytearray)):
            raise TypeError(f"key must be bytes, got {type(key).__name__}")
        self._data[bytes(key)] = bytes(value)
        self.store_calls += 1

    def retrieve(self, key: bytes) -> Optional[bytes]:
        return self._data.get(bytes(key))

    def exists(self, key: bytes) -> bool:
        return bytes(key) in self._data

    def delete(self, key: bytes) -> None:
        if not isinstance(key, (bytes, bytearray)):
            raise TypeError(f"key must be bytes, got {type(key).__name__}")
        # Deleting an absent key is a no-op (idempotent), mirroring NVMe-KV
        # Delete returning KEY_DOES_NOT_EXIST without error to the GC sweep.
        self._data.pop(bytes(key), None)

    def iter_keys(self) -> Iterable[bytes]:
        # Snapshot so callers may delete during iteration (the GC sweep does).
        return list(self._data.keys())

    def __len__(self) -> int:
        return len(self._data)
