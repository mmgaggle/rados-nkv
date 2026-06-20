#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""Rung-2 acceptance check (bead spdk-p9k.1.1).

Proves the GPU-direct vLLM loader emit: a model published into a live NVMe-KV
namespace, loaded through the GPU-direct datapath, yields tensors that are
DEVICE-resident (``.is_cuda``) and BYTE-EXACT vs the CPU loader's tensors
(``iter_named_tensors`` over the SPDK-NVMe host shim) for the same model/precision.

Both legs are LIVE over the same mem-backed nvmf_tgt (no Ceph):

  * Publisher + CPU reference (phase 1): the admin/read-only NvmeKvClient (SPDK
    NVMe host shim, libradosnkv_kvshim.so) publishes a small synthetic model and
    reads it back into HOST torch tensors (the existing iter_named_tensors).
  * GPU-direct loader (phase 2): the KvgHandle (libkvg_gpu.so) Retrieves each
    Value straight into a hipMalloc dma-buf GPU region and wraps the device
    pointer as a device-resident torch.Tensor (iter_named_tensors_gpu).

The two SPDK host clients each init the SPDK env, which DPDK allows only once per
process, so the two phases run as SEPARATE processes (this script re-execs itself
with --phase). Phase 1 writes a reference digest file; phase 2 compares against it.

Usage:
  python rung2_check.py --vfu-addr <DIR-with-cntrl>
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile

PRECISION = "bf16"
# A revision suffix lets a second run (e.g. pack=False) use a distinct catalog
# identity so it doesn't collide with the first publish's manifest.
MODEL_REV = "rung2-synthetic@" + os.environ.get("RUNG2_REV", "v1")


def _synthetic_tensors():
    """A small synthetic model: a mix of sizes/dtypes exercising the unpacked
    fast path (large tensors == whole Value) and the packed path (small tensors
    sharing a Value). Returns (TensorsSpec, {name: raw_bytes}).

    Bytes are deterministic so the reference is reproducible. dtype strings are
    the numpy/torch names the publisher records in the manifest.
    """
    import numpy as np

    specs = {}
    raws = {}

    def add(name, dtype_str, shape, seed):
        n = 1
        for d in shape:
            n *= d
        # bf16 is not a numpy dtype; synthesize raw bytes (2 bytes/elt) directly.
        if dtype_str == "bfloat16":
            rng = np.random.default_rng(seed)
            data = rng.integers(0, 256, size=n * 2, dtype=np.uint8).tobytes()
        else:
            npdt = np.dtype(dtype_str)
            rng = np.random.default_rng(seed)
            arr = (rng.standard_normal(n) * 10).astype(npdt)
            data = arr.tobytes()
        specs[name] = {PRECISION: (dtype_str, list(shape), data)}
        raws[name] = data

    # Large tensors -> own Value (unpacked fast path, direct GPU wrap).
    add("model.embed.weight", "bfloat16", (1024, 256), 1)   # 512 KiB
    add("model.layer0.mlp.weight", "float32", (512, 512), 2)  # 1 MiB
    add("model.layer0.attn.weight", "float16", (768, 256), 3)  # 384 KiB
    # Small tensors -> packed together into a shared Value (packed path).
    add("model.layer0.ln.bias", "float32", (256,), 4)
    add("model.layer0.ln.weight", "float32", (256,), 5)
    add("model.head.bias", "float32", (128,), 6)
    return specs, raws


