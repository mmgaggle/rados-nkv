# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""vLLM ``--load-format`` integration for the Weights catalog.

This is the real, key-native vLLM model loader: a standalone
``--load-format=rados-nkv`` plugin ("a standalone key-native vLLM
``--load-format`` plugin, not an extension of Run:ai Model Streamer"). Given a
:class:`~vllm.config.ModelConfig` it derives a Model revision + Precision
variant, Retrieves the Weight manifest, and streams each tensor's Weight chunks
out of a **read-only** NVMe-KV namespace, reassembling them into per-tensor
``torch.Tensor`` objects that it hands to the model's own ``load_weights`` —
which applies tensor-parallel sharding and copies into device memory.

**torch / vllm are optional.** This module imports them *lazily*, inside the
functions that need them; merely importing :mod:`rados_nkv_weights.vllm_loader`
neither requires nor imports torch or vllm. The core package
(:mod:`rados_nkv_weights`) does not import this module at top level, so the
pure-Python read path keeps working with neither installed. Call
:func:`register` (which imports vllm) only from a process that actually runs
vLLM — e.g. a ``vllm.plugins`` general-plugin entrypoint.

How the pieces map
------------------

vLLM's loader contract (``vllm.model_executor.model_loader``):

* ``register_model_loader(load_format)`` is a decorator that registers a
  :class:`~vllm.model_executor.model_loader.base_loader.BaseModelLoader`
  subclass under a ``--load-format`` string.
* ``BaseModelLoader`` requires two methods: ``download_model(model_config)`` and
  ``load_weights(model, model_config)``. Its concrete ``load_model`` initializes
  the empty model on the device and then calls ``self.load_weights(model,
  model_config)``.
* A vLLM model exposes ``model.load_weights(weights)`` where ``weights`` is an
  iterable of ``(name, torch.Tensor)`` tuples; the model maps each name onto its
  parameters and applies TP sharding. This is the same contract
  ``DefaultModelLoader`` feeds from safetensors.

So our :class:`RadosNkvModelLoader.load_weights` simply builds that
``(name, tensor)`` generator from the catalog and passes it to
``model.load_weights`` — chunk-streaming becomes the weight iterator.
"""

from typing import Dict, Iterator, Optional, Tuple

from .keys import canonical_revision
from .kvclient import KvClient
from .loader import _load_manifest, _reassemble
from .manifest import WeightManifest

#: The ``--load-format`` string this plugin registers under.
LOAD_FORMAT = "rados-nkv"


# ---------------------------------------------------------------------------
# dtype mapping: manifest dtype string -> torch dtype
# ---------------------------------------------------------------------------
#
# The manifest records each Precision variant's dtype as a string (the numpy
# dtype name the publisher wrote, e.g. "float16", "bfloat16", "float8_e4m3fn",
# "uint8"). To build a torch.Tensor we map that string to a torch dtype. This
# covers dtypes numpy cannot represent (bf16, fp8) — the reason we go straight to
# torch here rather than reusing load_arrays' numpy path.


def _torch_dtype(torch, dtype_str: str):
    """Resolve a manifest dtype string to a ``torch.dtype``.

    Accepts both bare names (``"float16"``) and ``"torch."``-prefixed names.
    Falls back to ``getattr(torch, name)`` so any dtype the installed torch
    exposes works without an exhaustive table; raises a clear error otherwise.
    """
    name = dtype_str
    if name.startswith("torch."):
        name = name[len("torch."):]
    # A small set of friendly aliases the publisher / catalog may emit.
    aliases = {
        "fp16": "float16",
        "fp32": "float32",
        "fp64": "float64",
        "bf16": "bfloat16",
        "half": "float16",
        "float": "float32",
        "double": "float64",
    }
    name = aliases.get(name, name)
    dt = getattr(torch, name, None)
    if dt is None or not isinstance(dt, torch.dtype):
        raise ValueError(
            f"manifest dtype {dtype_str!r} does not map to a torch.dtype "
            f"(resolved name {name!r}); install a torch that supports it or "
            f"republish the catalog with a torch-representable dtype"
        )
    return dt


def iter_named_tensors(
    kv: KvClient,
    model_revision: str,
    precision: str,
    *,
    manifest: Optional[WeightManifest] = None,
) -> Iterator[Tuple[str, "object"]]:
    """Stream ``(tensor_name, torch.Tensor)`` for one Model revision/Precision.

    This is the chunk-streaming-into-tensors core, framed as vLLM's weight
    iterator. For each tensor published at ``precision`` it: retrieves the
    tensor's Weight chunks (with the loader's content-hash integrity check and
    cross-tensor Value caching, via :func:`loader._reassemble`), then wraps the
    reassembled bytes in a ``torch.Tensor`` of the manifest's dtype and reshapes
    to the manifest's shape.

    ``torch`` is imported lazily here; importing this module does not require it.

    The tensor's storage is the reassembled ``bytes`` buffer
    (``torch.frombuffer`` is zero-copy over a read-only buffer); vLLM's
    ``model.load_weights`` copies it into the (possibly sharded) device
    parameter, so the transient host tensor is freed as loading proceeds.

    Pass a pre-read ``manifest`` to avoid a second Manifest-key Retrieve when the
    caller already holds it.
    """
    import torch  # lazy: only needed when actually streaming into tensors

    if manifest is None:
        manifest = _load_manifest(kv, model_revision)

    # _reassemble does the manifest precision check, the per-chunk Retrieve, the
    # content-hash integrity verification, and the length check — exactly the
    # same read path load()/load_arrays() use. We then re-dtype/reshape to torch.
    raw: Dict[str, bytes] = _reassemble(kv, manifest, model_revision, precision)

    for name, data in raw.items():
        dtype_str, shape, _sizes = manifest.tensor_meta(name, precision)
        td = _torch_dtype(torch, dtype_str)
        # frombuffer over a bytearray copy of the reassembled bytes: torch needs a
        # writable buffer (a bare bytes is read-only and warns), so we hand it a
        # mutable copy; the model copies into its own device params from here.
        flat = torch.frombuffer(bytearray(data), dtype=td)
        tensor = flat.reshape(tuple(shape)) if shape else flat.reshape(())
        yield name, tensor


# ---------------------------------------------------------------------------
# GPU-direct emit: device-resident tensors via the Rung-1 dma-buf datapath
# ---------------------------------------------------------------------------
#
# Instead of torch.frombuffer(bytearray(data)) (a HOST tensor + an implicit H2D
# copy inside model.load_weights), the GPU-direct path Retrieves each catalog
# Value DIRECTLY into a hipMalloc GPU buffer (exported as a dma-buf and
# registered as a vfio-user DMA region; see gpu_direct/kvg_gpu.hip, bead
# spdk-p9k.1) and wraps the resulting HIP device pointer as a device-resident
# torch.Tensor via __cuda_array_interface__. So the weight bytes are born in GPU
# memory and model.load_weights does no host bounce.
#
# The transport for this leg is the GPU shim (rados_nkv_weights._kvshim_gpu),
# NOT the SPDK-NVMe kv_host_shim the CPU NvmeKvClient uses, because the GPU shim
# is the one that owns the vfio_device and can register the dma-buf region.


def _cai_typestr(torch, dt) -> Optional[str]:
    """Map a torch dtype to a ``__cuda_array_interface__`` typestr, or None.

    CAI cannot express bf16 / fp8 (no numpy kind for them). For those we wrap the
    GPU bytes as uint8 and ``.view(dtype)`` on the device tensor instead — see
    :func:`_wrap_gpu_buf_as_tensor`.
    """
    table = {
        torch.float64: "<f8",
        torch.float32: "<f4",
        torch.float16: "<f2",
        torch.int64: "<i8",
        torch.int32: "<i4",
        torch.int16: "<i2",
        torch.int8: "|i1",
        torch.uint8: "|u1",
        torch.bool: "|b1",
    }
    return table.get(dt)


def _wrap_gpu_buf_as_tensor(torch, dptr: int, nbytes: int, dt, shape):
    """Wrap a raw HIP device pointer as a device-resident ``torch.Tensor``.

    ``dptr`` is the GPU virtual address of ``nbytes`` valid bytes (the Retrieve
    DMA already landed them there). Returns a tensor of dtype ``dt`` reshaped to
    ``shape``, with ``.is_cuda == True``, viewing those exact GPU bytes (no host
    bounce, no extra device copy beyond what reshape/view need).

    For dtypes CAI can express we hand torch a CAI object directly. For bf16/fp8
    (which CAI's typestr cannot name) we wrap the bytes as a uint8 device tensor
    and bit-cast with ``.view(dt)``.
    """
    typestr = _cai_typestr(torch, dt)
    if typestr is not None:
        itemsize = torch.empty(0, dtype=dt).element_size()
        count = nbytes // itemsize

        class _Cai:
            __cuda_array_interface__ = {
                "data": (dptr, False),  # (ptr, read_only=False)
                "shape": (count,),
                "typestr": typestr,
                "version": 3,
                "strides": None,
            }

        flat = torch.as_tensor(_Cai(), device="cuda")
    else:
        # bf16 / fp8: wrap raw bytes as uint8 on device, then bit-cast.
        class _CaiU8:
            __cuda_array_interface__ = {
                "data": (dptr, False),
                "shape": (nbytes,),
                "typestr": "|u1",
                "version": 3,
                "strides": None,
            }

        flat_u8 = torch.as_tensor(_CaiU8(), device="cuda")
        flat = flat_u8.view(dt)
    return flat.reshape(tuple(shape)) if shape else flat.reshape(())


class _KvgManifestClient(KvClient):
    """Minimal :class:`KvClient` over a ``KvgHandle`` for the small HOST reads.

    :func:`iter_named_tensors_gpu` uses its ``kv`` argument only to read the
    Arrow manifest and to report ``kv.max_value_len`` (to size the GPU regions);
    the bulk weight bytes go through ``kvg`` straight to GPU memory. This adapter
    serves those small reads through the SAME ``KvgHandle`` (Retrieve into a GPU
    buffer, then copy the small manifest bytes to host), so the whole GPU-direct
    flow needs only the single ``KvgHandle`` SPDK env — DPDK permits exactly one
    SPDK env per process, so we cannot also open an NvmeKvClient.
    """

    def __init__(self, kvg, max_value_len: Optional[int] = None) -> None:
        self._kvg = kvg
        if max_value_len is None:
            from .config import DEFAULT_MAX_VALUE_LEN

            max_value_len = DEFAULT_MAX_VALUE_LEN
        self._cap = int(max_value_len)

    @property
    def max_value_len(self) -> int:
        return self._cap

    def retrieve(self, key: bytes) -> Optional[bytes]:
        buf = self._kvg.alloc_buf(self._cap)
        try:
            rc, true_len = self._kvg.retrieve_gpu(key, buf)
            if rc == 0x87:  # KEY_DOES_NOT_EXIST
                return None
            if rc != 0:
                raise OSError(f"GPU manifest Retrieve failed for "
                              f"{bytes(key).hex()}: rc={rc}")
            return buf.copy_to_host(true_len)
        finally:
            buf.free()

    def exists(self, key: bytes) -> bool:
        raise NotImplementedError("exists() not needed on the GPU loader path")

    def store(self, key: bytes, value: bytes) -> None:
        raise NotImplementedError("store() not available on a read-only loader")

    def delete(self, key: bytes) -> None:
        raise NotImplementedError("delete() not available on a read-only loader")

    def iter_keys(self):
        raise NotImplementedError("iter_keys() not available over NVMe-KV")


def iter_named_tensors_gpu(
    kvg,
    kv: KvClient,
    model_revision: str,
    precision: str,
    *,
    manifest: Optional[WeightManifest] = None,
    keep_buffers: Optional[list] = None,
    verify: bool = False,
) -> Iterator[Tuple[str, "object"]]:
    """GPU-direct variant of :func:`iter_named_tensors`.

    Yields ``(tensor_name, torch.Tensor)`` where each tensor is
    **device-resident** (``.is_cuda``), filled by a host-side NVMe-KV Retrieve
    that DMAs straight into a GPU dma-buf region (no host bounce).

    Two transports are in play:

    * ``kvg`` — a :class:`rados_nkv_weights._kvshim_gpu.KvgHandle` (the live
      GPU-landing transport: Retrieve(key) -> hipMalloc dma-buf region).
    * ``kv`` — a :class:`KvClient` used ONLY to read the Arrow manifest (small,
      host). The manifest fetch is not on the bulk weight datapath.

    The manifest's content-integrity model (whole-Value content-hash) is a HOST
    check over bytes, so for integrity we keep the same guarantee by verifying on
    the GPU is impractical; instead we rely on the manifest's per-Value content
    addressing implicitly — the same Values the CPU path reads. For the common
    *unpacked* tensor (one key, offset 0, whole Value == tensor) the GPU Retrieve
    lands the whole Value and we wrap it directly. For *packed* / multi-slice
    tensors we Retrieve each distinct Value once into its own GPU region and build
    the tensor by concatenating device slices with ``torch.cat`` (still no host
    bounce of the weight bytes).

    ``keep_buffers`` (if given) collects the underlying :class:`GpuBuf` objects
    so the caller can keep the GPU dma-buf regions alive as long as the wrapped
    tensors are in use, then free them. (A directly-wrapped tensor aliases the
    GpuBuf's memory; freeing the buffer invalidates the tensor. The
    concatenated/packed path copies into a fresh device tensor, so those buffers
    can be freed eagerly.)
    """
    import torch  # lazy

    if manifest is None:
        manifest = _load_manifest(kv, model_revision)
    if precision not in manifest.precisions():
        from .loader import PrecisionNotFoundError

        raise PrecisionNotFoundError(
            f"precision {precision!r} not in manifest for {model_revision!r}; "
            f"available: {manifest.precisions()}"
        )

    # Per-process cache of distinct Values already Retrieved into GPU regions, so
    # a packed Value shared by several tensors is fetched once.
    gpu_values: Dict[bytes, "object"] = {}
    if keep_buffers is None:
        keep_buffers = []

    def _retrieve_value_gpu(key: bytes):
        """Retrieve the whole Value for ``key`` into a fresh GPU dma-buf region.

        Returns ``(GpuBuf, true_len)``. Raises on a missing key / error.
        """
        cached = gpu_values.get(key)
        if cached is not None:
            return cached
        # Size the region to the namespace cap (the device truncates to the true
        # value length, reported via cdw0). Fall back to the catalog default cap.
        buf_len = kv.max_value_len
        gbuf = kvg.alloc_buf(buf_len)
        # Register for cleanup IMMEDIATELY — before the Retrieve (or a later
        # tensor) can fail and strand this region. With one SPDK env per process a
        # leaked hipMalloc+dma-buf+DMA-region is unrecoverable. GpuBuf.free() is
        # idempotent, and the caller frees keep_buffers in its finally.
        keep_buffers.append(gbuf)
        rc, true_len = kvg.retrieve_gpu(key, gbuf)
        if rc != 0:
            from .loader import MissingChunkError

            if rc == 0x87:
                raise MissingChunkError(
                    f"value {key.hex()} missing from catalog (GPU Retrieve "
                    f"returned KEY_DOES_NOT_EXIST)"
                )
            raise OSError(f"GPU Retrieve failed for key {key.hex()}: rc={rc}")
        if verify:
            # Opt-in content-integrity check: re-hash the retrieved bytes (D2H)
            # against the content-addressed key, matching the host path's
            # guarantee. Defeats the no-copy benefit, so it is off by default.
            from .keys import chunk_key
            from .loader import IntegrityError

            host = gbuf.copy_to_host(true_len)
            if chunk_key(host) != key:
                raise IntegrityError(
                    f"GPU-path integrity check failed for value {key.hex()}: "
                    f"retrieved bytes hash to {chunk_key(host).hex()}"
                )
        gpu_values[key] = (gbuf, true_len)
        return gbuf, true_len

    for name in manifest.tensors(precision=precision):
        dtype_str, shape, _sizes = manifest.tensor_meta(name, precision)
        dt = _torch_dtype(torch, dtype_str)
        slices = manifest.chunk_slices(name, precision)
        total = sum(size for _k, _o, size in slices)

        if len(slices) == 1 and slices[0][1] == 0:
            # Unpacked fast path: one key, offset 0. If the slice is the WHOLE
            # Value, wrap the GPU region directly (zero device copies). If it is a
            # prefix (size < true_len), the wrap still views the correct leading
            # bytes (the Retrieve landed the whole Value contiguously).
            key, _off, size = slices[0]
            gbuf, true_len = _retrieve_value_gpu(key)
            if size > true_len:
                raise ValueError(
                    f"slice size {size} for tensor {name!r} exceeds retrieved "
                    f"value length {true_len} for key {key.hex()}"
                )
            # Symmetric with the packed path's numel check: the slice byte count
            # must equal the tensor's expected bytes, so _wrap's floor division
            # (count = nbytes // itemsize) can't silently mis-size.
            itemsize = torch.empty(0, dtype=dt).element_size()
            nelem = 1
            for d in shape:
                nelem *= d
            if size != nelem * itemsize:
                raise ValueError(
                    f"slice size {size} for tensor {name!r} != expected "
                    f"{nelem * itemsize} bytes (shape {tuple(shape)} "
                    f"dtype {dtype_str})"
                )
            tensor = _wrap_gpu_buf_as_tensor(torch, gbuf.dptr, size, dt, shape)
            yield name, tensor
            continue

        # Packed / multi-slice: build device byte views per slice and concat into
        # a fresh contiguous device tensor (so the shared Value buffers can be
        # freed independently of the resulting tensor).
        parts = []
        for key, offset, size in slices:
            gbuf, true_len = _retrieve_value_gpu(key)
            end = offset + size
            if offset < 0 or end > true_len:
                raise ValueError(
                    f"slice [{offset}:{end}] for tensor {name!r} out of range "
                    f"for value {key.hex()} of length {true_len}"
                )
            whole_u8 = _wrap_gpu_buf_as_tensor(
                torch, gbuf.dptr, true_len, torch.uint8, (true_len,)
            )
            parts.append(whole_u8[offset:end])
        flat_u8 = torch.cat(parts) if len(parts) > 1 else parts[0].clone()
        if int(flat_u8.numel()) != total:
            raise ValueError(
                f"reassembled tensor {name!r} is {int(flat_u8.numel())} bytes, "
                f"manifest expected {total}"
            )
        flat = flat_u8.view(dt)
        tensor = flat.reshape(tuple(shape)) if shape else flat.reshape(())
        yield name, tensor

    # Every GPU region is registered into keep_buffers at creation
    # (_retrieve_value_gpu), so there is nothing to sweep here: the caller frees
    # all of keep_buffers after model.load_weights has consumed the iterator,
    # including any region allocated for a tensor that errored mid-stream.


# ---------------------------------------------------------------------------
# ModelConfig -> (Model revision, Precision variant)
# ---------------------------------------------------------------------------


def _revision_from_model_config(model_config) -> str:
    """Derive the catalog Model revision identity from a vLLM ``ModelConfig``.

    vLLM's ``ModelConfig`` carries ``model`` (the model id/path passed to
    ``--model``) and an optional ``revision``. We join them into the catalog's
    revision identity ``"<model>@<revision>"`` (and canonicalize via the keys
    module so the publisher and loader resolve the same Manifest key). When no
    explicit revision is set, the bare model id is used.

    An explicit override wins: a ``rados_nkv_revision`` set in
    ``model_config.model_loader_extra_config`` is used verbatim. That dict is the
    documented channel for ``--model-loader-extra-config`` JSON, so an operator
    can point the plugin at any catalog identity / precision regardless of how
    ``--model`` is spelled.
    """
    extra = getattr(model_config, "model_loader_extra_config", None) or {}
    override = extra.get("rados_nkv_revision")
    if override:
        return canonical_revision(str(override))

    model = getattr(model_config, "model", None)
    if not model:
        raise ValueError(
            "ModelConfig has no 'model' to derive a catalog revision from; "
            "set model_loader_extra_config={'rados_nkv_revision': ...}"
        )
    revision = getattr(model_config, "revision", None)
    ident = f"{model}@{revision}" if revision else str(model)
    return canonical_revision(ident)


def _precision_from_model_config(model_config) -> str:
    """Derive the Precision variant to load from a vLLM ``ModelConfig``.

    Resolution order:

    1. An explicit ``rados_nkv_precision`` in ``model_loader_extra_config``.
    2. The model's quantization method (``model_config.quantization``) if set —
       the quant name is the Precision variant key (e.g. ``"fp8"``, ``"awq"``).
    3. The model dtype (``model_config.dtype``) mapped to a short name
       (``torch.float16`` -> ``"fp16"``, ``torch.bfloat16`` -> ``"bf16"``, …).

    The chosen string must match a Precision variant column the publisher wrote;
    a mismatch surfaces as
    :class:`~rados_nkv_weights.loader.PrecisionNotFoundError` at load time, which
    lists the manifest's available precisions.
    """
    extra = getattr(model_config, "model_loader_extra_config", None) or {}
    override = extra.get("rados_nkv_precision")
    if override:
        return str(override)

    quant = getattr(model_config, "quantization", None)
    if quant:
        return str(quant)

    dtype = getattr(model_config, "dtype", None)
    if dtype is not None:
        # dtype may be a torch.dtype (repr "torch.float16") or already a string.
        name = str(dtype)
        if name.startswith("torch."):
            name = name[len("torch."):]
        short = {
            "float16": "fp16",
            "bfloat16": "bf16",
            "float32": "fp32",
            "float8_e4m3fn": "fp8",
            "float8_e5m2": "fp8",
        }
        return short.get(name, name)

    raise ValueError(
        "could not derive a Precision variant from ModelConfig (no "
        "rados_nkv_precision, quantization, or dtype); set "
        "model_loader_extra_config={'rados_nkv_precision': ...}"
    )


# ---------------------------------------------------------------------------
# Read-only KvClient acquisition (overridable for tests / alternate transports)
# ---------------------------------------------------------------------------


def _open_loader_kv(model_config) -> Tuple[KvClient, "callable"]:
    """Open the **read-only** NVMe-KV client for the loader, plus a closer.

    Reads connection params from ``model_loader_extra_config``:

    * ``rados_nkv_vfu_addr`` — the VFIOUSER controller socket directory of a
      running ``nvmf_tgt`` (as accepted by ``NvmeKvClient.open_loader``).
    * ``rados_nkv_nsid`` — KV namespace id to bind (default 0 = first KV ns).

    Returns ``(client, close)``. When no ``rados_nkv_vfu_addr`` is given this
    raises — there is no implicit fallback in the vLLM path, because silently
    loading from an empty in-memory catalog would hand vLLM a model with no
    weights. (The CLI demo in :mod:`loader` keeps the in-memory fallback; the
    real plugin does not.)
    """
    extra = getattr(model_config, "model_loader_extra_config", None) or {}
    vfu_addr = extra.get("rados_nkv_vfu_addr")
    if not vfu_addr:
        raise ValueError(
            "rados-nkv load-format requires model_loader_extra_config["
            "'rados_nkv_vfu_addr'] (the nvmf_tgt VFIOUSER socket dir); none set"
        )
    nsid = int(extra.get("rados_nkv_nsid", 0))
    from .nvmekv_client import NvmeKvClient

    client = NvmeKvClient.open_loader(vfu_addr, nsid=nsid)
    return client, client.close


# ---------------------------------------------------------------------------
# The vLLM BaseModelLoader subclass
# ---------------------------------------------------------------------------


def _base_model_loader_cls():
    """Import and return vLLM's ``BaseModelLoader`` (lazy, vllm required)."""
    from vllm.model_executor.model_loader.base_loader import BaseModelLoader

    return BaseModelLoader


