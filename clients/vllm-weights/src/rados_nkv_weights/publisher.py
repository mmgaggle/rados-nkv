# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""Weights publisher — the write path.

Ingests a model's tensors, splits each into Weight chunks bounded by the value
cap, Stores each chunk under its content-hash Chunk key — skipping any that
already Exist (dedup) — and writes the per-Model-revision Weight manifest under
its deterministic Manifest key.

Runs out-of-band on a privileged **admin NVMe-KV** connection permitted to
Store; the read-only loader fleet never writes.
"""

import argparse
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

from .chunking import iter_chunks
from .config import DEFAULT_MAX_VALUE_LEN
from .keys import canonical_revision, chunk_key, manifest_key
from .kvclient import KvClient
from .manifest import WeightManifest

#: tensors mapping shape:
#:   {tensor_name: {precision: (dtype:str, shape:tuple[int,...], data:bytes)}}
TensorsSpec = Dict[str, Dict[str, Tuple[str, Tuple[int, ...], bytes]]]


@dataclass
class PublishStats:
    """Outcome of a :func:`publish` call.

    - ``chunks_total``:   tensor slices produced across all tensors/precisions.
    - ``values_total``:   distinct Values (packed or whole) referenced.
    - ``values_stored``:  Values actually Stored (new content).
    - ``values_deduped``: Values skipped because the content key already Existed.
    - ``bytes_total``:    total tensor bytes processed (pre-dedup).

    ``chunks_stored`` / ``chunks_deduped`` remain as aliases of the per-Value
    counters for backward compatibility (each packed Value is counted once).
    """

    chunks_total: int = 0
    values_total: int = 0
    values_stored: int = 0
    values_deduped: int = 0
    bytes_total: int = 0

    @property
    def chunks_stored(self) -> int:
        return self.values_stored

    @property
    def chunks_deduped(self) -> int:
        return self.values_deduped


@dataclass
class _Slice:
    """One slice of a tensor: ``(key, offset, size)`` recorded on flush."""

    tensor: str
    precision: str
    seq: int  # position of this slice in the tensor's slice list


@dataclass
class _Pending:
    """A Value being assembled (one or more packed slices) prior to flush."""

    buf: bytearray = field(default_factory=bytearray)
    members: List[Tuple[_Slice, int, int]] = field(default_factory=list)
    # members: (slice, offset_within_value, size)


class _Builder:
    """Assembles slice records per (tensor, precision) and flushes Values.

    The manifest can only learn a packed Value's content-hash key after the
    Value's bytes are sealed, so slice records are buffered and filled in when
    their containing Value flushes.
    """

    def __init__(self, kv: KvClient, max_value_len: int, pack: bool) -> None:
        self._kv = kv
        self._max = max_value_len
        self._pack = pack
        self._stats = PublishStats()
        # (tensor, precision) -> ordered list of [key, offset, size] (mutable)
        self._records: Dict[Tuple[str, str], List[List]] = {}
        self._pending = _Pending()

    def _new_slice(self, tensor: str, precision: str, size: int) -> _Slice:
        recs = self._records.setdefault((tensor, precision), [])
        seq = len(recs)
        recs.append([None, None, size])  # filled on flush
        self._stats.chunks_total += 1
        return _Slice(tensor, precision, seq)

    def add_tensor(self, tensor: str, precision: str, data: bytes) -> None:
        """Chunk ``data`` to the cap and append its slices, packing sub-cap
        pieces into shared Values when packing is enabled."""
        self._stats.bytes_total += len(data)
        if len(data) == 0:
            # No slices; the tensor reconstructs to b"" from zero parts.
            self._records.setdefault((tensor, precision), [])
            return
        for chunk in iter_chunks(data, max_len=self._max):
            sl = self._new_slice(tensor, precision, len(chunk))
            if (not self._pack) or len(chunk) == self._max:
                # A whole-Value chunk: never share, store as its own Value.
                self._flush_pending()
                self._store_whole(sl, chunk)
            else:
                self._append_packed(sl, chunk)
        # Packing across tensor boundaries is allowed; pending stays open.

    def _append_packed(self, sl: _Slice, chunk: bytes) -> None:
        if len(self._pending.buf) + len(chunk) > self._max:
            self._flush_pending()
        offset = len(self._pending.buf)
        self._pending.buf.extend(chunk)
        self._pending.members.append((sl, offset, len(chunk)))

    def _store_whole(self, sl: _Slice, chunk: bytes) -> None:
        key = chunk_key(chunk)
        self._store_value(key, chunk)
        self._fill(sl, key, 0, len(chunk))

    def _flush_pending(self) -> None:
        p = self._pending
        if not p.members:
            return
        value = bytes(p.buf)
        key = chunk_key(value)
        self._store_value(key, value)
        for sl, offset, size in p.members:
            self._fill(sl, key, offset, size)
        self._pending = _Pending()

    def _store_value(self, key: bytes, value: bytes) -> None:
        self._stats.values_total += 1
        if self._kv.exists(key):
            self._stats.values_deduped += 1
        else:
            self._kv.store(key, value)
            self._stats.values_stored += 1

    def _fill(self, sl: _Slice, key: bytes, offset: int, size: int) -> None:
        rec = self._records[(sl.tensor, sl.precision)][sl.seq]
        rec[0] = key
        rec[1] = offset
        assert rec[2] == size

    def finalize(
        self,
        tensors: TensorsSpec,
        model_revision: str,
        base: Optional[WeightManifest] = None,
    ) -> PublishStats:
        """Build (or extend) the Weight manifest and Store it.

        When ``base`` is given (a manifest already in the catalog for this
        Model revision), the tensors/precisions assembled by this builder are
        *added to* it rather than replacing it — so publishing a new Precision
        variant of an already-published model keeps the existing variants. A
        precision already present for a tensor in ``base`` is an error
        (re-publishing the same precision must go through dedup, not clobber the
        manifest's record of it), surfaced by :meth:`WeightManifest.add_tensor`.
        """
        self._flush_pending()
        manifest = base if base is not None else WeightManifest()
        for tensor_name, by_precision in tensors.items():
            for precision, (dtype, shape, _data) in by_precision.items():
                recs = self._records.get((tensor_name, precision), [])
                keys = [r[0] for r in recs]
                offsets = [r[1] for r in recs]
                sizes = [r[2] for r in recs]
                manifest.add_tensor(
                    tensor_name,
                    precision,
                    dtype,
                    tuple(shape),
                    keys,
                    sizes,
                    offsets=offsets,
                )
        self._kv.store(manifest_key(model_revision), manifest.to_ipc())
        return self._stats


def publish(
    kv: KvClient,
    model_revision: str,
    tensors: TensorsSpec,
    max_value_len: Optional[int] = None,
    pack: bool = True,
    merge: bool = False,
) -> PublishStats:
    """Publish a Model revision's tensors into the Weights catalog.

    Chunking respects a Value cap derived, in order of preference, from the
    explicit ``max_value_len`` argument, then the bound KV namespace's
    advertised ``kvvml`` (:attr:`KvClient.max_value_len`), falling back to
    :data:`config.DEFAULT_MAX_VALUE_LEN`. No stored Value ever exceeds this cap.

    Each tensor is split into chunks ``<= max_value_len``. When ``pack`` is
    True (the default), sub-cap chunks from one or more tensors are **packed**
    into a single shared Value to cut object count and Store/Retrieve
    round-trips; the manifest records each tensor slice as ``(key, offset,
    size)`` so the loader slices the exact bytes back out. A chunk that fills
    the whole cap is stored as its own Value (never packed).

    Each Value's key is the content hash of *its* bytes, so identical Values
    dedup via Exist-before-Store. **Trade-off:** packing reduces
    dedup *granularity* — two models that share a tensor but pack it alongside
    different neighbours produce different Values and will not dedup that
    tensor. Pass ``pack=False`` to keep one-Value-per-chunk content addressing
    (maximal dedup, more objects) when cross-model sharing matters more than
    object count.

    Records the ordered slices into a :class:`WeightManifest`, then Stores the
    manifest under :func:`manifest_key(model_revision)
    <rados_nkv_weights.keys.manifest_key>`. Returns a :class:`PublishStats`.

    When ``merge`` is True and a manifest already exists for ``model_revision``,
    the freshly-assembled tensors/precisions are **added to** that existing
    manifest (re-read, extended, re-Stored) rather than replacing it. This is
    how a catalog accumulates multiple Precision variants of one model — publish
    fp16, then publish fp8 with ``merge=True`` into the same revision and both
    coexist in one manifest. Content-hash dedup is independent of the
    manifest, so identical Values across precisions/calls still Store once.
    Re-publishing a precision that is already recorded for a tensor raises (the
    manifest builder rejects a duplicate precision column for a tensor).
    """
    if max_value_len is None:
        max_value_len = getattr(kv, "max_value_len", DEFAULT_MAX_VALUE_LEN)
    if max_value_len <= 0:
        raise ValueError(f"max_value_len must be positive, got {max_value_len}")

    base: Optional[WeightManifest] = None
    if merge:
        existing = kv.retrieve(manifest_key(model_revision))
        if existing is not None:
            base = WeightManifest.from_ipc(existing)

    builder = _Builder(kv, max_value_len, pack)
    for tensor_name, by_precision in tensors.items():
        for precision, (_dtype, _shape, data) in by_precision.items():
            builder.add_tensor(tensor_name, precision, data)
    return builder.finalize(tensors, model_revision, base=base)


def _collect_safetensors_files(path: str) -> List[str]:
    """Resolve ``path`` to the ordered list of ``.safetensors`` shard files.

    Accepts:
    - a single ``.safetensors`` file -> ``[path]``;
    - a ``*.index.json`` (or ``*.safetensors.index.json``) -> the distinct shard
      filenames listed in its ``weight_map``, resolved relative to the index;
    - a directory -> its ``model.safetensors.index.json`` if present (sharded),
      else every ``*.safetensors`` file inside it (sorted for stable ordering).
    """
    import glob
    import json
    import os

    if os.path.isdir(path):
        index = os.path.join(path, "model.safetensors.index.json")
        if os.path.isfile(index):
            return _collect_safetensors_files(index)
        files = sorted(glob.glob(os.path.join(path, "*.safetensors")))
        if not files:
            raise FileNotFoundError(
                f"no .safetensors files found in directory {path!r}"
            )
        return files

    if path.endswith(".json"):
        with open(path, "r", encoding="utf-8") as fh:
            index = json.load(fh)
        weight_map = index.get("weight_map", {})
        if not weight_map:
            raise ValueError(f"index {path!r} has no 'weight_map'")
        base = os.path.dirname(path)
        # Preserve first-seen order; dedup shard filenames.
        seen: Dict[str, None] = {}
        for shard in weight_map.values():
            seen.setdefault(shard, None)
        return [os.path.join(base, shard) for shard in seen]

    return [path]


def publish_safetensors(
    kv: KvClient,
    model_revision: str,
    path: str,
    precision: str,
    max_value_len: Optional[int] = None,
    pack: bool = True,
    merge: bool = True,
) -> PublishStats:
    """Load model tensors from safetensors and publish them under one Precision
    variant.

    ``path`` may be a single ``.safetensors`` file, a sharded-model
    ``*.index.json`` (its ``weight_map`` names the shards), or a directory
    (its ``model.safetensors.index.json`` if sharded, else every
    ``*.safetensors`` inside). All shards are read and their tensors merged into
    one ``{name: {precision: (dtype, shape, bytes)}}`` spec fed to
    :func:`publish`.

    The ``safetensors`` dependency is optional (extra ``[publish]``) and imported
    lazily so the catalog core has no hard safetensors/torch/HF dependency. The
    numpy backend extracts raw bytes + dtype + shape without importing torch.

    ``merge`` defaults to True so publishing a second precision into an
    already-published ``model_revision`` *adds* its columns to the existing
    manifest rather than clobbering the other precisions (mixed-precision
    catalog).
    """
    from .safetensors_raw import read_tensors

    tensors: TensorsSpec = {}
    for shard in _collect_safetensors_files(path):
        for name, (dtype, shape, data) in read_tensors(shard).items():
            if name in tensors:
                raise ValueError(
                    f"tensor {name!r} appears in more than one shard of {path!r}"
                )
            tensors[name] = {precision: (dtype, shape, data)}
    return publish(
        kv,
        model_revision,
        tensors,
        max_value_len=max_value_len,
        pack=pack,
        merge=merge,
    )


def publish_huggingface(
    kv: KvClient,
    repo_id: str,
    precision: str,
    revision: Optional[str] = None,
    model_revision: Optional[str] = None,
    max_value_len: Optional[int] = None,
    pack: bool = True,
    merge: bool = True,
    cache_dir: Optional[str] = None,
    token: Optional[str] = None,
) -> PublishStats:
    """Resolve + download a HuggingFace Hub model's safetensors and publish them.

    NETWORK-DEPENDENT: this downloads the model's safetensors weights from the
    Hub via ``huggingface_hub`` (optional ``[publish]`` extra, lazy import). It
    delegates to :func:`publish_safetensors`, so all the shard/index handling,
    dtype/shape/byte extraction, dedup, and multi-precision manifest merge are
    shared with the local path.

    The Model revision recorded in the catalog defaults to
    ``f"{repo_id}@{revision or 'main'}"`` (then canonicalized by the keying
    layer), so the same Hub model at the same Hub revision keys one manifest
    across precisions. Pass ``model_revision`` to override.
    """
    try:
        from huggingface_hub import snapshot_download  # type: ignore
    except ImportError as exc:  # pragma: no cover - exercised only without extra
        raise ImportError(
            "publish_huggingface requires the optional 'huggingface_hub' "
            "dependency; install with: pip install 'rados-nkv-weights[publish]'"
        ) from exc

    if model_revision is None:
        model_revision = f"{repo_id}@{revision or 'main'}"
    model_revision = canonical_revision(model_revision)

    # Fetch only the safetensors weights + their shard index (skip the rest of
    # the repo). snapshot_download returns the local snapshot directory, which
    # publish_safetensors then resolves to the file/shards/index.
    local_dir = snapshot_download(
        repo_id,
        revision=revision,
        allow_patterns=["*.safetensors", "*.safetensors.index.json"],
        cache_dir=cache_dir,
        token=token,
    )
    return publish_safetensors(
        kv,
        model_revision,
        local_dir,
        precision,
        max_value_len=max_value_len,
        pack=pack,
        merge=merge,
    )


def main(argv=None) -> int:
    """CLI: ``python -m rados_nkv_weights.publisher``.

    Ingests a model's safetensors — from a local file/dir/index (``--safetensors``)
    or a HuggingFace Hub repo id (``--hf``) — and publishes it into a Weights
    catalog at the chosen ``--precision``. Since only the in-memory transport is
    concrete today (the real admin NVMe-KV client is a TODO), this CLI publishes
    into an :class:`~rados_nkv_weights.kvclient.InMemoryKvClient`: a demonstration
    of the ingest flow rather than a persistent production job.
    """
    parser = argparse.ArgumentParser(
        prog="python -m rados_nkv_weights.publisher",
        description="Publish model weights into a Weights catalog (NVMe-KV-on-RADOS).",
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument(
        "--safetensors",
        metavar="PATH",
        help=(
            "Path to a .safetensors file, a *.index.json shard index, or a "
            "directory of shards"
        ),
    )
    source.add_argument(
        "--hf",
        metavar="REPO_ID",
        help=(
            "HuggingFace Hub model id to download + publish (NETWORK-DEPENDENT; "
            "requires the [publish] extra)"
        ),
    )
    parser.add_argument(
        "--model-revision",
        default=None,
        help=(
            "Model revision identity that keys the manifest. Defaults to the "
            "safetensors path, or '<repo_id>@<hf-revision>' for --hf."
        ),
    )
    parser.add_argument(
        "--hf-revision",
        default=None,
        help="Hub git revision (branch/tag/sha) for --hf (default: main)",
    )
    parser.add_argument(
        "--precision",
        default="fp16",
        help="Precision variant label to record (default: fp16)",
    )
    parser.add_argument(
        "--max-value-len",
        type=int,
        default=None,
        help=(
            "Max Value size in bytes (default: derive from the KV namespace "
            "kvvml, else the 64 MiB librados cap)"
        ),
    )
    parser.add_argument(
        "--no-pack",
        action="store_true",
        help=(
            "Disable small-tensor packing (one Value per chunk; maximal "
            "cross-model dedup, more objects)"
        ),
    )
    parser.add_argument(
        "--no-merge",
        action="store_true",
        help=(
            "Replace any existing manifest for this revision instead of merging "
            "the new precision into it (default: merge, so precisions accumulate)"
        ),
    )
    parser.add_argument(
        "--vfu-addr",
        default=None,
        metavar="DIR",
        help=(
            "Publish over a real NVMe-KV namespace via the admin NvmeKvClient: "
            "DIR is the VFIOUSER controller socket directory of a running "
            "nvmf_tgt. Omit to use the in-memory transport (flow demo only)."
        ),
    )
    parser.add_argument(
        "--nsid",
        type=int,
        default=0,
        help="KV namespace id to bind when --vfu-addr is given (default: 0 = first KV ns)",
    )
    args = parser.parse_args(argv)

    kv, close = _open_publisher_kv(args.vfu_addr, args.nsid)
    # With a live namespace, default the chunk cap to the advertised kvvml so a
    # Store never exceeds the device's max value length.
    max_value_len = args.max_value_len
    if max_value_len is None and getattr(kv, "max_value_len", 0):
        max_value_len = kv.max_value_len
    try:
        if args.hf is not None:
            stats = publish_huggingface(
                kv,
                args.hf,
                args.precision,
                revision=args.hf_revision,
                model_revision=args.model_revision,
                max_value_len=max_value_len,
                pack=not args.no_pack,
                merge=not args.no_merge,
            )
            revision_label = args.model_revision or f"{args.hf}@{args.hf_revision or 'main'}"
        else:
            revision_label = args.model_revision or args.safetensors
            stats = publish_safetensors(
                kv,
                revision_label,
                args.safetensors,
                args.precision,
                max_value_len=max_value_len,
                pack=not args.no_pack,
                merge=not args.no_merge,
            )
    finally:
        close()
    print(
        f"published {revision_label!r} @ {args.precision}: "
        f"chunks_total={stats.chunks_total} "
        f"values_total={stats.values_total} "
        f"values_stored={stats.values_stored} "
        f"values_deduped={stats.values_deduped} "
        f"bytes_total={stats.bytes_total}",
        file=sys.stdout,
    )
    return 0


def _open_publisher_kv(vfu_addr, nsid):
    """Build the publisher's KvClient and a matching close() callback.

    Returns the real admin :class:`NvmeKvClient` (write path) when ``vfu_addr``
    is set, else the in-memory stub (a per-process flow demo that does not
    persist). The closer is a no-op for the stub.
    """
    if vfu_addr is None:
        from .kvclient import InMemoryKvClient

        return InMemoryKvClient(), (lambda: None)
    from .nvmekv_client import NvmeKvClient

    client = NvmeKvClient.open_publisher(vfu_addr, nsid=nsid)
    return client, client.close


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
