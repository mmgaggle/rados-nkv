# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""The Weight manifest: an Arrow IPC table, one row per tensor.

The manifest is the per-Model-revision self-describing index — the safetensors
header analog. The Weights loader Retrieves and reads it first, picks a Precision
variant column, then Retrieves the listed Chunk keys in order.

Schema (one row per tensor):

- ``tensor_name``: ``string``
- ``shape``: ``list<int64>``
- per Precision variant ``p`` (a *set* of columns):
    - ``f"{p}__dtype"``:   ``string``
    - ``f"{p}__keys"``:    ``list<binary>``  (ordered 16-byte Value keys)
    - ``f"{p}__offsets"``: ``list<int64>``   (byte offset of this tensor's slice
      WITHIN the value named by the corresponding key)
    - ``f"{p}__sizes"``:   ``list<int64>``   (byte length of each slice, in order)

Each ``(key, offset, size)`` triple names a *slice* of a stored Value: the bytes
``value[offset : offset + size]`` of the Value retrieved under ``key``. A tensor
reconstructs by concatenating its slices in order. This generalizes the old
"one key == one whole chunk" model: small tensors (and sub-cap chunks) can be
*packed* into a single shared Value, each tensor pointing at its own
``(offset, size)`` window; an unpacked chunk is simply ``offset == 0`` and
``size == len(value)``. Because a packed Value's key is still the content hash of
its bytes, identical packed Values dedup (packing trades dedup granularity for
fewer objects — see :mod:`rados_nkv_weights.publisher`).

