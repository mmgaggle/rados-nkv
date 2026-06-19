# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""Torch-free safetensors reading (handles bf16 / fp8).

Reads each tensor of a ``.safetensors`` file as ``(dtype_name, shape, raw_bytes)``
without importing torch. Where the dtype is numpy-representable it uses the
optional ``safetensors`` numpy backend (canonical dtype names, header
validation); for dtypes numpy cannot hold (``bfloat16`` and the fp8 variants) it
falls back to a direct header parse + byte slice.

The bytes are the exact on-disk little-endian payload either way, so a
publish -> load round-trip is byte-exact for any dtype. Only
:func:`rados_nkv_weights.loader.load_arrays` (which reshapes via numpy) is
limited to numpy-representable dtypes; the raw ``load`` bytes path is not.

This is why the catalog can carry a bf16 model (e.g. ibm-granite/granite-4.0-h)
even though numpy has no bfloat16: the publisher stores the raw bytes under the
recorded dtype string, and the loader returns them verbatim.
"""

import json
import os
import struct
from typing import Dict, Tuple

#: Upper bound on the JSON header length, mirroring the official safetensors
#: limit. A corrupt or hostile file can declare an arbitrary 8-byte length; we
#: refuse to allocate for it rather than risk memory exhaustion.
MAX_HEADER_BYTES = 100 * 1024 * 1024

#: safetensors dtype code -> canonical (numpy-style) dtype name recorded in the
#: Weight manifest. numpy can construct ``np.dtype(name)`` for every entry
#: EXCEPT ``bfloat16`` / the ``float8_*`` ones (those round-trip as raw bytes).
ST_DTYPE_TO_NAME = {
    "F64": "float64", "F32": "float32", "F16": "float16", "BF16": "bfloat16",
    "I64": "int64", "I32": "int32", "I16": "int16", "I8": "int8",
    "U64": "uint64", "U32": "uint32", "U16": "uint16", "U8": "uint8",
    "BOOL": "bool",
    "F8_E4M3": "float8_e4m3fn", "F8_E5M2": "float8_e5m2",
}

TensorEntry = Tuple[str, Tuple[int, ...], bytes]


def parse_header(path: str) -> Tuple[dict, int]:
    """Return ``(header_dict, data_section_offset)`` for a safetensors file.

    The format is: 8-byte little-endian header length N, N bytes of JSON header
    (tensor name -> {dtype, shape, data_offsets}), then the tensor data section
    that ``data_offsets`` index into.
    """
    size = os.path.getsize(path)
    with open(path, "rb") as fh:
        head = fh.read(8)
        if len(head) < 8:
            raise ValueError(f"{path!r}: truncated safetensors file (no header length)")
        (n,) = struct.unpack("<Q", head)
        if n > MAX_HEADER_BYTES:
            raise ValueError(
                f"{path!r}: safetensors header length {n} exceeds the "
                f"{MAX_HEADER_BYTES}-byte limit"
            )
        if 8 + n > size:
            raise ValueError(f"{path!r}: declared header length {n} runs past EOF")
        header = json.loads(fh.read(n))
        data_base = fh.tell()
    return header, data_base


def _read_raw(path: str, data_base: int, meta: dict) -> bytes:
    start, end = meta["data_offsets"]
    size = os.path.getsize(path)
    if not (0 <= start <= end) or data_base + end > size:
        raise ValueError(
            f"{path!r}: tensor data_offsets [{start}, {end}] are out of range "
            f"for a {size}-byte file (data section starts at {data_base})"
        )
    with open(path, "rb") as fh:
        fh.seek(data_base + start)
        return fh.read(end - start)


def read_tensors(path: str) -> Dict[str, TensorEntry]:
    """Read every tensor of one ``.safetensors`` file as ``(dtype, shape, bytes)``.

    numpy-representable dtypes go through the ``safetensors`` numpy backend when
    it is installed; bf16/fp8 (and everything, if ``safetensors`` is absent) is
    read by slicing the raw byte range from the header. Either path yields the
    exact on-disk bytes.
    """
    header, data_base = parse_header(path)
    names = [k for k in header.keys() if k != "__metadata__"]

    try:
        from safetensors import safe_open  # type: ignore
    except ImportError:
        safe_open = None  # pure raw-slice path below

    out: Dict[str, TensorEntry] = {}
    sf = None
    if safe_open is not None:
        sf = safe_open(path, framework="numpy")
        sf.__enter__()
    try:
        for name in names:
            meta = header[name]
            shape = tuple(meta["shape"])
            if sf is not None:
                try:
                    arr = sf.get_tensor(name)
                    out[name] = (str(arr.dtype), tuple(arr.shape), arr.tobytes())
                    continue
                except (TypeError, ValueError):
                    # numpy cannot represent this dtype (bf16/fp8): raw-slice it.
                    pass
            dt = ST_DTYPE_TO_NAME.get(meta["dtype"])
            if dt is None:
                raise ValueError(
                    f"unsupported safetensors dtype {meta['dtype']!r} for tensor "
                    f"{name!r} in {path!r}"
                )
            out[name] = (dt, shape, _read_raw(path, data_base, meta))
    finally:
        if sf is not None:
            sf.__exit__(None, None, None)
    return out


def iter_tensor_digests(path: str):
    """Yield ``(name, dtype, shape, nbytes, sha256_hexdigest)`` per tensor.

    Streams one tensor at a time (hash, then discard) so a multi-GB model can be
    fingerprinted for verification without holding the whole file in memory.
    """
    import hashlib

    header, data_base = parse_header(path)
    names = [k for k in header.keys() if k != "__metadata__"]
    for name in names:
        meta = header[name]
        raw = _read_raw(path, data_base, meta)
        dt = ST_DTYPE_TO_NAME.get(meta["dtype"], meta["dtype"])
        yield (
            name,
            dt,
            tuple(meta["shape"]),
            len(raw),
            hashlib.sha256(raw).hexdigest(),
        )