def make_loader_cls():
    """Build the ``RadosNkvModelLoader`` class, subclassing vLLM's
    ``BaseModelLoader``.

    Defined as a factory so that the class body — which must subclass a vllm
    symbol — is only constructed when vllm is importable. :func:`register` calls
    this; nothing at import time of this module does.
    """
    BaseModelLoader = _base_model_loader_cls()

    class RadosNkvModelLoader(BaseModelLoader):
        """vLLM ``--load-format=rados-nkv`` loader over the Weights catalog.

        Streams tensors out of a read-only NVMe-KV namespace into the model's
        parameters. Connection + catalog-coordinate parameters come from
        ``--model-loader-extra-config`` (see :func:`_open_loader_kv`,
        :func:`_revision_from_model_config`, :func:`_precision_from_model_config`).

        The optional ``kv`` constructor argument injects a :class:`KvClient`
        (used by tests with :class:`InMemoryKvClient`); when ``None`` the loader
        opens the real :class:`NvmeKvClient` from the ModelConfig at load time.
        """

        def __init__(self, load_config, kv: Optional[KvClient] = None):
            super().__init__(load_config)
            self._injected_kv = kv

        def download_model(self, model_config) -> None:
            """No-op: the catalog is fetched on demand by content-addressed key.

            There is nothing to pre-download — Weight chunks are Retrieved
            lazily as the weight iterator is consumed. (A future optimization
            could pre-warm by issuing the manifest Retrieve here.)
            """
            return None

        def load_weights(self, model, model_config) -> None:
            """Stream catalog tensors into ``model`` via its ``load_weights``.

            Derives revision + precision from ``model_config``, opens the
            read-only client (or uses the injected one), builds the
            ``(name, torch.Tensor)`` weight iterator with
            :func:`iter_named_tensors`, and hands it to ``model.load_weights``
            so the model applies TP sharding and copies into device memory.

            **GPU-direct path (bead spdk-p9k.1.1).** When
            ``model_loader_extra_config['rados_nkv_gpu_direct']`` is set, the
            weight bytes are Retrieved DIRECTLY into hipMalloc GPU dma-buf
            regions and yielded as device-resident tensors
            (:func:`iter_named_tensors_gpu`), so ``model.load_weights`` does no
            implicit H2D copy. This leg uses the GPU shim
            (:mod:`rados_nkv_weights._kvshim_gpu`) for the device-landing
            Retrieve and a small host KvClient only for the Arrow manifest.
            """
            revision = _revision_from_model_config(model_config)
            precision = _precision_from_model_config(model_config)
            extra = getattr(model_config, "model_loader_extra_config", None) or {}

            if extra.get("rados_nkv_gpu_direct"):
                self._load_weights_gpu_direct(model, model_config, revision, precision)
                return

            if self._injected_kv is not None:
                kv, close = self._injected_kv, (lambda: None)
            else:
                kv, close = _open_loader_kv(model_config)
            try:
                weights = iter_named_tensors(kv, revision, precision)
                model.load_weights(weights)
            finally:
                close()

        def _load_weights_gpu_direct(self, model, model_config, revision, precision):
            """GPU-direct emit: device-resident tensors via the dma-buf Retrieve.

            Opens a :class:`~rados_nkv_weights._kvshim_gpu.KvgHandle` at
            ``rados_nkv_vfu_addr`` (the device-landing transport) plus a host
            KvClient for the manifest, builds the device-tensor iterator, hands it
            to ``model.load_weights``, then frees the GPU dma-buf regions once the
            model has copied the weights into its parameters.
            """
            from ._kvshim_gpu import KvgHandle

            extra = getattr(model_config, "model_loader_extra_config", None) or {}
            vfu_addr = extra.get("rados_nkv_vfu_addr")
            if not vfu_addr:
                raise ValueError(
                    "rados_nkv_gpu_direct requires model_loader_extra_config["
                    "'rados_nkv_vfu_addr'] (the nvmf_tgt VFIOUSER socket dir)"
                )

            kvg = KvgHandle.open(vfu_addr)
            keep = []

            # Manifest (small host read). The GPU shim and the SPDK-NVMe host
            # shim each init the SPDK env, which DPDK permits only ONCE per
            # process, so we MUST NOT also open an NvmeKvClient here. Use the
            # injected KvClient if present (tests), else serve the manifest
            # Retrieve through the SAME KvgHandle (GPU buffer -> small host copy)
            # via the adapter below — one env, both reads.
            if self._injected_kv is not None:
                kv, close_kv = self._injected_kv, (lambda: None)
            else:
                kv, close_kv = _KvgManifestClient(kvg), (lambda: None)

            try:
                weights = iter_named_tensors_gpu(
                    kvg, kv, revision, precision, keep_buffers=keep,
                    verify=bool(extra.get("rados_nkv_gpu_verify")),
                )
                model.load_weights(weights)
            finally:
                for gbuf in keep:
                    gbuf.free()
                kvg.close()
                close_kv()

    return RadosNkvModelLoader


def register() -> None:
    """Register the ``rados-nkv`` ``--load-format`` with the running vLLM.

    Wired as a ``vllm.general_plugins`` entry point (see ``pyproject.toml``), so
    vLLM calls it automatically during plugin discovery; it may also be called
    directly before constructing the engine. Imports vllm; do **not** call this
    from the core package import path.

    **Re-entrant:** vLLM may invoke a general plugin more than once per process.
    Upstream ``register_model_loader`` would just warn and overwrite on a
    duplicate ``load_format``; our explicit guard below makes the second call a
    clean no-op instead. After this, ``--load-format=rados-nkv`` (and
    ``LoadConfig(load_format="rados-nkv")``) selects this loader.
    """
    from vllm.model_executor.model_loader import (
        _LOAD_FORMAT_TO_MODEL_LOADER,
        register_model_loader,
    )

    if LOAD_FORMAT in _LOAD_FORMAT_TO_MODEL_LOADER:
        return  # already registered this process — re-entrant no-op
    register_model_loader(LOAD_FORMAT)(make_loader_cls())
