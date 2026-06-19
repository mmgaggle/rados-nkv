# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""Real NVMe-KV transport :class:`KvClient` (ADR-0008).

:class:`NvmeKvClient` is a concrete :class:`~rados_nkv_weights.kvclient.KvClient`
that talks to a real NVMe-KV namespace over the SPDK in-process host shim
(``kv_host_shim`` / :mod:`rados_nkv_weights._kvshim`). It replaces the dev-only
:class:`~rados_nkv_weights.kvclient.InMemoryKvClient` with the genuine wire
transport.

Per ADR-0008 the namespace has an asymmetric read/write split, so this client
comes in two flavours wrapping the SAME namespace:

- :meth:`open_loader` — a **read-only loader** handle: only
  :meth:`retrieve` / :meth:`exists`. :meth:`store` raises (enforced client-side
  here, defense-in-depth; the target-side read-only namespace is the separate
  issue spdk-2xl).
- :meth:`open_publisher` — an **admin publisher** handle that additionally
  permits :meth:`store`.

This module is an OPTIONAL extra. It is imported lazily (never by the package
``__init__``), and importing it requires the native shim ``.so``
(:mod:`rados_nkv_weights._kvshim`); without it, ``import rados_nkv_weights``
still works and the pure-Python core is unaffected.

All keys are raw 16-byte :class:`bytes`
(:data:`rados_nkv_weights.config.KEY_LEN`); all values are :class:`bytes`.
"""

from typing import Iterable, Optional

from .config import KEY_LEN
from .kvclient import KvClient
from ._kvshim import ShimHandle, KV_KEY_DOES_NOT_EXIST


class ReadOnlyError(PermissionError):
    """A write (:meth:`NvmeKvClient.store`) was attempted on a read-only
    (loader) client. Client-side guard mirroring the target-side read-only
    namespace (ADR-0008)."""


class NvmeKvClient(KvClient):
    """A :class:`KvClient` backed by a real NVMe-KV namespace via the SPDK shim.

    Construct via the :meth:`open_loader` / :meth:`open_publisher` factories
    rather than directly, so the read/write privilege flavour is explicit.
    """

    def __init__(self, handle: ShimHandle, read_only: bool) -> None:
        self._h = handle
        #: When True this is a loader handle and :meth:`store` is rejected.
        self.read_only = read_only

    # -- factories ---------------------------------------------------------

    @classmethod
    def open_loader(cls, vfu_addr: str, nsid: int = 0) -> "NvmeKvClient":
        """Open a **read-only loader** client (retrieve/exists only).

        ``vfu_addr`` is the VFIOUSER transport address (the directory containing
        the controller socket). ``nsid=0`` binds the first CSI==KV namespace.
        """
        handle = ShimHandle.open(vfu_addr, nsid=nsid, name="rados_nkv_loader")
        return cls(handle, read_only=True)

    @classmethod
    def open_publisher(cls, vfu_addr: str, nsid: int = 0) -> "NvmeKvClient":
        """Open an **admin publisher** client (retrieve/exists **and** store)."""
        handle = ShimHandle.open(vfu_addr, nsid=nsid, name="rados_nkv_publisher")
        return cls(handle, read_only=False)

    # -- namespace info ----------------------------------------------------

    @property
    def max_value_len(self) -> int:
        """Max value length (kvvml) the bound KV namespace advertises."""
        return self._h.max_value_len

    @property
    def max_key_len(self) -> int:
        """Max key length (kvkml) the bound KV namespace advertises."""
        return self._h.max_key_len

    # -- lifecycle ---------------------------------------------------------

    def close(self) -> None:
        """Close the underlying shim handle (idempotent)."""
        self._h.close()

    def __enter__(self) -> "NvmeKvClient":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- KvClient ----------------------------------------------------------

    @staticmethod
    def _check_key(key) -> bytes:
        """Validate the key is exactly ``KEY_LEN`` raw bytes.

        Guards both the catalog's 16-byte invariant and the shim's ``uint8``
        ``key_len`` (a >255-byte key would otherwise wrap mod 256 and alias).
        """
        if not isinstance(key, (bytes, bytearray)):
            raise TypeError(f"key must be bytes, got {type(key).__name__}")
        if len(key) != KEY_LEN:
            raise ValueError(f"key must be exactly {KEY_LEN} bytes, got {len(key)}")
        return bytes(key)

    def store(self, key: bytes, value: bytes) -> None:
        """NVMe-KV Store ``value`` under ``key`` (publisher only).

        Raises :class:`ReadOnlyError` on a loader client (client-side guard).
        Raises :class:`OSError` on a non-zero shim status.
        """
        if self.read_only:
            raise ReadOnlyError(
                "store() rejected: this is a read-only (loader) NvmeKvClient; "
                "use open_publisher() for the admin write path"
            )
        key = self._check_key(key)
        rc = self._h.store(key, bytes(value))
        if rc != 0:
            raise OSError(f"NVMe-KV Store failed for key {bytes(key).hex()}: rc={rc}")

    def retrieve(self, key: bytes) -> Optional[bytes]:
        """NVMe-KV Retrieve the value for ``key`` (true-length bytes), or
        ``None`` if the key does not exist.

        Sizes the DMA retrieve buffer to the namespace ``max_value_len`` and
        returns exactly the device-reported value length.
        """
        key = self._check_key(key)
        buf_len = self._h.max_value_len
        rc, data = self._h.retrieve(key, buf_len)
        if rc == KV_KEY_DOES_NOT_EXIST:
            return None
        if rc != 0:
            raise OSError(
                f"NVMe-KV Retrieve failed for key {bytes(key).hex()}: rc={rc}")
        return data

    def exists(self, key: bytes) -> bool:
        """NVMe-KV Exist for ``key`` (``0x87`` == absent)."""
        key = self._check_key(key)
        rc = self._h.exist(key)
        if rc == 0:
            return True
        if rc == KV_KEY_DOES_NOT_EXIST:
            return False
        raise OSError(
            f"NVMe-KV Exist failed for key {bytes(key).hex()}: rc={rc}")

    def delete(self, key: bytes) -> None:
        """NVMe-KV Delete ``key`` (publisher/admin only).

        Mirrors :meth:`store`'s privilege split: raises :class:`ReadOnlyError`
        on a loader client (client-side guard; the target-side read-only
        namespace also forbids Delete). Deleting an absent key
        (``KEY_DOES_NOT_EXIST``) is treated as a successful no-op so the GC
        sweep is idempotent. Raises :class:`OSError` on any other non-zero shim
        status.
        """
        if self.read_only:
            raise ReadOnlyError(
                "delete() rejected: this is a read-only (loader) NvmeKvClient; "
                "use open_publisher() for the admin write path"
            )
        key = self._check_key(key)
        rc = self._h.delete(key)
        if rc != 0 and rc != KV_KEY_DOES_NOT_EXIST:
            raise OSError(
                f"NVMe-KV Delete failed for key {bytes(key).hex()}: rc={rc}")

    def iter_keys(self) -> Iterable[bytes]:
        """NOT available over NVMe-KV: enumeration is deferred (ADR-0009).

        NVMe-KV ``List`` is not implemented on the librados backend, so a
        loader/publisher handle cannot enumerate the namespace over the wire.
        Garbage collection therefore needs an out-of-band enumeration source.

        Production strategy: run the GC sweep in the rados-side admin path, where
        the namespace's backing objects can be enumerated directly with
        ``rados ls`` on the pool/namespace (each NVMe-KV key maps to a rados
        object name), OR maintain a separate catalog index object (a key list
        updated on Store/Delete) that the admin path reads to drive the sweep.
        Either way the live-set computation and sweep logic in
        :func:`rados_nkv_weights.gc.gc` are reused unchanged — only the key
        enumeration source differs.
        """
        raise NotImplementedError(
            "iter_keys() is unavailable over NVMe-KV: List is deferred on the "
            "librados backend (ADR-0009). Drive GC enumeration from the "
            "rados-side admin path (`rados ls`) or a maintained catalog index; "
            "see the NvmeKvClient.iter_keys docstring."
        )
