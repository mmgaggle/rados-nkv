# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""vLLM ``--load-format`` integration for the Weights catalog (ADR-0008).

This is the real, key-native vLLM model loader: a standalone
``--load-format=rados-nkv`` plugin (ADR-0008: "a standalone key-native vLLM
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
            """
            revision = _revision_from_model_config(model_config)
            precision = _precision_from_model_config(model_config)

            if self._injected_kv is not None:
                kv, close = self._injected_kv, (lambda: None)
            else:
                kv, close = _open_loader_kv(model_config)
            try:
                weights = iter_named_tensors(kv, revision, precision)
                model.load_weights(weights)
            finally:
                close()

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
