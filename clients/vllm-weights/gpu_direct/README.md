<!-- SPDX-License-Identifier: BSD-3-Clause
     Copyright (C) 2026 IBM Corporation. All rights reserved. -->

# Host-side GPU-direct NVMe-KV weights loading (bead spdk-p9k.1)

Prototype proving that an NVMe-KV **Retrieve lands byte-exact directly in GPU
memory**, host-side, with no host bounce and no QEMU guest, on the gfx1151 UMA
iGPU. This is the GPU-direct datapath under the `rados-nkv-weights` vLLM loader.

## What is proven (Rung 1 — DONE, byte-exact 4 KiB → 64 MiB)

`gpu_direct_dmamap.hip` allocates two pure `hipMalloc` GPU buffers, registers
each as a **vfio-user DMA region backed by its dma-buf fd**, then:

1. a GPU kernel fills `src` with a pattern (data born in GPU memory);
2. a host-side **KV Store** DMAs *from* the `src` GPU buffer;
3. a host-side **KV Retrieve** DMAs *into* a different `dst` GPU buffer;
4. a GPU kernel reads `dst` *at the device pointer* and verifies byte-exact.

No `spdk_dma_zmalloc` staging, no `hipMemcpy` on the datapath: the weight bytes
land in the exact GPU pages the kernel (and, in Rung 2, torch) reads.

### The mechanism

The host vfio-user client (`clients/nvme-kv/kv/vfu_host/nkv_vfu.h`, ADR-0016)
maps DMA regions out of SPDK env memory and uses `iova == vaddr` (IOVA-as-VA).
SPDK's *implicit* region registration (`vfio_user_pci.c:vfio_mr_map_notify` →
`memory.c:spdk_mem_get_fd_and_offset` → `rte_mem_virt2memseg`) only works for
**DPDK memsegs**, so a `hipMalloc`/dma-buf buffer is rejected (`-ENOENT`).

We bypass that by registering the dma-buf **directly**:

```c
hipMalloc(&dptr, size);
hsa_amd_portable_export_dmabuf(dptr, size, &dmabuf_fd, &dmabuf_off);
void *map = mmap(NULL, size, PROT_RW, MAP_SHARED, dmabuf_fd, dmabuf_off);
struct vfio_memory_region mr = {
    .iova = (uint64_t)map, .vaddr = (uint64_t)map,   /* IOVA-as-VA */
    .size = size, .offset = dmabuf_off, .fd = dmabuf_fd,
};
vfio_user_dev_dma_map_unmap(d->dev, &mr, true);       /* server mmaps SAME fd */
/* program mr.iova into the NVMe SGL/PRP; DMA lands in the GPU pages. */
```

`vfio_user_dev_dma_map_unmap` and `struct vfio_memory_region` live in
`spdk/lib/vfio_user/host/vfio_user_internal.h`; the symbol is a defined `T` in
`libspdk_vfio_user.a` (only hidden by the shared-object version script), so it
links from the static archive — **no SPDK source change**.

### spdk-avu open-Q1 resolved (for the iGPU)

On gfx1151 UMA the dma-buf is **CPU-mmap'able and fully coherent in both
directions** (GPU write ↔ CPU mmap read; CPU mmap write → GPU read; see
`gpu_direct_probe.hip` Strategy B). So the *simple vaddr path* works — SPDK does
**not** need fd-only (non-CPU-mappable) DMA regions here. A discrete GPU's VRAM
BAR dma-buf may not be CPU-mappable; that remains the open question for dGPU.

### Why a single dma-buf avoids the 2 MiB cliff

A `hipMalloc` dma-buf is **one fd with contiguous offsets**, so it maps as one
contiguous region; the region-bounded SGL (`nvfu_sgl_set_dptr`, 2 MiB chunks)
scatters across it to 64 MiB. A `spdk_dma_zmalloc` buffer > 2 MiB instead spans
multiple DPDK hugepages with *distinct* fds (the memory-note 2 MiB-region rule),
which is why Strategy A (hipHostRegister of host DMA memory) caps at 1 MiB.

## Build + run

```bash
# 1. mem-backed nvmf_tgt KV ns, max value 64 MiB (no Ceph):
sudo rm -f /var/tmp/spdk_cpu_lock_*
RUN=/tmp/p9k; mkdir -p $RUN/muser/kv
sudo bash -c "ulimit -l unlimited; setpriv --reuid $USER --regid $(id -gn) --init-groups \
  env LD_LIBRARY_PATH=/usr/local/lib spdk/build/bin/nvmf_tgt -r $RUN/rpc.sock -m 0x3" &
RPC="spdk/scripts/rpc.py -s $RUN/rpc.sock"
$RPC nvmf_create_transport -t VFIOUSER
$RPC kvdev_mem_create KvMem0 --max-value-len 67108864
$RPC nvmf_create_subsystem nqn.2026-06.io.ceph-gpu:kv -s SPDKKVR01 -a
$RPC nvmf_subsystem_add_kv_ns nqn.2026-06.io.ceph-gpu:kv KvMem0
$RPC nvmf_subsystem_add_listener nqn.2026-06.io.ceph-gpu:kv -t VFIOUSER -a $RUN/muser/kv -s 0

# 2. build + run the proof (reuses the primary checkout's prebuilt SPDK):
bash clients/vllm-weights/gpu_direct/build.sh
sudo bash -c "ulimit -l unlimited; setpriv --reuid $USER --regid $(id -gn) --init-groups \
  env LD_LIBRARY_PATH=/usr/local/lib:spdk/build/libvfio-user/usr/local/lib \
  clients/vllm-weights/gpu_direct/gpu_direct_dmamap $RUN/muser/kv 67108864"
# => byte-exact 4 KiB .. 64 MiB into hipMalloc GPU memory.
```

