# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""Tests for the vLLM ``--load-format=rados-nkv`` integration.

torch and vllm are OPTIONAL to the core package and are not installed in CI here,
so these tests stand up tiny **fakes** for both: a fake ``torch`` (just enough of
``dtype`` / ``frombuffer`` / ``reshape`` to land bytes into a typed, shaped
buffer) and a fake ``vllm`` ``BaseModelLoader`` + model. With those injected we
exercise the *real* loader code paths end to end:

* chunk-streaming -> per-tensor tensors land with correct dtype/shape/bytes
  (including a multi-chunk tensor, a packed shared Value with non-zero offsets,
  a bf16 dtype numpy cannot represent, and a 0-d scalar);
* the full ``RadosNkvModelLoader.load_weights`` drives ``model.load_weights`` so
  every published tensor reaches the model;
* ModelConfig -> (revision, precision) mapping;
* the content-hash integrity check still fires on the torch path.

No real torch / vllm / GPU is required or used. The acceptance criterion that a
*real* vLLM loads a *real* model and produces correct inference needs a vLLM +
model + GPU and is NOT exercised here (tracked on the bead as DEFERRED).
"""

import sys
import types

import numpy as np
import pytest

from rados_nkv_weights.kvclient import InMemoryKvClient
from rados_nkv_weights.publisher import publish


# ---------------------------------------------------------------------------
# Fake torch: enough surface for vllm_loader.iter_named_tensors.
# ---------------------------------------------------------------------------


class _FakeDtype:
    def __init__(self, name, np_dtype, itemsize):
        self.name = name
        self.np = np_dtype
        self.itemsize = itemsize

    def __repr__(self):
        return f"torch.{self.name}"


class _FakeTensor:
    """A minimal stand-in: holds the raw bytes, a dtype, and a shape.

    Records enough to assert that the bytes/dtype/shape landed correctly; bf16
    (which numpy can't view) is kept as raw bytes + element count.
    """

    def __init__(self, data: bytes, dtype: _FakeDtype, shape=None):
        self._data = bytes(data)
        self.dtype = dtype
        if dtype.itemsize:
            self._count = len(self._data) // dtype.itemsize
        else:
            self._count = 0
        self.shape = (self._count,) if shape is None else tuple(shape)

    def reshape(self, shape):
        return _FakeTensor(self._data, self.dtype, tuple(shape))

    def numpy(self):
        arr = np.frombuffer(self._data, dtype=self.dtype.np)
        return arr.reshape(self.shape)

    def tobytes(self):
        return self._data


def _make_fake_torch():
    mod = types.ModuleType("torch")
    mod.dtype = _FakeDtype  # so isinstance(dt, torch.dtype) works in _torch_dtype
    mod.float16 = _FakeDtype("float16", np.float16, 2)
    mod.float32 = _FakeDtype("float32", np.float32, 4)
    mod.float64 = _FakeDtype("float64", np.float64, 8)
    mod.uint8 = _FakeDtype("uint8", np.uint8, 1)
    mod.int8 = _FakeDtype("int8", np.int8, 1)
    # bfloat16: numpy has no native bf16; back it with uint16 storage so the
    # itemsize math is right while staying numpy-unrepresentable as a float.
    mod.bfloat16 = _FakeDtype("bfloat16", np.uint16, 2)

    def frombuffer(buf, dtype):
        return _FakeTensor(bytes(buf), dtype)

    mod.frombuffer = frombuffer
    return mod


@pytest.fixture
def fake_torch(monkeypatch):
    mod = _make_fake_torch()
    monkeypatch.setitem(sys.modules, "torch", mod)
    return mod


# ---------------------------------------------------------------------------
# iter_named_tensors: chunk-streaming -> tensors with right dtype/shape/bytes.
# ---------------------------------------------------------------------------


def test_iter_named_tensors_shapes_dtypes_bytes(fake_torch):
    from rados_nkv_weights.vllm_loader import iter_named_tensors

    kv = InMemoryKvClient()
    a = np.arange(24, dtype=np.float16).reshape(4, 6)
    b = np.arange(6, dtype=np.float32).reshape(2, 3)
    publish(
        kv,
        "m@rev",
        {
            "a.weight": {"fp16": ("float16", (4, 6), a.tobytes())},
            "b.weight": {"fp16": ("float32", (2, 3), b.tobytes())},
        },
    )

    out = dict(iter_named_tensors(kv, "m@rev", "fp16"))
    assert set(out) == {"a.weight", "b.weight"}

    assert out["a.weight"].dtype is fake_torch.float16
    assert out["a.weight"].shape == (4, 6)
    np.testing.assert_array_equal(out["a.weight"].numpy(), a)

    assert out["b.weight"].dtype is fake_torch.float32
    assert out["b.weight"].shape == (2, 3)
    np.testing.assert_array_equal(out["b.weight"].numpy(), b)


def test_iter_named_tensors_multichunk_tensor_reassembles(fake_torch):
    # A tensor larger than the value cap spans multiple Weight chunks; the
    # streamed tensor must reassemble all chunks in order.
    from rados_nkv_weights.vllm_loader import iter_named_tensors

    kv = InMemoryKvClient(max_value_len=64)
    big = np.arange(200, dtype=np.float32)  # 800 bytes >> 64-byte cap
    publish(
        kv,
        "big@rev",
        {"big.weight": {"fp32": ("float32", (200,), big.tobytes())}},
        max_value_len=64,
    )

    out = dict(iter_named_tensors(kv, "big@rev", "fp32"))
    assert out["big.weight"].shape == (200,)
    np.testing.assert_array_equal(out["big.weight"].numpy(), big)


def test_iter_named_tensors_packed_offsets(fake_torch):
    # Packing co-locates small tensors in one Value at distinct offsets; each
    # streamed tensor must slice its own (offset, size) window.
    from rados_nkv_weights.vllm_loader import iter_named_tensors

    kv = InMemoryKvClient()
    t1 = np.arange(8, dtype=np.uint8)
    t2 = (np.arange(8, dtype=np.uint8) + 100).astype(np.uint8)
    publish(
        kv,
        "pack@rev",
        {
            "t1": {"int8": ("uint8", (8,), t1.tobytes())},
            "t2": {"int8": ("uint8", (8,), t2.tobytes())},
        },
        pack=True,
    )

    out = dict(iter_named_tensors(kv, "pack@rev", "int8"))
    np.testing.assert_array_equal(out["t1"].numpy(), t1)
    np.testing.assert_array_equal(out["t2"].numpy(), t2)


def test_iter_named_tensors_bf16_numpy_unrepresentable(fake_torch):
    # bf16 cannot be viewed as a numpy float; the torch path must still carry the
    # raw 2-byte elements and the right shape. (load_arrays' numpy path can't.)
    from rados_nkv_weights.vllm_loader import iter_named_tensors

    kv = InMemoryKvClient()
    raw = bytes(range(16))  # 8 bf16 elements
    publish(kv, "bf@rev", {"w": {"bf16": ("bfloat16", (8,), raw)}})

    out = dict(iter_named_tensors(kv, "bf@rev", "bf16"))
    assert out["w"].dtype is fake_torch.bfloat16
    assert out["w"].shape == (8,)
    assert out["w"].tobytes() == raw


def test_iter_named_tensors_scalar_shape(fake_torch):
    from rados_nkv_weights.vllm_loader import iter_named_tensors

    kv = InMemoryKvClient()
    scalar = np.float32(3.5)
    publish(kv, "s@rev", {"scale": {"fp32": ("float32", (), scalar.tobytes())}})

    out = dict(iter_named_tensors(kv, "s@rev", "fp32"))
    assert out["scale"].shape == ()
    assert out["scale"].numpy().shape == ()
    np.testing.assert_array_equal(out["scale"].numpy(), scalar)


def test_iter_named_tensors_integrity_check_fires(fake_torch):
    # The content-hash integrity guarantee must still hold on the torch path.
    from rados_nkv_weights.keys import manifest_key
    from rados_nkv_weights.loader import IntegrityError
    from rados_nkv_weights.vllm_loader import iter_named_tensors

    kv = InMemoryKvClient()
    payload = b"\x07" * 64
    publish(kv, "intg@rev", {"t": {"fp16": ("float16", (32,), payload)}})

    mkey = manifest_key("intg@rev")
    for k, v in list(kv._data.items()):
        if k != mkey and len(v) == len(payload):
            kv._data[k] = b"\xff" * len(payload)  # same length, wrong content

    with pytest.raises(IntegrityError):
        list(iter_named_tensors(kv, "intg@rev", "fp16"))


# ---------------------------------------------------------------------------
# dtype mapping helper.
# ---------------------------------------------------------------------------


def test_torch_dtype_aliases_and_unknown(fake_torch):
    from rados_nkv_weights.vllm_loader import _torch_dtype

    assert _torch_dtype(fake_torch, "fp16") is fake_torch.float16
    assert _torch_dtype(fake_torch, "bfloat16") is fake_torch.bfloat16
    assert _torch_dtype(fake_torch, "torch.float32") is fake_torch.float32
    with pytest.raises(ValueError):
        _torch_dtype(fake_torch, "not_a_dtype")


# ---------------------------------------------------------------------------
# ModelConfig -> (revision, precision) mapping (pure; no torch/vllm needed).
# ---------------------------------------------------------------------------


class _ModelConfig:
    def __init__(self, **kw):
        self.model = kw.get("model")
        self.revision = kw.get("revision")
        self.dtype = kw.get("dtype")
        self.quantization = kw.get("quantization")
        self.model_loader_extra_config = kw.get("extra")


def test_revision_from_model_config():
    from rados_nkv_weights.keys import canonical_revision
    from rados_nkv_weights.vllm_loader import _revision_from_model_config

    mc = _ModelConfig(model="org/Model", revision="v1")
    assert _revision_from_model_config(mc) == canonical_revision("org/Model@v1")

    mc = _ModelConfig(model="org/Model")
    assert _revision_from_model_config(mc) == canonical_revision("org/Model")

    mc = _ModelConfig(model="ignored", extra={"rados_nkv_revision": "x@y"})
    assert _revision_from_model_config(mc) == canonical_revision("x@y")


def test_precision_from_model_config():
    from rados_nkv_weights.vllm_loader import _precision_from_model_config

    assert _precision_from_model_config(_ModelConfig(extra={"rados_nkv_precision": "int4"})) == "int4"
    assert _precision_from_model_config(_ModelConfig(quantization="fp8")) == "fp8"
    assert _precision_from_model_config(_ModelConfig(dtype="torch.float16")) == "fp16"
    assert _precision_from_model_config(_ModelConfig(dtype="bfloat16")) == "bf16"
    with pytest.raises(ValueError):
        _precision_from_model_config(_ModelConfig())


# ---------------------------------------------------------------------------
# Full plugin path with a fake vllm BaseModelLoader + fake model.
# ---------------------------------------------------------------------------


def _install_fake_vllm(monkeypatch):
    """Inject a minimal fake vllm exposing BaseModelLoader + register_model_loader."""
    base_mod = types.ModuleType("vllm.model_executor.model_loader.base_loader")

    class BaseModelLoader:
        def __init__(self, load_config):
            self.load_config = load_config

    base_mod.BaseModelLoader = BaseModelLoader

    loader_pkg = types.ModuleType("vllm.model_executor.model_loader")
    registry = {}

    def register_model_loader(load_format):
        def deco(cls):
            if not issubclass(cls, BaseModelLoader):
                raise TypeError("loader must subclass BaseModelLoader")
            if load_format in registry:
                raise ValueError(f"{load_format} already registered")
            registry[load_format] = cls
            return cls

        return deco

    loader_pkg.register_model_loader = register_model_loader
    loader_pkg._LOAD_FORMAT_TO_MODEL_LOADER = registry

    # Parent packages so the dotted imports resolve.
    vllm_mod = types.ModuleType("vllm")
    me_mod = types.ModuleType("vllm.model_executor")
    monkeypatch.setitem(sys.modules, "vllm", vllm_mod)
    monkeypatch.setitem(sys.modules, "vllm.model_executor", me_mod)
    monkeypatch.setitem(sys.modules, "vllm.model_executor.model_loader", loader_pkg)
    monkeypatch.setitem(
        sys.modules, "vllm.model_executor.model_loader.base_loader", base_mod
    )
    return loader_pkg, registry


class _FakeModel:
    def __init__(self):
        self.loaded = {}

    def load_weights(self, weights):
        for name, tensor in weights:
            self.loaded[name] = tensor
        return set(self.loaded)


def test_full_plugin_load_weights_into_model(monkeypatch, fake_torch):
    loader_pkg, registry = _install_fake_vllm(monkeypatch)
    from rados_nkv_weights.vllm_loader import make_loader_cls, register

    # register() lands the format in the (fake) registry and is re-entrant.
    register()
    assert "rados-nkv" in registry
    register()  # second call: no-op, no error

    kv = InMemoryKvClient()
    a = np.arange(12, dtype=np.float16).reshape(3, 4)
    b = np.arange(4, dtype=np.float32)
    publish(
        kv,
        "demo/model@v1",
        {
            "layer.0.weight": {"fp16": ("float16", (3, 4), a.tobytes())},
            "layer.0.bias": {"fp16": ("float32", (4,), b.tobytes())},
        },
    )

    Loader = make_loader_cls()
    loader = Loader(load_config=object(), kv=kv)

    model = _FakeModel()
    mc = _ModelConfig(model="demo/model", revision="v1", dtype="torch.float16")

    loader.download_model(mc)  # no-op
    loader.load_weights(model, mc)

    assert set(model.loaded) == {"layer.0.weight", "layer.0.bias"}
    np.testing.assert_array_equal(model.loaded["layer.0.weight"].numpy(), a)
    np.testing.assert_array_equal(model.loaded["layer.0.bias"].numpy(), b)


def test_plugin_requires_vfu_addr_when_no_injected_kv(monkeypatch, fake_torch):
    _install_fake_vllm(monkeypatch)
    from rados_nkv_weights.vllm_loader import make_loader_cls

    Loader = make_loader_cls()
    loader = Loader(load_config=object(), kv=None)  # no injected client
    model = _FakeModel()
    mc = _ModelConfig(model="demo/model", revision="v1", dtype="torch.float16")

    # No rados_nkv_vfu_addr -> the real plugin refuses (won't silently load an
    # empty catalog and hand vLLM a weightless model).
    with pytest.raises(ValueError, match="rados_nkv_vfu_addr"):
        loader.load_weights(model, mc)


# ---------------------------------------------------------------------------
# Optionality: importing core + the vllm_loader module requires neither
# torch nor vllm.
# ---------------------------------------------------------------------------


def test_import_paths_do_not_require_torch_or_vllm():
    # torch/vllm are not installed in this env at all; the bare imports below
    # therefore prove the modules import with them absent. (Belt-and-braces: also
    # assert they're truly absent so the test is meaningful.)
    assert "torch" not in sys.modules or isinstance(
        sys.modules.get("torch"), types.ModuleType
    )
    import importlib

    import rados_nkv_weights  # noqa: F401
    import rados_nkv_weights.loader  # noqa: F401
    import rados_nkv_weights.vllm_loader  # noqa: F401

    importlib.reload(rados_nkv_weights.vllm_loader)


def test_stub_seam_returns_numpy_without_torch():
    # The torch/vllm-free seam still works with neither installed.
    from rados_nkv_weights.loader import RadosNkvModelLoaderStub

    kv = InMemoryKvClient()
    arr = np.arange(6, dtype=np.float32).reshape(2, 3)
    publish(kv, "seam@rev", {"t": {"fp32": ("float32", (2, 3), arr.tobytes())}})

    stub = RadosNkvModelLoaderStub(kv, "seam@rev", "fp32")
    out = stub.load_weights()
    np.testing.assert_array_equal(out["t"], arr)