def phase_publish_and_reference(vfu_addr, ref_path):
    """Phase 1 (own process): publish the synthetic model into the live target,
    then read it back with the CPU loader (HOST tensors) and write a reference
    digest file (per-tensor dtype/shape/sha256/nbytes)."""
    import torch  # noqa: F401  (loader imports it lazily; ensure it's present)

    from rados_nkv_weights.nvmekv_client import NvmeKvClient
    from rados_nkv_weights import publisher

    specs, raws = _synthetic_tensors()

    # --- publish via the admin (publisher) NvmeKvClient (live, SPDK NVMe shim).
    pub = NvmeKvClient.open_publisher(vfu_addr, nsid=0)
    try:
        pack = os.environ.get("RUNG2_PACK", "1") != "0"
        stats = publisher.publish(pub, MODEL_REV, specs, pack=pack)
        print(f"[phase1] published {len(specs)} tensors "
              f"(stored={stats.chunks_stored} deduped={stats.chunks_deduped})",
              file=sys.stderr)
    finally:
        pub.close()

    # The publisher closed its handle -> SPDK env torn down. The CPU reference
    # read needs a fresh env, which this process cannot re-init. So fork a child
    # for the read-back. We simply re-exec ourselves into the read-only sub-phase.
    sub = subprocess.run(
        [sys.executable, __file__, "--phase", "reference-read",
         "--vfu-addr", vfu_addr, "--ref", ref_path],
        env=os.environ.copy(),
    )
    if sub.returncode != 0:
        raise SystemExit(sub.returncode)

    # Sanity: the reference digests must match the source bytes we published.
    with open(ref_path) as fh:
        ref = json.load(fh)
    bad = 0
    for name, raw in raws.items():
        want = hashlib.sha256(raw).hexdigest()
        got = ref["tensors"].get(name, {}).get("sha256")
        if got != want:
            print(f"[phase1] REFERENCE MISMATCH vs source for {name}: "
                  f"{got} != {want}", file=sys.stderr)
            bad += 1
    if bad:
        raise SystemExit(f"[phase1] {bad} reference tensors disagree with source")
    print("[phase1] CPU reference matches published source bytes "
          f"({len(raws)} tensors)", file=sys.stderr)


def phase_reference_read(vfu_addr, ref_path):
    """Sub-phase (own process): read back via the read-only CPU loader into HOST
    tensors and write the reference digest file."""
    import torch

    from rados_nkv_weights.nvmekv_client import NvmeKvClient
    from rados_nkv_weights.vllm_loader import iter_named_tensors

    kv = NvmeKvClient.open_loader(vfu_addr, nsid=0)
    out = {"precision": PRECISION, "tensors": {}}
    try:
        for name, tensor in iter_named_tensors(kv, MODEL_REV, PRECISION):
            assert not tensor.is_cuda, f"CPU loader tensor {name} unexpectedly cuda"
            # Raw bytes of the host tensor (contiguous), dtype/shape recorded.
            b = tensor.contiguous().view(torch.uint8).numpy().tobytes()
            out["tensors"][name] = {
                "dtype": str(tensor.dtype),
                "shape": list(tensor.shape),
                "nbytes": len(b),
                "sha256": hashlib.sha256(b).hexdigest(),
            }
    finally:
        kv.close()
    with open(ref_path, "w") as fh:
        json.dump(out, fh)
    print(f"[reference-read] wrote {len(out['tensors'])} host-tensor digests",
          file=sys.stderr)


