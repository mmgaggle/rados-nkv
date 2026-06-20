# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""ctypes binding for the GPU-direct NVMe-KV host shim (bead spdk-p9k.1.1).

Loads ``libkvg_gpu.so`` (built from ``gpu_direct/build_kvg.sh``, which compiles
``gpu_direct/kvg_gpu.hip`` against SPDK + HIP + HSA) and declares the GPU-direct
shim's C ABI:

    kvg_open / kvg_close
    kvg_buf_alloc / kvg_buf_free / kvg_buf_dptr / kvg_buf_copy_to_host
    kvg_retrieve_gpu
    kvg_max_value_len

The point of this leg: a host-side NVMe-KV Retrieve DMAs *straight into a
hipMalloc GPU buffer* (registered as a vfio-user dma-buf DMA region). The Python
loader then wraps the resulting HIP device pointer as a **device-resident**
``torch.Tensor`` via ``__cuda_array_interface__`` — so vLLM's
``model.load_weights`` performs no implicit H2D copy.

This module is an OPTIONAL extra: imported only on demand (by the loader's
GPU-direct path), never by the package ``__init__``. Importing it without the
``.so`` raises :class:`KvgShimUnavailable`.
"""

import ctypes
import os
from typing import Optional, Tuple

#: NVMe-KV logical status: the requested key is not present.
KV_KEY_DOES_NOT_EXIST = 0x87


class KvgShimUnavailable(ImportError):
    """The native ``libkvg_gpu.so`` could not be found or loaded.

    Build it with ``gpu_direct/build_kvg.sh`` and either leave it in
    ``gpu_direct/`` (the build default, searched here) or point
    ``RADOSNKV_KVG_LIB`` at the resulting ``.so``.
    """


_LIB_NAME = "libkvg_gpu.so"


def _candidate_paths() -> Tuple[str, ...]:
    """Locations searched for the GPU shim ``.so``, in priority order."""
    cands = []
    env = os.environ.get("RADOSNKV_KVG_LIB")
    if env:
        cands.append(env)
    here = os.path.dirname(os.path.abspath(__file__))
    # src/rados_nkv_weights/_kvshim_gpu.py -> repo_root(vllm-weights)/gpu_direct/<lib>
    repo_root = os.path.dirname(os.path.dirname(here))
    cands.append(os.path.join(repo_root, "gpu_direct", _LIB_NAME))
    cands.append(os.path.join(here, _LIB_NAME))
    cands.append(_LIB_NAME)
    return tuple(cands)


_lib = None


def _load_lib() -> ctypes.CDLL:
    """Load and memoize the GPU shim ``.so``, declaring all signatures."""
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
        except OSError as exc:
            last_err = exc
    if lib is None:
        raise KvgShimUnavailable(
            f"could not load {_LIB_NAME}; tried: {tried}. "
            f"Build it with `gpu_direct/build_kvg.sh` or set RADOSNKV_KVG_LIB. "
            f"Last error: {last_err}"
        )

    # struct kvg_dev *kvg_open(const char *vfu_addr)
    lib.kvg_open.argtypes = [ctypes.c_char_p]
    lib.kvg_open.restype = ctypes.c_void_p

    # void kvg_close(struct kvg_dev *)
    lib.kvg_close.argtypes = [ctypes.c_void_p]
    lib.kvg_close.restype = None

    # struct kvg_gpu_buf *kvg_buf_alloc(struct kvg_dev *, size_t)
    lib.kvg_buf_alloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.kvg_buf_alloc.restype = ctypes.c_void_p

    # void kvg_buf_free(struct kvg_gpu_buf *)
    lib.kvg_buf_free.argtypes = [ctypes.c_void_p]
    lib.kvg_buf_free.restype = None

    # void *kvg_buf_dptr(struct kvg_gpu_buf *)
    lib.kvg_buf_dptr.argtypes = [ctypes.c_void_p]
    lib.kvg_buf_dptr.restype = ctypes.c_void_p

    # int kvg_buf_copy_to_host(struct kvg_gpu_buf *, void *, size_t)
    lib.kvg_buf_copy_to_host.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
    ]
    lib.kvg_buf_copy_to_host.restype = ctypes.c_int

    # int kvg_retrieve_gpu(dev, nsid, key, key_len, buf, got_len*)
    lib.kvg_retrieve_gpu.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint8,
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32),
    ]
    lib.kvg_retrieve_gpu.restype = ctypes.c_int

    # uint32_t kvg_max_value_len(struct kvg_dev *)
    lib.kvg_max_value_len.argtypes = [ctypes.c_void_p]
    lib.kvg_max_value_len.restype = ctypes.c_uint32

    _lib = lib
    return lib


def available() -> bool:
    """Return whether the native GPU shim ``.so`` can be loaded."""
    try:
        _load_lib()
        return True
    except KvgShimUnavailable:
        return False


class GpuBuf:
    """A GPU dma-buf region: a hipMalloc buffer registered as a vfio-user DMA
    region, into which an NVMe-KV Retrieve DMAs directly.

    :attr:`dptr` is the raw HIP device pointer (an integer address) the loader
    wraps as a device-resident ``torch.Tensor``.
    """

    def __init__(self, lib: ctypes.CDLL, raw: int, size: int) -> None:
        self._lib = lib
        self._raw = raw  # struct kvg_gpu_buf *
        self.size = size
        self._closed = False

    @property
    def dptr(self) -> int:
        """The HIP device pointer (GPU virtual address) backing this region."""
        if self._closed:
            raise ValueError("dptr on a freed GpuBuf")
        return int(self._lib.kvg_buf_dptr(self._raw))

    def copy_to_host(self, n: int) -> bytes:
        """Copy ``n`` bytes out of the GPU region to host (D2H). Verify-only."""
        if self._closed:
            raise ValueError("copy_to_host on a freed GpuBuf")
        out = (ctypes.c_uint8 * n)()
        rc = int(self._lib.kvg_buf_copy_to_host(self._raw, out, n))
        if rc != 0:
            raise OSError(f"kvg_buf_copy_to_host failed rc={rc}")
        return bytes(out)

    def free(self) -> None:
        if self._closed:
            return
        self._lib.kvg_buf_free(self._raw)
        self._closed = True
        self._raw = None


class KvgHandle:
    """An open GPU-direct shim handle (one VFIOUSER controller + GPU dma-buf
    registration path).

    Single-instance / single-lifetime: like the CPU shim, it inits the SPDK env
    once per process; DPDK cannot re-init, so at most one handle per process.
    """

    def __init__(self, lib: ctypes.CDLL, raw: int) -> None:
        self._lib = lib
        self._raw = raw  # struct kvg_dev *
        self._closed = False

    @classmethod
    def open(cls, vfu_addr: str) -> "KvgHandle":
        """Init SPDK env + attach the controller at ``vfu_addr`` over VFIOUSER."""
        lib = _load_lib()
        raw = lib.kvg_open(vfu_addr.encode("utf-8"))
        if not raw:
            raise OSError(f"kvg_open(vfu_addr={vfu_addr!r}) failed")
        return cls(lib, raw)

    def alloc_buf(self, size: int) -> GpuBuf:
        """Allocate + register a ``size``-byte GPU dma-buf DMA region."""
        if self._closed:
            raise ValueError("alloc_buf on a closed KvgHandle")
        raw = self._lib.kvg_buf_alloc(self._raw, size)
        if not raw:
            raise MemoryError(f"kvg_buf_alloc({size}) failed")
        return GpuBuf(self._lib, raw, size)

    def retrieve_gpu(
        self, key: bytes, buf: GpuBuf, nsid: int = 0
    ) -> Tuple[int, int]:
        """KV Retrieve ``key`` directly INTO the GPU region ``buf``.

        Returns ``(rc, true_len)``. ``rc`` 0 == success and ``true_len`` is the
        device-reported value length (cdw0). ``rc == 0x87`` == key absent.
        """
        if self._closed:
            raise ValueError("retrieve_gpu on a closed KvgHandle")
        key_buf = (ctypes.c_uint8 * len(key)).from_buffer_copy(key)
        got = ctypes.c_uint32(0)
        rc = int(self._lib.kvg_retrieve_gpu(
            self._raw, nsid, key_buf, len(key), buf._raw, ctypes.byref(got)))
        return rc, int(got.value)

    def close(self) -> None:
        if self._closed:
            return
        self._lib.kvg_close(self._raw)
        self._closed = True
        self._raw = None

    def __enter__(self) -> "KvgHandle":
        return self

    def __exit__(self, *exc) -> None:
        self.close()
