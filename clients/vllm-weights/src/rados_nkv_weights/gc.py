# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""Garbage collection for the Weights catalog: mark-sweep over LIVE manifests.

Content-hash Chunk keys are SHARED across Model revisions / Precision variants /
models (ADR-0009): identical chunk bytes dedup to one stored Value, so removing
a model can NOT naively delete "its" chunks — another live model may reference
the very same Value. Deletion is therefore deferred and reclaimed by a
**mark-sweep** over the set of LIVE model revisions (ADR-0009):

1. **Mark** — for every live revision, Retrieve+parse its Weight manifest and
   collect ALL referenced Value (Chunk) keys, across every precision and every
   tensor, into a *live-set*; add the revision's own Manifest key too.
2. **Sweep** — enumerate every key in the namespace (:meth:`KvClient.iter_keys`)
   and delete (:meth:`KvClient.delete`) everything NOT in the live-set.

Correctness invariants:

- A Value referenced by ANY live manifest is never deleted — including a chunk
  shared by two models where only one is being removed: it stays because the
  other model is live and its manifest still references it.
- The Manifest key of a live revision is itself live (never swept).

**Fail-safe (this is load-bearing):** the sweep deletes "everything not marked
live", so it is only safe if the live-set is COMPLETE. If a live revision's
manifest is MISSING, or present-but-unparseable, we cannot enumerate that
model's chunks — proceeding would risk sweeping live chunks. So GC **aborts**
(raises :class:`GcUnsafeError`) and deletes NOTHING when any live revision's
manifest cannot be read. The missing/unreadable revisions are reported on the
exception (and, for a clean missing-manifest case, would otherwise be counted)
so the operator can fix the live-set and re-run. We never sweep on a possibly
incomplete live-set.