def phase_gpu_load(vfu_addr, ref_path):
    """Phase 2 (own process): load via the GPU-direct datapath, assert each
    tensor is_cuda, and compare byte-exact vs the CPU reference digests."""
    import torch

    from rados_nkv_weights._kvshim_gpu import KvgHandle
    from rados_nkv_weights.loader import _load_manifest
    from rados_nkv_weights.vllm_loader import (
        iter_named_tensors_gpu,
        _KvgManifestClient,
    )

    with open(ref_path) as fh:
        ref = json.load(fh)

    # The GPU path needs the manifest (small, host) AND the GPU retrieve transport.
    # Both must come over the SAME live target, but only ONE SPDK env per process.
    # The KvgHandle owns the env here; it serves BOTH the manifest Retrieve and the
    # bulk GPU Retrieves. We expose a thin KvClient adapter over KvgHandle that
    # retrieves into a GPU buffer then copies the (small) manifest bytes to host.
    kvg = KvgHandle.open(vfu_addr)
    try:
        # Productized adapter (rados_nkv_weights.vllm_loader._KvgManifestClient):
        # serve the small manifest read through the SAME KvgHandle so the whole
        # flow needs only one SPDK env (one per process).
        kv = _KvgManifestClient(kvg)
        manifest = _load_manifest(kv, MODEL_REV)

        keep = []
        n_checked = 0
        mismatches = []
        for name, tensor in iter_named_tensors_gpu(
            kvg, kv, MODEL_REV, PRECISION, manifest=manifest, keep_buffers=keep
        ):
            # ACCEPTANCE 1: device-resident.
            if not tensor.is_cuda:
                mismatches.append(f"{name}: NOT device-resident (.is_cuda=False)")
                continue
            r = ref["tensors"].get(name)
            if r is None:
                mismatches.append(f"{name}: present on GPU path, absent from reference")
                continue
            # dtype / shape parity.
            if str(tensor.dtype) != r["dtype"]:
                mismatches.append(
                    f"{name}: dtype {tensor.dtype} != ref {r['dtype']}")
            if list(tensor.shape) != r["shape"]:
                mismatches.append(
                    f"{name}: shape {list(tensor.shape)} != ref {r['shape']}")
            # ACCEPTANCE 2: byte-exact. Copy the DEVICE tensor's bytes back to
            # host (only for verification) and sha256-compare to the CPU ref.
            b = tensor.contiguous().view(torch.uint8).cpu().numpy().tobytes()
            got = hashlib.sha256(b).hexdigest()
            if len(b) != r["nbytes"]:
                mismatches.append(
                    f"{name}: nbytes {len(b)} != ref {r['nbytes']}")
            elif got != r["sha256"]:
                mismatches.append(f"{name}: sha256 mismatch (GPU vs CPU)")
            else:
                print(f"[phase2] OK {name:<28} is_cuda=True  dtype={tensor.dtype} "
                      f"shape={tuple(tensor.shape)}  {len(b)} bytes  BYTE-EXACT",
                      file=sys.stderr)
                n_checked += 1
        for gbuf in keep:
            gbuf.free()
    finally:
        kvg.close()

    print(file=sys.stderr)
    if mismatches:
        for m in mismatches:
            print(f"[phase2] MISMATCH {m}", file=sys.stderr)
        print(f"[phase2] FAIL: {len(mismatches)} mismatch(es), "
              f"{n_checked} byte-exact device tensors", file=sys.stderr)
        raise SystemExit(2)
    if n_checked != len(ref["tensors"]):
        print(f"[phase2] FAIL: checked {n_checked} but reference has "
              f"{len(ref['tensors'])} tensors", file=sys.stderr)
        raise SystemExit(2)
    print(f"[phase2] PASS: all {n_checked} tensors device-resident (.is_cuda) "
          f"AND byte-exact vs the CPU loader (live mem nvmf_tgt, GPU-direct "
          f"dma-buf Retrieve).", file=sys.stderr)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--vfu-addr", required=True,
                    help="VFIOUSER controller socket dir (contains 'cntrl')")
    ap.add_argument("--phase", default="all",
                    choices=["all", "publish", "reference-read", "gpu-load"])
    ap.add_argument("--ref", default=None, help="reference digest JSON path")
    args = ap.parse_args(argv)

    ref_path = args.ref or os.path.join(tempfile.gettempdir(), "rung2_ref.json")

    if args.phase == "reference-read":
        phase_reference_read(args.vfu_addr, ref_path)
        return 0
    if args.phase == "publish":
        phase_publish_and_reference(args.vfu_addr, ref_path)
        return 0
    if args.phase == "gpu-load":
        phase_gpu_load(args.vfu_addr, ref_path)
        return 0

    # "all": run phase 1 (publish + CPU reference) then phase 2 (GPU load) as
    # separate child processes so each gets its own SPDK env.
    print("=== Rung-2 check: phase 1 (publish + CPU reference) ===", file=sys.stderr)
    p1 = subprocess.run(
        [sys.executable, __file__, "--phase", "publish",
         "--vfu-addr", args.vfu_addr, "--ref", ref_path],
        env=os.environ.copy())
    if p1.returncode != 0:
        return p1.returncode
    print("=== Rung-2 check: phase 2 (GPU-direct load + compare) ===", file=sys.stderr)
    p2 = subprocess.run(
        [sys.executable, __file__, "--phase", "gpu-load",
         "--vfu-addr", args.vfu_addr, "--ref", ref_path],
        env=os.environ.copy())
    return p2.returncode


if __name__ == "__main__":
    raise SystemExit(main())
