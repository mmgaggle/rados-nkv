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

## Rung 2 (wire into the vLLM loader) — design + the one blocker

The datapath primitive above is the hard part and is done. Rung 2 wraps it:

1. **C/HIP transport helper** (extend `kv_host_shim` or add a sibling):
   `kv_host_shim_dma_alloc_gpu(sh, len) -> {dev_ptr, mmap_iova}` that does the
   `hipMalloc` + `hsa_amd_portable_export_dmabuf` + `mmap` +
   `vfio_user_dev_dma_map_unmap` dance and returns both the GPU device pointer
   and the iova to program; plus a `..._retrieve_gpu(sh, key, dev_buf)` that
   Retrieves into it. The shim already owns the `vfio_device`, so it can call
   `vfio_user_dev_dma_map_unmap` directly (it links the static lib).
2. **Python binding**: expose the device pointer to Python (ctypes over a small
   `libkv_host_shim_gpu.so`, mirroring `kv_shim/libkv_host_shim.so`).
3. **Loader emit side** (`vllm_loader.iter_named_tensors`): instead of
   `torch.frombuffer(bytearray(data))` (a host tensor), Retrieve each tensor's
   chunks into a registered GPU buffer and wrap the device pointer as a
   device-resident `torch.Tensor` (`torch.as_tensor`/`from_dlpack` over the HIP
   pointer), so vLLM's `model.load_weights` performs **no H2D copy**.

### BLOCKER for a *complete* Rung-2 e2e on this box: CPU-only torch

The provisioned venv has **`torch 2.11.0+cpu`** (`torch.cuda.is_available()
== False`, `torch.version.hip == None`; same in the worktree and the primary
checkout). With CPU-only torch there is no HIP/ROCm tensor backend at all, so a
*device-resident* `torch.Tensor` — the literal Rung-2 acceptance criterion —
cannot be constructed regardless of the datapath. The datapath (Rung 1) is
proven independently of torch via raw HIP kernels.

To finish Rung 2 e2e, the loader environment needs a **ROCm torch build for
gfx1151** (e.g. `torch==2.x+rocm6.y`, gfx1151/`HSA_OVERRIDE_GFX_VERSION` as
needed). Once present, steps 1–3 above are mechanical and validate against the
CPU reference (`examples/cpu_e2e_vllm.py`, bead spdk-0nt) by greedy-output
match. Tracked as a follow-up bead.
