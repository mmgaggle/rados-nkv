# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""Weights loader — the read path (ADR-0008 / ADR-0009).

Given a Model revision and a chosen Precision variant: Retrieve the Weight
manifest by its deterministic Manifest key, then for each tensor Retrieve its
Chunk keys in order, concatenate, and reassemble the tensor bytes. Runs
unprivileged on every GPU host against a **read-only** NVMe-KV namespace handle
(Retrieve/Exist only).
"""

import argparse
import sys
from typing import Dict

from .keys import canonical_revision, chunk_key, manifest_key
from .kvclient import KvClient
from .manifest import WeightManifest


class MissingChunkError(KeyError):
    """A Chunk key referenced by the manifest was absent from the catalog."""


class PrecisionNotFoundError(KeyError):
    """The requested Precision variant is not present in the manifest."""


class ManifestNotFoundError(KeyError):
    """No Weight manifest exists for the requested Model revision."""


class IntegrityError(ValueError):
    """A retrieved Weight chunk failed verification against its content-addressed
    Chunk key (the catalog returned bytes that do not hash to the key requested).
    """


def _load_manifest(kv: KvClient, model_revision: str) -> WeightManifest:
    raw = kv.retrieve(manifest_key(model_revision))
    if raw is None:
        raise ManifestNotFoundError(
            f"no Weight manifest for model revision "
            f"{canonical_revision(model_revision)!r} "
            f"(manifest key absent from catalog)"
        )
    return WeightManifest.from_ipc(raw)


def _reassemble(
    kv: KvClient, manifest: WeightManifest, model_revision: str, precision: str
) -> Dict[str, bytes]:
    """Reassemble the tensors of ``manifest`` published at ``precision``.

    Loading precision ``p`` materializes **exactly** the tensors published in
    ``p`` (``manifest.tensors(precision=p)``); tensors lacking that precision
    are skipped rather than raising. Each retrieved chunk is verified two ways:
    its length must match the manifest, and it must hash back to the requested
    content-addressed Chunk key (else :class:`IntegrityError`).
    """
    if precision not in manifest.precisions():
        raise PrecisionNotFoundError(
            f"precision {precision!r} not in manifest for "
            f"{canonical_revision(model_revision)!r}; "
            f"available: {manifest.precisions()}"
        )

    # Cache verified Values across tensors: a packed Value shared by several
    # tensors is Retrieved and integrity-checked once, then sliced many times.
    verified: Dict[bytes, bytes] = {}

    def _get_verified(key: bytes, tensor: str) -> bytes:
        cached = verified.get(key)
        if cached is not None:
            return cached
        value = kv.retrieve(key)
        if value is None:
            raise MissingChunkError(
                f"value {key.hex()} for tensor {tensor!r} "
                f"({model_revision!r}/{precision}) missing from catalog"
            )
        # Verify the WHOLE Value hashes to its content-addressed key BEFORE
        # slicing (Slice-2 integrity guarantee): the key commits to every byte
        # of the Value, so any corruption — inside or outside this tensor's
        # window — is caught here, not silently sliced past.
        actual_key = chunk_key(value)
        if actual_key != key:
            raise IntegrityError(
                f"value for tensor {tensor!r} "
                f"({model_revision!r}/{precision}) failed integrity check: "
                f"requested key {key.hex()} but retrieved bytes hash "
                f"to {actual_key.hex()}"
            )
        verified[key] = value
        return value

    out: Dict[str, bytes] = {}
    for tensor in manifest.tensors(precision=precision):
        slices = manifest.chunk_slices(tensor, precision)
        parts = []
        for key, offset, size in slices:
            value = _get_verified(key, tensor)
            end = offset + size
            if offset < 0 or end > len(value):
                raise ValueError(
                    f"slice [{offset}:{end}] for tensor {tensor!r} out of range "
                    f"for value {key.hex()} of length {len(value)}"
                )
            piece = value[offset:end]
            parts.append(piece)
        data = b"".join(parts)
        total = sum(size for _k, _o, size in slices)
        if len(data) != total:
            raise ValueError(
                f"reassembled tensor {tensor!r} is {len(data)} bytes, "
                f"manifest expected {total}"
            )
        out[tensor] = data
    return out


def load(kv: KvClient, model_revision: str, precision: str) -> Dict[str, bytes]:
    """Reassemble the tensors of ``model_revision`` published at ``precision``.

    **Contract:** loading precision ``p`` materializes exactly the tensors that
    were published in ``p``. In a manifest with mixed per-tensor precision, a
    tensor present only in another precision is skipped (not an error).

    **Integrity:** every retrieved Weight chunk is verified before use — its
    length must match the manifest, and it must hash back to the
    content-addressed Chunk key it was fetched by (:func:`keys.chunk_key`),
    guarding against silent corruption. The reassembled tensor length is
    likewise checked.

    Returns ``{tensor_name: reassembled_bytes}``. Raises
    :class:`ManifestNotFoundError` if the revision has no manifest,
    :class:`PrecisionNotFoundError` if ``precision`` is absent from the manifest,
    :class:`MissingChunkError` if a referenced Chunk key is missing, and
    :class:`IntegrityError` if a chunk fails its content-address check.
    """
    manifest = _load_manifest(kv, model_revision)
    return _reassemble(kv, manifest, model_revision, precision)


def load_arrays(kv: KvClient, model_revision: str, precision: str):
    """Like :func:`load`, but reshape each tensor to a numpy array via the
    manifest's dtype/shape.

    Shares a **single** manifest fetch/parse with the reassembly step (the
    manifest is not retrieved twice). Same contract and integrity guarantees as
    :func:`load`.

    Returns ``{tensor_name: numpy.ndarray}``.
    """
    import numpy as np

    manifest = _load_manifest(kv, model_revision)
    raw = _reassemble(kv, manifest, model_revision, precision)
    arrays = {}
    for tensor, data in raw.items():
        dtype, shape, _sizes = manifest.tensor_meta(tensor, precision)
        arr = np.frombuffer(data, dtype=np.dtype(dtype))
        # Always reshape to the recorded shape, including 0-d scalars (shape ()):
        # a truthiness guard would skip () and leave a (1,) array, mangling scalars.
        arr = arr.reshape(tuple(shape))
        arrays[tensor] = arr
    return arrays


# ---------------------------------------------------------------------------
# vLLM --load-format integration
# ---------------------------------------------------------------------------
#
# The real, key-native vLLM loader lives in rados_nkv_weights.vllm_loader: a
# standalone --load-format=rados-nkv plugin (ADR-0008: "a standalone key-native
# vLLM --load-format plugin, not an extension of Run:ai Model Streamer"). It
# subclasses vLLM's BaseModelLoader and streams Weight chunks into the model's
# tensors. That module imports torch/vllm lazily and is NOT imported here, so
# this loader's pure-Python read path stays importable with neither installed.
#
# To register the plugin from a vLLM process:
#
#     from rados_nkv_weights.vllm_loader import register
#     register()  # then --load-format=rados-nkv selects it
#
# RadosNkvModelLoaderStub remains as a thin, torch/vllm-free integration seam
# for code/tests that just want "(model_revision, precision) -> arrays" without
# a live vLLM. It now delegates the actual streaming to the real module's
# building blocks.


class RadosNkvModelLoaderStub:
    """Lightweight, vLLM/torch-free seam onto the Weights catalog read path.

    The production ``--load-format=rados-nkv`` plugin is
    :class:`rados_nkv_weights.vllm_loader.RadosNkvModelLoader` (a real
    ``BaseModelLoader`` subclass). This small class stays as the dependency-free
    integration point: given a Model revision + Precision variant and a
    :class:`KvClient`, it reassembles the tensors so the read path is exercisable
    without vLLM/torch/GPU.

    Deliberately no ``import vllm`` / ``import torch`` here — that is what makes
    it usable in environments (and tests) where they are absent.
    """

    load_format_name = "rados-nkv"

    def __init__(self, kv: KvClient, model_revision: str, precision: str):
        self._kv = kv
        self._model_revision = model_revision
        self._precision = precision

    def load_weights(self, model=None):
        """Reassemble and return ``{tensor_name: numpy.ndarray}`` from the
        catalog (the same integrity-checked read path :func:`load_arrays` uses).

        The real plugin (:class:`vllm_loader.RadosNkvModelLoader`) instead yields
        ``(name, torch.Tensor)`` into ``model.load_weights``; this seam returns
        numpy arrays so the integration point works without torch. ``model`` is
        accepted (and ignored) to keep the call shape parallel.
        """
        return load_arrays(self._kv, self._model_revision, self._precision)

    def named_tensors(self):
        """Yield ``(name, torch.Tensor)`` exactly as the vLLM plugin feeds
        ``model.load_weights`` — for code/tests that want the real torch weight
        iterator without standing up vLLM. Requires torch (imported lazily by
        :func:`vllm_loader.iter_named_tensors`)."""
        from .vllm_loader import iter_named_tensors

        return iter_named_tensors(self._kv, self._model_revision, self._precision)


def main(argv=None) -> int:
    """CLI: ``python -m rados_nkv_weights.loader``.

    Loads a Model revision/Precision variant and prints a per-tensor summary.
    With ``--vfu-addr`` it reads a real read-only NVMe-KV namespace via
    :class:`NvmeKvClient`; without it, the in-memory transport loads an empty
    catalog (flow demo only). ``--verify PATH`` additionally checks every loaded
    tensor byte-for-byte against the source ``.safetensors``.
    """
    parser = argparse.ArgumentParser(
        prog="python -m rados_nkv_weights.loader",
        description="Load model weights from a Weights catalog (NVMe-KV-on-RADOS).",
    )
    parser.add_argument("model_revision", help="Model revision identity")
    parser.add_argument("precision", help="Precision variant to load (e.g. fp16)")
    parser.add_argument(
        "--arrays",
        action="store_true",
        help="Reshape tensors to numpy arrays (load_arrays) and print shapes "
        "(numpy-representable dtypes only; not bf16/fp8)",
    )
    parser.add_argument(
        "--vfu-addr",
        default=None,
        metavar="DIR",
        help=(
            "Load over a real read-only NVMe-KV namespace via NvmeKvClient: DIR "
            "is the VFIOUSER controller socket directory of a running nvmf_tgt. "
            "Omit to use the in-memory transport (empty catalog; flow demo only)."
        ),
    )
    parser.add_argument(
        "--nsid",
        type=int,
        default=0,
        help="KV namespace id to bind when --vfu-addr is given (default: 0 = first KV ns)",
    )
    parser.add_argument(
        "--verify",
        default=None,
        metavar="SAFETENSORS",
        help=(
            "Verify each loaded tensor byte-for-byte against this source "
            ".safetensors file/dir/index; exit nonzero on any mismatch"
        ),
    )
    args = parser.parse_args(argv)

    kv, close = _open_loader_kv(args.vfu_addr, args.nsid)
    try:
        if args.verify is not None:
            return _verify(kv, args.model_revision, args.precision, args.verify)
        if args.arrays:
            arrays = load_arrays(kv, args.model_revision, args.precision)
            for name, arr in arrays.items():
                print(f"{name}\t{arr.dtype}\t{tuple(arr.shape)}", file=sys.stdout)
        else:
            tensors = load(kv, args.model_revision, args.precision)
            total = 0
            for name, data in tensors.items():
                total += len(data)
                print(f"{name}\t{len(data)} bytes", file=sys.stdout)
            print(
                f"# loaded {len(tensors)} tensors, {total} bytes total",
                file=sys.stdout,
            )
    finally:
        close()
    return 0


def _open_loader_kv(vfu_addr, nsid):
    """Build the loader's KvClient and a matching close() callback.

    Returns the real read-only :class:`NvmeKvClient` (retrieve/exist only) when
    ``vfu_addr`` is set, else the in-memory stub. The closer is a no-op for the
    stub.
    """
    if vfu_addr is None:
        from .kvclient import InMemoryKvClient

        return InMemoryKvClient(), (lambda: None)
    from .nvmekv_client import NvmeKvClient

    client = NvmeKvClient.open_loader(vfu_addr, nsid=nsid)
    return client, client.close


def _verify(kv: KvClient, model_revision: str, precision: str, source: str) -> int:
    """Load every tensor and compare byte-for-byte to the source safetensors.

    Streams the source digests (one tensor at a time) so the peak memory is the
    loaded catalog plus a small digest map, not two full copies of the model.
    Returns 0 if every loaded tensor matches its source, else 1.
    """
    import hashlib

    from .publisher import _collect_safetensors_files
    from .safetensors_raw import iter_tensor_digests

    src = {}
    for shard in _collect_safetensors_files(source):
        for name, _dt, _shape, nbytes, sha in iter_tensor_digests(shard):
            src[name] = (sha, nbytes)

    tensors = load(kv, model_revision, precision)

    mismatches = []
    for name, data in tensors.items():
        if name not in src:
            mismatches.append(f"{name}: loaded but absent from source")
            continue
        sha, nbytes = src[name]
        if len(data) != nbytes:
            mismatches.append(f"{name}: length {len(data)} != source {nbytes}")
        elif hashlib.sha256(data).hexdigest() != sha:
            mismatches.append(f"{name}: sha256 mismatch")
    missing = set(src) - set(tensors)

    print(
        f"# verify: loaded={len(tensors)} source={len(src)} "
        f"mismatches={len(mismatches)} source-only={len(missing)}",
        file=sys.stdout,
    )
    if mismatches or missing:
        for m in mismatches[:20]:
            print(f"MISMATCH {m}", file=sys.stderr)
        if missing:
            print(
                f"MISSING (in source, not loaded): {sorted(missing)[:20]}",
                file=sys.stderr,
            )
        print("VERIFY FAILED", file=sys.stderr)
        return 1
    print(
        f"VERIFY OK: {len(tensors)} tensors byte-exact ({precision})",
        file=sys.stdout,
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