Runs on a privileged **admin NVMe-KV** connection (Store/Delete permitted); the
read-only loader handle rejects :meth:`KvClient.delete` (ADR-0008).
"""

import argparse
import sys
from dataclasses import dataclass, field
from typing import Iterable, List, Set

from .keys import canonical_revision, manifest_key
from .kvclient import KvClient
from .manifest import WeightManifest


class GcUnsafeError(RuntimeError):
    """A live revision's manifest could not be read, so the live-set is
    incomplete and a sweep would risk deleting live chunks. GC aborted WITHOUT
    deleting anything (fail-safe). ``unreadable`` lists the offending revisions.
    """

    def __init__(self, unreadable: List[str]) -> None:
        self.unreadable = list(unreadable)
        super().__init__(
            "GC aborted (fail-safe): cannot read the manifest of live "
            f"revision(s) {self.unreadable!r}; the live-set is incomplete so "
            "no sweep was performed. Fix the live-set (or republish the "
            "missing manifest) and re-run."
        )


@dataclass
class GcStats:
    """Outcome of a :func:`gc` run.

    - ``live_chunks``:    distinct Value (Chunk) keys referenced by live
      manifests (the manifest keys themselves are excluded from this count).
    - ``live_manifests``: live revisions whose manifest was found and marked.
    - ``swept``:          keys deleted (or, with ``dry_run``, that WOULD be).
    - ``bytes_swept``:    total bytes of the swept Values (best-effort: counted
      from a Retrieve before delete; ``0`` for any key that could not be sized).
    - ``dry_run``:        whether this was a dry run (nothing deleted).
    - ``swept_keys``:     the exact keys swept / to-be-swept (for assertions).
    """

    live_chunks: int = 0
    live_manifests: int = 0
    swept: int = 0
    bytes_swept: int = 0
    dry_run: bool = False
    swept_keys: List[bytes] = field(default_factory=list)


def _mark(kv: KvClient, live_revisions: Iterable[str]) -> "tuple[Set[bytes], int]":
    """Build the live-set from the live revisions. FAIL-SAFE.

    Returns ``(live_set, live_manifest_count)``. Raises :class:`GcUnsafeError`
    (before any deletion can happen) if any live revision's manifest is missing
    or unparseable — its chunks are unknown and must not be swept.
    """
    live: Set[bytes] = set()
    unreadable: List[str] = []
    live_manifests = 0

    # De-dup revisions by their canonical manifest key so two spellings of the
    # same revision don't double-count.
    seen_mkeys: Set[bytes] = set()
    for rev in live_revisions:
        mkey = manifest_key(rev)
        if mkey in seen_mkeys:
            continue
        seen_mkeys.add(mkey)

        raw = kv.retrieve(mkey)
        if raw is None:
            unreadable.append(canonical_revision(rev))
            continue
        try:
            manifest = WeightManifest.from_ipc(raw)
        except Exception:
            unreadable.append(canonical_revision(rev))
            continue

        # The manifest key itself is live (never swept).
        live.add(mkey)
        live_manifests += 1
        # Every referenced Value key, across every precision and tensor.
        for precision in manifest.precisions():
            for tensor in manifest.tensors(precision=precision):
                for key in manifest.chunk_keys(tensor, precision):
                    live.add(bytes(key))

    if unreadable:
        # Fail-safe: incomplete live-set -> abort, delete nothing.
        raise GcUnsafeError(unreadable)

    return live, live_manifests


def gc(
    kv: KvClient,
    live_revisions: Iterable[str],
    *,
    dry_run: bool = False,
) -> GcStats:
    """Mark-sweep reclaim of orphaned Values/manifests in the Weights catalog.

    ``live_revisions`` is the set of Model revisions that must be PRESERVED.
    Every Value key referenced by a live manifest (all precisions, all tensors)
    plus each live Manifest key is marked live; every other enumerated key is an
    orphan and is deleted (unless ``dry_run``).

    FAIL-SAFE: if any live revision's manifest is missing or unparseable the
    live-set is incomplete, so GC aborts with :class:`GcUnsafeError` and deletes
    NOTHING (a shared/unknown chunk must never be swept on an incomplete mark).

    With ``dry_run=True`` nothing is deleted, but the returned
    :class:`GcStats` reports the exact set that WOULD be swept (``swept`` /
    ``swept_keys``), so a sweep can be previewed before committing.

    Requires an admin :class:`KvClient` (Delete permitted) and a working
    :meth:`KvClient.iter_keys`; the read-only loader handle rejects delete, and
    :class:`~rados_nkv_weights.nvmekv_client.NvmeKvClient` cannot enumerate over
    NVMe-KV (see its ``iter_keys`` docstring for the rados-side strategy).
    """
    # Mark first (and fail safe) BEFORE enumerating/deleting anything.
    live, live_manifests = _mark(kv, live_revisions)

    stats = GcStats(
        live_chunks=len(live) - live_manifests,
        live_manifests=live_manifests,
        dry_run=dry_run,
    )

    for key in kv.iter_keys():
        key = bytes(key)
        if key in live:
            continue
        # Orphan -> sweep.
        value = kv.retrieve(key)
        if value is not None:
            stats.bytes_swept += len(value)
        stats.swept += 1
        stats.swept_keys.append(key)
        if not dry_run:
            kv.delete(key)

    return stats


def main(argv=None) -> int:
    """CLI: ``python -m rados_nkv_weights.gc --live rev1 --live rev2 [--dry-run]``.

    Mark-sweeps a Weights catalog given the LIVE Model revisions. Since only the
    in-memory transport is concrete today (the real admin NVMe-KV client is a
    TODO, and it cannot enumerate over NVMe-KV anyway — see
    :meth:`~rados_nkv_weights.nvmekv_client.NvmeKvClient.iter_keys`), this CLI
    runs against a fresh, empty :class:`~rados_nkv_weights.kvclient.InMemoryKvClient`:
    a demonstration of the GC flow/accounting rather than a persistent job.
    """
    parser = argparse.ArgumentParser(
        prog="python -m rados_nkv_weights.gc",
        description=(
            "Garbage-collect orphaned chunks/manifests from a Weights catalog "
            "via mark-sweep over the LIVE model revisions (NVMe-KV-on-RADOS)."
        ),
    )
    parser.add_argument(
        "--live",
        metavar="MODEL_REVISION",
        action="append",
        default=[],
        help=(
            "A Model revision that must be PRESERVED. Repeat for each live "
            "revision; everything not referenced by a live manifest is swept."
        ),
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Report what WOULD be swept without deleting anything.",
    )
    args = parser.parse_args(argv)

    # NOTE: real deployments inject an admin NVMe-KV client AND an out-of-band
    # key enumeration source here (TODO; NVMe-KV List is deferred). The CLI uses
    # a fresh in-memory client because it is the only concrete transport today.
    from .kvclient import InMemoryKvClient

    kv = InMemoryKvClient()
    try:
        stats = gc(kv, args.live, dry_run=args.dry_run)
    except GcUnsafeError as exc:
        print(f"GC aborted (fail-safe): {exc}", file=sys.stderr)
        return 1

    mode = "would sweep" if stats.dry_run else "swept"
    print(
        f"GC: live_manifests={stats.live_manifests} "
        f"live_chunks={stats.live_chunks} "
        f"{mode}={stats.swept} bytes_swept={stats.bytes_swept}",
        file=sys.stdout,
    )
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
