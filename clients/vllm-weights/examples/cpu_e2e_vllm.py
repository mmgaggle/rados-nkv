# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""Live CPU end-to-end of the rados-nkv vLLM loader (bead spdk-0nt).

Runs a REAL vLLM + REAL torch on CPU (no GPU, no SPDK/Ceph): publish a tiny
model into an in-memory Weights catalog, then load it through
``--load-format=rados-nkv`` and compare greedy inference against a default-load
reference. The in-memory catalog is injected by monkeypatching
``vllm_loader._open_loader_kv`` (vLLM constructs the loader itself, so there is
no kv constructor hook on the engine path).

This validates the loader plugin against the installed vLLM API and exercises the
real chunk-stream -> torch.Tensor -> model.load_weights -> inference path. The
NVMe-KV transport leg (NvmeKvClient over the SPDK shim) is a separate escalation.

  MODEL=facebook/opt-125m DTYPE=bfloat16 python examples/cpu_e2e_vllm.py
"""
import os
import sys

# Run the engine in-process (no worker subprocess) so the in-memory catalog and
# the _open_loader_kv override below are visible to the model load. Must be set
# before vllm is imported.
os.environ.setdefault("VLLM_ENABLE_V1_MULTIPROCESSING", "0")

import torch
from safetensors.torch import load_file
from huggingface_hub import snapshot_download

from rados_nkv_weights.kvclient import InMemoryKvClient
from rados_nkv_weights.keys import canonical_revision
from rados_nkv_weights.publisher import publish
import rados_nkv_weights.vllm_loader as vl

MODEL = os.environ.get("MODEL", "HuggingFaceTB/SmolLM2-135M-Instruct")
DTYPE = os.environ.get("DTYPE", "bfloat16")
PROMPT = os.environ.get("PROMPT", "The capital of France is")
PRECISION = {"bfloat16": "bf16", "float16": "fp16", "float32": "fp32"}[DTYPE]
torch_dtype = getattr(torch, DTYPE)


def publish_model_to_catalog(kv, model_id):
    """Download the model's safetensors, cast to DTYPE, publish under MODEL@..."""
    snap = snapshot_download(model_id, allow_patterns=["*.safetensors"])
    import glob
    spec = {}
    for shard in sorted(glob.glob(os.path.join(snap, "*.safetensors"))):
        for name, t in load_file(shard).items():
            tb = t.to(torch_dtype).contiguous()
            data = tb.numpy().tobytes() if DTYPE != "bfloat16" \
                else tb.view(torch.uint16).numpy().tobytes()
            spec[name] = {PRECISION: (DTYPE, tuple(tb.shape), data)}
    stats = publish(kv, canonical_revision(model_id), spec)
    print(f"[publish] {len(spec)} tensors -> catalog ({stats})")


def run(load_format, kv=None):
    from vllm import LLM, SamplingParams
    extra = {}
    if load_format == "rados-nkv":
        # Inject the in-memory catalog: the engine builds RadosNkvModelLoader(kv=None),
        # so override the kv-acquisition hook to hand back our populated client.
        vl._open_loader_kv = lambda mc: (kv, lambda: None)
        vl.register()
    llm = LLM(model=MODEL, dtype=DTYPE, load_format=load_format,
              enforce_eager=True)
    out = llm.generate([PROMPT], SamplingParams(temperature=0.0, max_tokens=16))
    text = out[0].outputs[0].text
    ids = list(out[0].outputs[0].token_ids)
    del llm
    return text, ids


def main():
    kv = InMemoryKvClient()
    publish_model_to_catalog(kv, MODEL)

    print("\n=== reference: default load (HF safetensors) ===")
    ref_text, ref_ids = run("auto")
    print(f"[ref]  {ref_text!r}")

    print("\n=== rados-nkv: load from the in-memory catalog ===")
    got_text, got_ids = run("rados-nkv", kv=kv)
    print(f"[nkv]  {got_text!r}")

    print("\n=== compare (greedy token ids) ===")
    if got_ids == ref_ids:
        print("PASS: rados-nkv inference matches the default-load reference token-for-token")
        return 0
    print(f"FAIL: token ids differ\n  ref={ref_ids}\n  nkv={got_ids}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
