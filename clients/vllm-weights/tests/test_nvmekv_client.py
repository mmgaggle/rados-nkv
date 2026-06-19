# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""Live NVMe-KV transport test for :class:`NvmeKvClient`.

Brings up a real SPDK ``nvmf_tgt`` with an in-memory kvdev bound to a KV
namespace over the VFIOUSER transport (no Ceph, no GPU), then exercises the
genuine wire path through the SPDK host shim:

1. direct store/retrieve/exist round-trip (incl. a binary value with NULs and a
   missing-key -> None/False);
2. the END-TO-END publisher.publish(...) -> loader.load(...) over
   :class:`NvmeKvClient` (instead of InMemoryKvClient), asserting tensors
   byte-match — proving the real transport carries the catalog.

Skipped automatically when the native shim ``.so`` is not built or ``nvmf_tgt``
is absent.

NOTE (single-lifetime): an ``init_env=True`` shim initializes the process's SPDK
env exactly once per process and cannot be reopened after close (DPDK can't
re-init the env). So this module opens at most ONE shim handle for its whole
run: a single publisher client is shared across the tests (a loader client would
need a second env-init in the same process). The client-side read-only guard is
tested without opening a second live handle.
"""

import os
import shutil
import socket
import subprocess
import tempfile
import time

import numpy as np
import pytest

SPDK_ROOT = os.environ.get("SPDK_ROOT", "/mnt/spdk")
NVMF_TGT = os.path.join(SPDK_ROOT, "build", "bin", "nvmf_tgt")
RPC_PY = os.path.join(SPDK_ROOT, "scripts", "rpc.py")

# Skip the whole module unless the native lib loads AND the target/rpc exist.
try:
    from rados_nkv_weights import _kvshim
    _SHIM_OK = _kvshim.available()
    _SHIM_WHY = "" if _SHIM_OK else "libradosnkv_kvshim.so not loadable"
except Exception as exc:  # pragma: no cover
    _SHIM_OK = False
    _SHIM_WHY = f"_kvshim import failed: {exc}"

_TGT_OK = os.path.exists(NVMF_TGT) and os.path.exists(RPC_PY)

pytestmark = pytest.mark.skipif(
    not (_SHIM_OK and _TGT_OK),
    reason=(
        f"native shim unavailable ({_SHIM_WHY or 'ok'}) or "
        f"nvmf_tgt/rpc.py missing (nvmf_tgt={NVMF_TGT} exists={os.path.exists(NVMF_TGT)})"
    ),
)

NQN = "nqn.2026-06.io.spdk:rados-nkv-test-cnode0"
KVDEV = "RadosNkvTestMem0"


def _wait_for_sock(path, timeout=30.0):
    """Wait until the RPC unix socket at ``path`` accepts a connection."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(path)
                s.close()
                return True
            except OSError:
                pass
            finally:
                try:
                    s.close()
                except OSError:
                    pass
        time.sleep(0.1)
    return False