Note: the mem kvdev default `max_value_len` is 1 MiB
(`kvdev_mem.c:KVDEV_MEM_DEFAULT_MAX_VALUE_LEN`); over that, Store returns NVMe
`sc=0x85 SPDK_NVME_SC_INVALID_VALUE_SIZE` (rc=133). Pass `--max-value-len`.
That is a KV-value-size cap, **not** a DMA/region/GPU limit.

## Rung 2 (wire into the vLLM loader) — DONE (device-resident, byte-exact)

The datapath primitive above is the hard part. Rung 2 (bead spdk-p9k.1.1) wraps
it so the loader emits **device-resident** tensors via the dma-buf Retrieve:

1. **C/HIP transport shim** — `kvg_gpu.hip` → `libkvg_gpu.so` (built by
   `build_kvg.sh`). Built on the raw vfio-user host client (`nkv_vfu.h`), which
   exposes the underlying `struct vfio_device *` that
   `vfio_user_dev_dma_map_unmap` needs. C ABI: `kvg_open(vfu_addr)`,
   `kvg_buf_alloc(dev, size)` (the `hipMalloc` + `hsa_amd_portable_export_dmabuf`
   + `mmap` + `vfio_user_dev_dma_map_unmap` dance), `kvg_buf_dptr(buf)` (the GPU
   device pointer), `kvg_retrieve_gpu(dev, nsid, key, key_len, buf, &got_len)`
   (Retrieve straight into the GPU region; binary keys, region-bounded SGL).
   *(The SPDK-NVMe `kv_host_shim` is NOT used for this leg — it hides the
   `vfio_device` behind the NVMe driver. We use the raw client instead.)*
2. **Python binding** — `src/rados_nkv_weights/_kvshim_gpu.py` (ctypes over
   `libkvg_gpu.so`, mirroring `_kvshim.py`): `KvgHandle` + `GpuBuf`, exposing the
   HIP device pointer (`GpuBuf.dptr`) to Python.
3. **Loader emit side** — `vllm_loader.iter_named_tensors_gpu` Retrieves each
   Value directly into a GPU dma-buf region and wraps the device pointer as a
   device-resident `torch.Tensor` via `__cuda_array_interface__` (uint8 wrap +
   `.view(dtype)` for bf16/fp8, which CAI's typestr can't name). The unpacked
   case (one key, offset 0) wraps the whole GPU region with **zero device
   copies**; the packed/multi-slice case concatenates device byte-slices. The
   `RadosNkvModelLoader` selects this path when
   `model_loader_extra_config['rados_nkv_gpu_direct']` is set, so
   `model.load_weights` does **no implicit H2D copy**.

### Acceptance — PROVEN on this box (gfx1151)

`rung2_check.py` publishes a synthetic model into a **live mem-backed nvmf_tgt**
(no Ceph), reads it back via the CPU loader (host tensors, SPDK-NVMe shim) as the
reference, then loads it via the GPU-direct path and asserts every tensor is
**device-resident (`.is_cuda`)** and **byte-exact** vs the reference. Both the
unpacked direct-wrap path (`pack=False`) and the packed concat path (`pack=True`)
pass for a bf16/fp16/fp32 mix. The two SPDK host clients each init the SPDK env
(DPDK: one env per process), so the check runs publish + GPU-load as **separate
processes**.

```bash
# bring up the mem nvmf_tgt + KV ns as in the Rung-1 repro above, then:
bash gpu_direct/build_kvg.sh                 # libkvg_gpu.so
SPDK_ROOT=<spdk> bash native/build.sh        # libradosnkv_kvshim.so (CPU ref)
PYTHONPATH=clients/vllm-weights/src \
  <venv-rocm>/bin/python gpu_direct/rung2_check.py --vfu-addr $RUN/muser/kv
# => [phase2] PASS: all N tensors device-resident (.is_cuda) AND byte-exact.
```

The README's earlier blocker (CPU-only torch) is **resolved**: the provisioned
`.venv-rocm` (bead spdk-32o) ships `torch 2.10.0+rocm7.13` with native gfx1151
kernels (`torch.cuda.is_available() == True`, `torch.version.hip == 7.13`), so
device-resident tensors are constructible.

### Best-effort: full `vllm.LLM(...).generate()` — needs vLLM-on-gfx1151

The `.venv-rocm` has torch but **not vLLM** (no gfx1151 build installed), so a
full `vllm.LLM(load_format="rados-nkv", model_loader_extra_config=
{"rados_nkv_gpu_direct": 1}).generate()` is deferred. The loader's GPU-direct
branch is already wired (`RadosNkvModelLoader._load_weights_gpu_direct`), so this
is a wiring/validation task once vLLM-on-gfx1151 is present — tracked as bead
**spdk-kuc**. The achievable Rung-2 core (device-resident byte-exact emission) is
proven directly above without vLLM.
