# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""ctypes binding for the SPDK in-process NVMe-KV host shim.

Loads ``libradosnkv_kvshim.so`` (built from ``native/build.sh``, which compiles
SPDK's ``kv_host_shim.c`` against the SPDK + DPDK static libraries) and declares
the shim's public C ABI:

    kv_host_shim_open / kv_host_shim_close
    kv_host_shim_dma_alloc / kv_host_shim_dma_free
    kv_host_shim_max_value_len / kv_host_shim_max_key_len
    kv_host_shim_store / kv_host_shim_retrieve / kv_host_shim_exist / kv_host_shim_delete

This module is an OPTIONAL extra: it is only imported on demand (by
:mod:`rados_nkv_weights.nvmekv_client`), never by the package's top-level
``__init__``, so ``import rados_nkv_weights`` keeps working with no SPDK / native
library present. Importing this module without the ``.so`` raises
:class:`KvShimUnavailable`.

The shim's return convention (mirrored here):

- ``0``  — SUCCESS (NVMe status 0x00).
- positive — an NVMe-KV *logical* status code, e.g. ``0x87``
  (``KEY_DOES_NOT_EXIST``).
- negative — a negated errno for submit-/transport-level errors.
"""

import ctypes
import os
from typing import Optional, Tuple

#: NVMe-KV logical status: the requested key is not present.
KV_KEY_DOES_NOT_EXIST = 0x87


class KvShimUnavailable(ImportError):
    """The native ``libradosnkv_kvshim.so`` could not be found or loaded.

    Build it with ``native/build.sh`` (``SPDK_ROOT=/path/to/spdk
    native/build.sh``) and either install it next to this package or point
    ``RADOSNKV_KVSHIM_LIB`` at the resulting ``.so``.
    """


_LIB_NAME = "libradosnkv_kvshim.so"


def _candidate_paths() -> Tuple[str, ...]:
    """Locations searched for the shim ``.so``, in priority order.

    1. ``$RADOSNKV_KVSHIM_LIB`` (an explicit path to the ``.so``).
    2. ``native/`` beside the installed/checked-out package (the build default).
    3. The bare soname, leaving it to the dynamic loader / ``LD_LIBRARY_PATH``.
    """
    cands = []
    env = os.environ.get("RADOSNKV_KVSHIM_LIB")
    if env:
        cands.append(env)
    here = os.path.dirname(os.path.abspath(__file__))
    # src/rados_nkv_weights/_kvshim.py -> repo_root/native/<lib>
    repo_root = os.path.dirname(os.path.dirname(here))
    cands.append(os.path.join(repo_root, "native", _LIB_NAME))
    # Also allow the lib to sit directly beside the package.
    cands.append(os.path.join(here, _LIB_NAME))
    cands.append(_LIB_NAME)
    return tuple(cands)


class _KvHostShimOpts(ctypes.Structure):
    """Mirror of ``struct kv_host_shim_opts`` (size-versioned).

    ``opts_size`` MUST be set to ``sizeof(struct kv_host_shim_opts)`` so the
    shim can stay ABI-compatible as fields are appended. The field order/types
    here must match the C header exactly.
    """

    _fields_ = [
        ("opts_size", ctypes.c_size_t),
        ("name", ctypes.c_char_p),
        ("vfu_addr", ctypes.c_char_p),
        ("nsid", ctypes.c_uint32),
        ("init_env", ctypes.c_bool),
    ]


_lib = None


def _load_lib() -> ctypes.CDLL:
    """Load and memoize the shim ``.so``, declaring all signatures.

    Raises :class:`KvShimUnavailable` if no candidate path loads.
    """
    global _lib
    if _lib is not None:
        return _lib

    last_err: Optional[BaseException] = None
    lib = None
    tried = []
    for path in _candidate_paths():
        tried.append(path)
        try:
            lib = ctypes.CDLL(path, mode=ctypes.RTLD_GLOBAL)
            break
        except OSError as exc:  # not found / unresolved deps
            last_err = exc
    if lib is None:
        raise KvShimUnavailable(
            f"could not load {_LIB_NAME}; tried: {tried}. "
            f"Build it with `SPDK_ROOT=/path/to/spdk native/build.sh` or set "
            f"RADOSNKV_KVSHIM_LIB. Last error: {last_err}"
        )

    # int kv_host_shim_open(const struct kv_host_shim_opts *, struct kv_host_shim **)
    lib.kv_host_shim_open.argtypes = [
        ctypes.POINTER(_KvHostShimOpts),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.kv_host_shim_open.restype = ctypes.c_int

    # void kv_host_shim_close(struct kv_host_shim *)
    lib.kv_host_shim_close.argtypes = [ctypes.c_void_p]
    lib.kv_host_shim_close.restype = None

    # void *kv_host_shim_dma_alloc(size_t)
    lib.kv_host_shim_dma_alloc.argtypes = [ctypes.c_size_t]
    lib.kv_host_shim_dma_alloc.restype = ctypes.c_void_p

    # void kv_host_shim_dma_free(void *)
    lib.kv_host_shim_dma_free.argtypes = [ctypes.c_void_p]
    lib.kv_host_shim_dma_free.restype = None

    # uint32_t kv_host_shim_max_value_len(const struct kv_host_shim *)
    lib.kv_host_shim_max_value_len.argtypes = [ctypes.c_void_p]
    lib.kv_host_shim_max_value_len.restype = ctypes.c_uint32

    # uint32_t kv_host_shim_max_key_len(const struct kv_host_shim *)
    lib.kv_host_shim_max_key_len.argtypes = [ctypes.c_void_p]
    lib.kv_host_shim_max_key_len.restype = ctypes.c_uint32

    # int kv_host_shim_store(sh, key, key_len, value, value_len)
    lib.kv_host_shim_store.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint8,
        ctypes.c_void_p, ctypes.c_uint32,
    ]
    lib.kv_host_shim_store.restype = ctypes.c_int

    # int kv_host_shim_retrieve(sh, key, key_len, value, buf_len, value_len_out*)
    lib.kv_host_shim_retrieve.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint8,
        ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32),
    ]
    lib.kv_host_shim_retrieve.restype = ctypes.c_int

    # int kv_host_shim_exist(sh, key, key_len)
    lib.kv_host_shim_exist.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint8,
    ]
    lib.kv_host_shim_exist.restype = ctypes.c_int

    # int kv_host_shim_delete(sh, key, key_len)
    lib.kv_host_shim_delete.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint8,
    ]
    lib.kv_host_shim_delete.restype = ctypes.c_int

    _lib = lib
    return lib


def available() -> bool:
    """Return whether the native shim ``.so`` can be loaded (no exception)."""
    try:
        _load_lib()
        return True
    except KvShimUnavailable:
        return False


# --------------------------------------------------------------------------
# Thin, Pythonic wrappers. These translate to/from the raw C ABI but apply NO
# policy (no read-only enforcement, no key padding) — that lives in
# nvmekv_client.NvmeKvClient.
# --------------------------------------------------------------------------


class ShimHandle:
    """An open shim handle (one VFIOUSER controller + bound KV namespace).

    Wraps the opaque ``struct kv_host_shim *`` and exposes store/retrieve/exist/
    delete plus the namespace max key/value lengths. Use :meth:`open` to create
    and :meth:`close` (or a ``with`` block) to release.

    Single-instance / single-lifetime when ``init_env=True``: DPDK cannot
    re-init the SPDK env in one process, so at most one ``init_env=True`` handle
    can be opened over a process's whole lifetime (see the C header).
    """

    def __init__(self, lib: ctypes.CDLL, raw: ctypes.c_void_p) -> None:
        self._lib = lib
        self._raw = raw  # struct kv_host_shim *
        self._closed = False

    @classmethod
    def open(cls, vfu_addr: str, nsid: int = 0, name: str = "rados_nkv_weights",
             init_env: bool = True) -> "ShimHandle":
        """Probe/attach the controller at ``vfu_addr`` over VFIOUSER and bind
        the KV namespace (``nsid=0`` selects the first CSI==KV namespace)."""
        lib = _load_lib()
        opts = _KvHostShimOpts()
        opts.opts_size = ctypes.sizeof(_KvHostShimOpts)
        opts.name = name.encode("utf-8")
        opts.vfu_addr = vfu_addr.encode("utf-8")
        opts.nsid = nsid
        opts.init_env = init_env
        raw = ctypes.c_void_p()
        rc = lib.kv_host_shim_open(ctypes.byref(opts), ctypes.byref(raw))
        if rc != 0 or not raw:
            raise OSError(
                f"kv_host_shim_open(vfu_addr={vfu_addr!r}, nsid={nsid}) "
                f"failed rc={rc}"
            )
        return cls(lib, raw)

    def close(self) -> None:
        """Close the handle. For an ``init_env=True`` handle this also tears the
        process SPDK env down (single-lifetime). Idempotent."""
        if self._closed:
            return
        self._lib.kv_host_shim_close(self._raw)
        self._closed = True
        self._raw = ctypes.c_void_p()

    def __enter__(self) -> "ShimHandle":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self):  # best-effort
        try:
            self.close()
        except Exception:
            pass

    def _check_open(self) -> None:
        if self._closed or not self._raw:
            raise ValueError("operation on a closed ShimHandle")

    @property
    def max_value_len(self) -> int:
        """Max value length (kvvml) advertised by the bound KV namespace."""
        self._check_open()
        return int(self._lib.kv_host_shim_max_value_len(self._raw))

    @property
    def max_key_len(self) -> int:
        """Max key length (kvkml) advertised by the bound KV namespace."""
        self._check_open()
        return int(self._lib.kv_host_shim_max_key_len(self._raw))

    def store(self, key: bytes, value: bytes) -> int:
        """KV Store ``value`` under ``key``. Returns the shim status code
        (0 == success). DMA-allocs/copies/frees internally."""
        self._check_open()
        lib = self._lib
        key_buf = (ctypes.c_uint8 * len(key)).from_buffer_copy(key)
        vlen = len(value)
        dma = lib.kv_host_shim_dma_alloc(vlen if vlen else 1)
        if not dma:
            raise MemoryError("kv_host_shim_dma_alloc failed")
        try:
            if vlen:
                ctypes.memmove(dma, value, vlen)
            return int(lib.kv_host_shim_store(
                self._raw, key_buf, len(key), dma, vlen))
        finally:
            lib.kv_host_shim_dma_free(dma)

    def retrieve(self, key: bytes, buf_len: int) -> Tuple[int, Optional[bytes]]:
        """KV Retrieve ``key`` into a ``buf_len``-byte DMA buffer.

        Returns ``(rc, value_bytes)``. On success (rc 0) ``value_bytes`` is the
        TRUE-length value (per the completion cdw0, truncated to ``buf_len`` if
        the device value was larger). On any non-zero rc ``value_bytes`` is
        ``None``.
        """
        self._check_open()
        lib = self._lib
        key_buf = (ctypes.c_uint8 * len(key)).from_buffer_copy(key)
        dma = lib.kv_host_shim_dma_alloc(buf_len if buf_len else 1)
        if not dma:
            raise MemoryError("kv_host_shim_dma_alloc failed")
        try:
            vlen_out = ctypes.c_uint32(0)
            rc = int(lib.kv_host_shim_retrieve(
                self._raw, key_buf, len(key), dma, buf_len,
                ctypes.byref(vlen_out)))
            if rc != 0:
                return rc, None
            true_len = int(vlen_out.value)
            # Per the NVMe KV Retrieve contract the device delivered min(true,
            # buf_len) bytes into the buffer; never read past buf_len.
            n = min(true_len, buf_len)
            data = ctypes.string_at(dma, n)
            return rc, data
        finally:
            lib.kv_host_shim_dma_free(dma)

    def exist(self, key: bytes) -> int:
        """KV Exist ``key``. Returns 0 if present, ``0x87`` if absent."""
        self._check_open()
        key_buf = (ctypes.c_uint8 * len(key)).from_buffer_copy(key)
        return int(self._lib.kv_host_shim_exist(self._raw, key_buf, len(key)))

    def delete(self, key: bytes) -> int:
        """KV Delete ``key``. Returns 0 on success, ``0x87`` if absent."""
        self._check_open()
        key_buf = (ctypes.c_uint8 * len(key)).from_buffer_copy(key)
        return int(self._lib.kv_host_shim_delete(self._raw, key_buf, len(key)))
