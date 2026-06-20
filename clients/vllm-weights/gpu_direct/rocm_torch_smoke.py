# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
"""ROCm-torch smoke test for gfx1151 (bead spdk-32o).

Proves the vllm-weights ROCm venv can build a DEVICE-resident torch.Tensor on the
Strix Halo iGPU and compute correctly — the prerequisite for Rung-2 GPU-direct
weight tensors (spdk-p9k.1.1). If the wheel lacks native gfx1151 kernels, re-run
with HSA_OVERRIDE_GFX_VERSION=11.0.0 (masquerade as gfx1100/RDNA3).

Provision (gfx1151-native; the stable download.pytorch.org/whl/rocm6.4 wheel
sees the device but ships NO gfx1151 kernels -> hipErrorInvalidDeviceFunction):

  uv venv --python 3.12 .venv-rocm && source .venv-rocm/bin/activate
  uv pip install numpy torch --index-url https://rocm.nightlies.amd.com/v2/gfx1151/
  python gpu_direct/rocm_torch_smoke.py
"""
import sys
import torch


def main():
    print(f"torch {torch.__version__} | hip {torch.version.hip} | "
          f"cuda.is_available={torch.cuda.is_available()}")
    if not torch.cuda.is_available():
        print("FAIL: no ROCm/HIP device visible to torch "
              "(try HSA_OVERRIDE_GFX_VERSION=11.0.0)")
        return 1
    dev = torch.device("cuda:0")
    print(f"device 0: {torch.cuda.get_device_name(0)}")

    # Device-resident tensor + a real op, checked against the CPU result.
    torch.manual_seed(0)
    a = torch.randn(512, 512, dtype=torch.float32)
    b = torch.randn(512, 512, dtype=torch.float32)
    ref = a @ b
    got = (a.to(dev) @ b.to(dev)).cpu()
    max_err = (got - ref).abs().max().item()
    print(f"matmul max abs err vs CPU: {max_err:.3e}")

    # bf16 too (the dtype the weights path uses).
    abf = a.to(dev).bfloat16()
    _ = (abf @ abf).float().cpu()
    print("bf16 matmul on device: OK")

    if max_err < 1e-2:
        print("PASS: ROCm torch builds device tensors and computes correctly on gfx1151")
        return 0
    print("FAIL: device matmul disagrees with CPU")
    return 1


if __name__ == "__main__":
    sys.exit(main())