A precision column group is null for a tensor that does not carry that variant,
so multiple Precision variants coexist in one manifest (selective precision
load: a host materializes exactly one ``p``).
"""

from typing import Dict, List, Optional, Tuple

import pyarrow as pa


def _dtype_col(p: str) -> str:
    return f"{p}__dtype"


def _keys_col(p: str) -> str:
    return f"{p}__keys"


def _offsets_col(p: str) -> str:
    return f"{p}__offsets"


def _sizes_col(p: str) -> str:
    return f"{p}__sizes"


class _Row:
    __slots__ = ("shape", "variants")

    def __init__(self, shape: Tuple[int, ...]):
        self.shape: List[int] = list(shape)
        # precision -> (dtype, keys, offsets, sizes)
        self.variants: Dict[
            str, Tuple[str, List[bytes], List[int], List[int]]
        ] = {}


class WeightManifest:
    """In-memory builder/reader for a Weight manifest, backed by Arrow IPC.

    Build with :meth:`add_tensor`, serialize with :meth:`to_ipc`, and round-trip
    back with :meth:`from_ipc`. Read accessors (:meth:`precisions`,
    :meth:`tensors`, :meth:`chunk_keys`, :meth:`tensor_meta`) work on either a
    freshly-built or a deserialized manifest.
    """

    def __init__(self) -> None:
        # Ordered so the manifest is stable across runs (one row per tensor).
        self._rows: Dict[str, _Row] = {}
        # Track precision insertion order for stable columns.
        self._precisions: List[str] = []

    # -- build -------------------------------------------------------------

    def add_tensor(
        self,
        name: str,
        precision: str,
        dtype: str,
        shape: Tuple[int, ...],
        chunk_keys: List[bytes],
        sizes: List[int],
        offsets: Optional[List[int]] = None,
    ) -> None:
        """Record one tensor's Precision variant: its dtype/shape and the
        ordered ``(key, offset, size)`` slices that reconstruct it.

        Each entry names ``value[offset : offset + size]`` of the Value stored
        under ``key``. ``offsets`` defaults to all-zero (the unpacked case where
        each key maps to a whole Value); pass explicit offsets to point a tensor
        at a window inside a *packed* Value shared with other tensors.

        Calling with multiple ``precision`` values for the same ``name`` adds
        precision column groups to the same row. ``shape`` must agree across
        precisions of one tensor.
        """
        if offsets is None:
            offsets = [0] * len(chunk_keys)
        if not (len(chunk_keys) == len(sizes) == len(offsets)):
            raise ValueError(
                f"chunk_keys ({len(chunk_keys)}), offsets ({len(offsets)}) and "
                f"sizes ({len(sizes)}) length mismatch for tensor {name!r}"
            )
        row = self._rows.get(name)
        if row is None:
            row = _Row(shape)
            self._rows[name] = row
        elif list(shape) != row.shape:
            raise ValueError(
                f"shape mismatch for tensor {name!r}: "
                f"{tuple(row.shape)} vs {tuple(shape)}"
            )
        if precision in row.variants:
            raise ValueError(
                f"precision {precision!r} already recorded for tensor {name!r}"
            )
        if precision not in self._precisions:
            self._precisions.append(precision)
        row.variants[precision] = (
            dtype,
            list(chunk_keys),
            list(offsets),
            list(sizes),
        )

    # -- serialize ---------------------------------------------------------

    def _to_table(self) -> pa.Table:
        names = list(self._rows.keys())
        precisions = list(self._precisions)

        columns: Dict[str, pa.Array] = {}
        columns["tensor_name"] = pa.array(names, type=pa.string())
        columns["shape"] = pa.array(
            [self._rows[n].shape for n in names], type=pa.list_(pa.int64())
        )

        for p in precisions:
            dtypes: List[Optional[str]] = []
            keys: List[Optional[List[bytes]]] = []
            offsets: List[Optional[List[int]]] = []
            sizes: List[Optional[List[int]]] = []
            for n in names:
                v = self._rows[n].variants.get(p)
                if v is None:
                    dtypes.append(None)
                    keys.append(None)
                    offsets.append(None)
                    sizes.append(None)
                else:
                    dtype, ks, offs, szs = v
                    dtypes.append(dtype)
                    keys.append(ks)
                    offsets.append(offs)
                    sizes.append(szs)
            columns[_dtype_col(p)] = pa.array(dtypes, type=pa.string())
            columns[_keys_col(p)] = pa.array(keys, type=pa.list_(pa.binary()))
            columns[_offsets_col(p)] = pa.array(offsets, type=pa.list_(pa.int64()))
            columns[_sizes_col(p)] = pa.array(sizes, type=pa.list_(pa.int64()))

        # Stash precision order in schema metadata so from_ipc is order-stable.
        schema_meta = {b"precisions": ",".join(precisions).encode("utf-8")}
        table = pa.table(columns)
        return table.replace_schema_metadata(schema_meta)

    def to_ipc(self) -> bytes:
        """Serialize the manifest to Arrow IPC stream bytes (the Value stored
        under the Manifest key)."""
        table = self._to_table()
        sink = pa.BufferOutputStream()
        with pa.ipc.new_stream(sink, table.schema) as writer:
            writer.write_table(table)
        return sink.getvalue().to_pybytes()

    @classmethod
    def from_ipc(cls, data: bytes) -> "WeightManifest":
        """Reconstruct a :class:`WeightManifest` from Arrow IPC stream bytes."""
        reader = pa.ipc.open_stream(pa.py_buffer(data))
        table = reader.read_all()
        m = cls()

        meta = table.schema.metadata or {}
        raw = meta.get(b"precisions")
        if raw:
            decoded = raw.decode("utf-8")
            precisions = decoded.split(",") if decoded else []
        else:
            # Fallback: derive precisions from column names.
            precisions = [
                c[: -len("__dtype")]
                for c in table.column_names
                if c.endswith("__dtype")
            ]

        names = table.column("tensor_name").to_pylist()
        shapes = table.column("shape").to_pylist()

        # Offsets column is optional for forward/backward compat: a manifest
        # written before packing existed has no ``__offsets`` column, in which
        # case every key maps to a whole Value (offset 0).
        has_offsets = {
            p: _offsets_col(p) in table.column_names for p in precisions
        }

        per_p = {}
        for p in precisions:
            offs = (
                table.column(_offsets_col(p)).to_pylist()
                if has_offsets[p]
                else None
            )
            per_p[p] = (
                table.column(_dtype_col(p)).to_pylist(),
                table.column(_keys_col(p)).to_pylist(),
                offs,
                table.column(_sizes_col(p)).to_pylist(),
            )

        for i, name in enumerate(names):
            shape = tuple(shapes[i] or [])
            for p in precisions:
                dtypes, keys, offsets, sizes = per_p[p]
                if dtypes[i] is None:
                    continue
                off_i = list(offsets[i]) if offsets is not None else None
                m.add_tensor(
                    name,
                    p,
                    dtypes[i],
                    shape,
                    list(keys[i]),
                    list(sizes[i]),
                    offsets=off_i,
                )
        return m

    # -- read --------------------------------------------------------------

    def precisions(self) -> List[str]:
        """Return the Precision variants present in this manifest, in order."""
        return list(self._precisions)

    def tensors(self, precision: Optional[str] = None) -> List[str]:
        """Return tensor names (one per manifest row), in order.

        With ``precision`` given, return only the tensors whose precision column
        group is **non-null** — i.e. the tensors actually published in that
        Precision variant. This is the set a host materializes when loading that
        precision; tensors lacking it are skipped (selective per-tensor
        precision). With ``precision`` omitted, return all tensors.
        """
        if precision is None:
            return list(self._rows.keys())
        return [
            name
            for name, row in self._rows.items()
            if precision in row.variants
        ]

    def chunk_keys(self, tensor: str, precision: str) -> List[bytes]:
        """Return the ordered Value keys reconstructing ``tensor`` at
        ``precision``.

        Note: with packing, the same key can appear for multiple tensors (a
        shared packed Value). Use :meth:`chunk_slices` to also get each slice's
        offset/size within its Value.
        """
        return list(self._variant(tensor, precision)[1])

    def chunk_slices(
        self, tensor: str, precision: str
    ) -> List[Tuple[bytes, int, int]]:
        """Return the ordered ``(key, offset, size)`` slices reconstructing
        ``tensor`` at ``precision``.

        Each triple names ``value[offset : offset + size]`` of the Value stored
        under ``key``; concatenating the slices in order yields the tensor's
        bytes.
        """
        _dtype, keys, offsets, sizes = self._variant(tensor, precision)
        return list(zip(keys, offsets, sizes))

    def tensor_meta(
        self, tensor: str, precision: str
    ) -> Tuple[str, Tuple[int, ...], List[int]]:
        """Return ``(dtype, shape, sizes)`` for ``tensor`` at ``precision``."""
        row = self._rows[tensor]
        dtype, _keys, _offsets, sizes = self._variant(tensor, precision)
        return dtype, tuple(row.shape), list(sizes)

    def _variant(self, tensor: str, precision: str):
        row = self._rows.get(tensor)
        if row is None:
            raise KeyError(f"tensor {tensor!r} not in manifest")
        v = row.variants.get(precision)
        if v is None:
            raise KeyError(
                f"precision {precision!r} not present for tensor {tensor!r}"
            )
        return v