class _Target:
    """A live nvmf_tgt + VFIOUSER + KV namespace, torn down on close()."""

    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="rados_nkv_test.")
        self.rpc_sock = os.path.join(self.tmp, "rpc.sock")
        self.muser_dir = os.path.join(self.tmp, "domain", "muser0", "0")
        os.makedirs(self.muser_dir, exist_ok=True)
        self.proc = None

    def _rpc(self, *args):
        cmd = ["python3", RPC_PY, "-s", self.rpc_sock, *args]
        subprocess.run(cmd, check=True, capture_output=True, text=True)

    def start(self):
        self.proc = subprocess.Popen(
            [NVMF_TGT, "-r", self.rpc_sock, "-m", "0x1", "--no-huge", "-s", "1024"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        if not _wait_for_sock(self.rpc_sock):
            self.close()
            raise RuntimeError("nvmf_tgt RPC socket did not come up")
        self._rpc("nvmf_create_transport", "-t", "VFIOUSER")
        self._rpc("kvdev_mem_create", KVDEV)
        self._rpc("nvmf_create_subsystem", NQN, "-s", "SPDKKV001", "-a")
        self._rpc("nvmf_subsystem_add_kv_ns", NQN, KVDEV)
        self._rpc(
            "nvmf_subsystem_add_listener", NQN,
            "-t", "VFIOUSER", "-a", self.muser_dir, "-s", "0",
        )
        return self

    def close(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=10)
            self.proc = None
        shutil.rmtree(self.tmp, ignore_errors=True)


@pytest.fixture(scope="module")
def target():
    tgt = _Target().start()
    try:
        yield tgt
    finally:
        tgt.close()


@pytest.fixture(scope="module")
def publisher_client(target):
    """A single live publisher NvmeKvClient (shared: one SPDK env per process)."""
    from rados_nkv_weights.nvmekv_client import NvmeKvClient

    client = NvmeKvClient.open_publisher(target.muser_dir, nsid=0)
    try:
        yield client
    finally:
        client.close()


# Keys must be raw 16-byte bytes (rados_nkv_weights.config.KEY_LEN).
def _k(tag, n):
    return bytes([tag]) + bytes([n]) * 15


def test_direct_store_retrieve_exist_roundtrip(publisher_client):
    kv = publisher_client

    # max value/key length advertised by the bound KV namespace.
    assert kv.max_value_len > 0
    assert kv.max_key_len >= 16

    # A binary value containing NUL bytes (binary-safety).
    key = _k(0x01, 0xAA)
    value = b"\x00\x01\x00\xfe\x00\xff" + bytes(range(256)) + b"\x00" * 32

    assert kv.exists(key) is False  # absent before store
    assert kv.retrieve(key) is None

    kv.store(key, value)

    assert kv.exists(key) is True
    got = kv.retrieve(key)
    assert got == value  # byte-exact, true length

    # A second, larger binary value under a different key.
    key2 = _k(0x01, 0xBB)
    value2 = bytes((i * 31 + 7) & 0xFF for i in range(8192))
    kv.store(key2, value2)
    assert kv.retrieve(key2) == value2

    # Missing key -> None / False.
    missing = _k(0x01, 0xCD)
    assert kv.retrieve(missing) is None
    assert kv.exists(missing) is False


def test_read_only_guard_rejects_store():
    """The client-side read_only guard rejects store() without needing a second
    live SPDK env (single-lifetime constraint)."""
    from rados_nkv_weights.nvmekv_client import NvmeKvClient, ReadOnlyError

    # Construct a loader-flavoured client around a dummy handle: store() must
    # short-circuit on the read_only flag BEFORE touching the handle.
    loader = NvmeKvClient(handle=None, read_only=True)
    with pytest.raises(ReadOnlyError):
        loader.store(_k(0x01, 0x01), b"nope")


def test_end_to_end_publish_then_load_over_real_transport(publisher_client):
    """publish(...) then load(...) through NvmeKvClient: a multi-tensor,
    multi-precision model must round-trip byte-for-byte over the real wire."""
    from rados_nkv_weights.publisher import publish
    from rados_nkv_weights.loader import load, load_arrays

    kv = publisher_client

    rev = "Org/RealTransport-Model@v1"

    # Multi-tensor, multi-precision model with distinct bytes per (tensor, prec).
    a_fp16 = np.arange(64, dtype=np.float16)
    a_fp32 = (np.arange(64, dtype=np.float32) * 1.5).astype(np.float32)
    b_int8 = np.arange(128, dtype=np.uint8)
    c_fp16 = (np.linspace(-1, 1, 100).astype(np.float16))

    tensors = {
        "layer0.weight": {
            "fp16": ("float16", (64,), a_fp16.tobytes()),
            "fp32": ("float32", (64,), a_fp32.tobytes()),
        },
        "layer0.bias": {
            "int8": ("uint8", (128,), b_int8.tobytes()),
        },
        "layer1.weight": {
            "fp16": ("float16", (100,), c_fp16.tobytes()),
        },
    }

    # Use a small max_value_len to force multi-chunk tensors over the wire.
    stats = publish(kv, rev, tensors, max_value_len=37)
    assert stats.chunks_total >= 4
    assert stats.bytes_total == sum(
        len(d) for byprec in tensors.values() for (_, _, d) in byprec.values()
    )

    # Load fp16: exactly the tensors published in fp16.
    loaded_fp16 = load(kv, rev, "fp16")
    assert set(loaded_fp16) == {"layer0.weight", "layer1.weight"}
    assert loaded_fp16["layer0.weight"] == a_fp16.tobytes()
    assert loaded_fp16["layer1.weight"] == c_fp16.tobytes()

    # Load fp32 and int8.
    loaded_fp32 = load(kv, rev, "fp32")
    assert set(loaded_fp32) == {"layer0.weight"}
    assert loaded_fp32["layer0.weight"] == a_fp32.tobytes()

    loaded_int8 = load(kv, rev, "int8")
    assert set(loaded_int8) == {"layer0.bias"}
    assert loaded_int8["layer0.bias"] == b_int8.tobytes()

    # load_arrays reshapes via dtype/shape and matches the originals exactly.
    arrs = load_arrays(kv, rev, "fp16")
    np.testing.assert_array_equal(arrs["layer0.weight"], a_fp16)
    np.testing.assert_array_equal(arrs["layer1.weight"], c_fp16)
