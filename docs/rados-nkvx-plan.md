# rados-nkvx: Sandboxed Near-Data Computation over NVMe Key-Value

**Status:** Draft for discussion — supersedes the cls-based Exec sketch.
**Builds on:** rados-nkv (NVMe-KV → RADOS), GPU-initiated IO via ROCm XIO + pci-mmio-bridge.
**Glossary:** [CONTEXT.md](../CONTEXT.md) — `rados-nkv`, `rados-nkvx`, `Exec`, `module`,
`sidecar`, cold fill, single/two-tier.

---

## 1. Summary

rados-nkv proved a GPU can issue NVMe-KV commands from `__device__` code and have
values land in VRAM, sourced from RADOS, with no CPU in the datapath. This plan adds
**Exec**: run an allowlisted, read-only **WASM module** against a value where it
rests, and DMA the result into VRAM.

The original sketch ran Exec as a Ceph object-class (`cls`) inside ceph-osd. We are
not doing that. Exec runs in **rados-nkvx**, a sandboxed executor that **owns its own
content-addressed NVMe namespace** and cold-fills from RADOS via plain librados. The
strategic core:

1. **Own the namespace, don't borrow the OSD's.** The hot path is zero-copy
   NVMe→WASM-linear-memory DMA out of a raw bdev the executor controls. RADOS is the
   cold/durable/distributing tier. The OSD is never modified and never in the hot path.
2. **Push compute to the data, not data to compute** — but as a *relocation of a
   frozen ABI*, not a new trust domain. The Exec opcode is identical whether the
   module runs co-located with the front (single-tier) or CRUSH-routed to the storage
   host (two-tier).
3. **Two flagships, opposite runtime needs.** GNN neighborhood sampling proves the
   *pushdown* claim (network bytes, host cores); LLM KV-cache transforms prove the
   *memoization* claim (cross-tenant result-cache hits). They are sequenced so the
   cheaper-to-demo one comes first.

---

## 2. Why this shape (the decisions already made)

The interface to the OSD is **unprivileged librados cold-fill, and nothing more.**
This is settled; the rejected alternatives are:

- **cls-in-OSD** — synchronous under the PG op-shard lock; a multi-ms sample
  head-of-line-blocks the shard, and a runtime bug crashes a durability process
  (tenant-triggered DoS). The OSD is not restartable the way an executor is.
- **Messenger MITM / `SO_REUSEPORT` sniffing** — require holding the OSD's cephx
  identity and reimplementing msgr2; REUSEPORT routes whole connections by hash, not
  ops by type, so it can't even demux cls. And the only caller of the cls is our own
  kvdev — nothing to intercept once we stop calling it.
- **Reaching into BlueStore's cache/extents** — pointer graphs, process-local
  refcounts, compressed/checksummed bytes, a vfio-claimed device. More OSD coupling
  than cls, for a copy we only pay cold.

There is **no shared-memory transport** in mainline Ceph: even a host-local librados
read crosses the loopback messenger and copies into a bufferlist. We accept that on
the cold path because immutable, content-addressed partitions are filled **once per
partition per host** and reused for an entire training run.

---

## 3. Architecture

### 3.1 Datapath (two-tier target)

```
GPU __device__ kernel
  │  NVMe-KV Exec SQE: op-ID (→ module), key, request payload (Arrow IPC)
  ▼
rados-nkv (front)
  ├── op-ID → module-hash + limits   (from the sidecar; deny by default)
  ├── CRUSH: key → object → PG → primary OSD → storage-host IP   (OSDMap, client-side)
  └── forward NVMe-KV Exec → that host's rados-nkvx (well-known NVMe-oF port)
        ▼
      rados-nkvx (executor, on the storage host)
        ├── object resident in WASM linear memory?  → reuse warm instance
        ├── else in OWN raw-bdev content-addressed cache?  → zero-copy DMA → linear memory
        ├── else cold fill: librados read (host-local) → write to own cache
        └── wasmtime: invoke module(object, request) OFF-REACTOR → Arrow result
        ▲
      result (small dense Arrow) ◀───────────────────────────────────────┘
  ▼
rados-nkv: result buffer → PRP/SGL → P2P-DMA into VRAM
```

**Single-tier (slice #1)** collapses front and executor into one process: no CRUSH
routing, no inter-tier hop, executor reads via librados and caches locally. Same
opcode, same module ABI — the only difference is where the bytes execute.

### 3.2 The executor (rados-nkvx)

- **Owns a raw-bdev content-addressed store.** Keys are content-ish (versioned,
  immutable) → LBA ranges. *We* control the layout, so the bytes are raw — no
  compression/checksum/encryption in the way of a direct read. This is the doc's
  original "purpose-built KV cache over a raw bdev," now load-bearing for two reasons:
  it's the cache *and* the only place zero-copy device→linear-memory DMA is legal.
- **Zero-copy into linear memory.** wasmtime linear memory is backed by our own
  buffers via a custom `MemoryCreator`, pinned/IOMMU-mapped as the DMA target, so a
  cache hit DMAs NVMe→linear-memory with no gather/copy. The cold-fill copy
  (librados → bufferlist → our cache) is the only copy, and it's cold.
- **Off-reactor execution.** A k-hop sample is 100s of µs–ms of compute. It MUST NOT
  run on the SPDK reactor thread or it head-of-line-blocks co-resident Retrieve
  (KV-cache) traffic. Exec dispatches to a worker pool with async completion
  hand-back. This impedance mismatch is the hardest part of slice #1 — harder than
  the sandbox.
- **Module runtime.** No WASI imports; fuel + epoch + linear-memory caps; pooling
  allocator + warm-instance cache keyed by (module, object). Module identity = sha256; authorization is by the
  sha256 in the binding, not by key. Per-invocation caps (fuel/epoch/memory)
  ride the binding; the per-namespace aggregate budget (fairness) is deferred. Modules
  live in any namespace, cold-filled by `namespace:key`, hash-verified, compiled
  (Cranelift) and cached on first use.

### 3.3 The front (rados-nkv) and control plane (sidecar)

- The front is a **scoped cephx client**: it subscribes to the OSDMap to compute
  placement, but issues no object IO itself in two-tier. Routing correctness is robust
  and locality is best-effort — a stale map sends compute to the wrong host, where the
  host-local read simply goes off-host until the map refreshes (librados is the source
  of truth; locality self-heals).
- The **sidecar** is unchanged from its current role: the privileged gRPC service that
  maps `(subsystem, namespace, op-ID) → module hash + limits`, deny-by-default.

### 3.4 On-object format

Unchanged from the original: Arrow IPC per partition (manifest, global→local ID map,
CSR adjacency as offsets+neighbors, features as fixed-stride columns, cut-edge table).
A minimal `no_std` reader inside the module; full pyarrow/cuDF outside. Partitions in
a **replicated** pool (erasure coding forfeits host-local data — the primary host holds
only one shard).

---

## 4. The two flagships (and why order matters)

| | GNN neighborhood sampling | LLM KV-cache transform |
|---|---|---|
| Proves | **pushdown** (net bytes, host cores) | **memoization** (result-cache hits) |
| Determinism | none — per-epoch seed varies → result cache ~useless | deterministic — no seed → high cross-tenant hit rate |
| Object size | large (memory64), pointer-chasing | small/hot, fits 32-bit memory |
| Result cache | off | on (read-only-compatible memoization) |
| Demo venue | real multi-box cluster (Phase 5) | **loopback box, early** |

The pushdown win is structurally **unmeasurable on loopback** — it needs a real
storage/GPU split. The memoization win is demonstrable on the loopback box in slice #1
(cross-engine result-cache hits over a shared system-prompt object). So **KV-transform
is the earlier, cheaper-to-prove flagship; GNN sampling is the Phase-5 finale.**

---

## 5. Goals / non-goals

### Goals
1. NVMe-KV **Exec** end-to-end: GPU → front → executor → wasmtime → result in VRAM.
2. Read-only WASM runtime in rados-nkvx: no imports, fuel/epoch/memory bounded,
   off-reactor, pooling allocator + warm-instance cache.
3. Executor-owned content-addressed raw-bdev store with zero-copy DMA into linear
   memory; RADOS as cold tier via librados.
4. CRUSH-routing front (two-tier) over a scoped OSDMap subscription; single-tier as the
   degenerate co-located case.
5. Frozen Exec ABI such that single-tier↔two-tier is invisible to tenants.
6. Reproducible benchmark with the pushdown and caching variables **separated** (§6).

### Non-goals (v1)
- Writes from modules (read-only is load-bearing).
- In-OSD execution / any ceph-osd modification.
- General WASI.
- Messenger-level interception / a transparent proxy for arbitrary RADOS clients.
- Replacing cls for genuine third-party cls callers (keep a thin real cls for them if
  they ever exist; orthogonal to this datapath).

---

## 6. Phases

> Decomposition note: we **plan** the two-tier target but **build** single-tier first.
> When this is broken into tracer bullets (`/to-issues`), slice #1 is single-tier; the
> CRUSH-routing fabric is a later slice. The ABI freeze (Phase 0) is what makes that
> reordering free.

- **Phase 0 — Contracts.** Freeze the Exec SQE layout (op-ID + key + request-payload
  addressing in the 64-byte command + DPTR), the **runtime-agnostic** artifact ABI
  (`op-ID → (runtime, artifact-hash, caps)`; WASM is the only runtime in v1, but the
  ABI hosts alternates like a Velox plan without re-freezing), the sidecar
  schema, and the partition schema. Exit: no open ABI question that would force a
  tenant-visible change later.
- **Phase 1 — Executor, single-tier.** wasmtime in rados-nkvx, **off-reactor**;
  Exec decoded/dispatched; allowlist enforced; fuel/epoch/memory limits; warm-instance
  cache; raw-bdev content-addressed store with zero-copy DMA. CPU-driven first. Exit:
  CPU-issued Exec runs a trivial module against a RADOS-backed object, limits
  demonstrably enforced, second invocation served zero-copy from the local store.
- **Phase 1b — KV-transform demo (loopback).** A deterministic transform module over a
  shared object; demonstrate cross-consumer **result-cache** hits. Exit: a measured
  hit rate that no single-engine app cache would capture.
- **Phase 2 — Sampling module + partition pipeline.** Partition builder (pyarrow +
  METIS over IGBH / ogbn-papers100M), `no_std` Arrow reader, k-hop sampler. Golden
  tests vs DGL/PyG — *with the cut-edge policy fixed on both sides* (§7). Exit:
  byte-identical subgraph for the agreed cut-edge policy; frontier round-trip working.
- **Phase 3 — GPU-initiated Exec (single-tier).** Teach ROCm XIO the Exec SQE;
  wavefront batch (thread 0 rings once, N requests in flight); validated CPU-side
  first. Exit: GPU rings → sampled subgraph in VRAM, loopback.
- **Phase 4 — Two-tier relocation.** CRUSH-routing front over an OSDMap subscription;
  per-host executors; inter-tier NVMe-oF. Same ABI. Exit: front routes Exec to the
  owning host; only the dense result crosses the network.
- **Phase 5 — Benchmark + publish (real cluster).** §6 matrix; honest write-up; Ceph
  Tech Talk / SDC. Exit: a defensible, data-backed answer to "does pushdown beat a
  local cache, and by how much, in the too-big-to-cache regime?"
- **Phase 6 — Read/write pushdown + ephemeral TTL tier (deferred).** Route
  Retrieve/Store to the owning-host executor; TTL writes become the ephemeral
  cache-resident tier, no-TTL writes durable RADOS write-through; the
  two-pool eviction policy is resolved here (it rides with this work, not v1).
  Deliberately sequenced *after* the computational runtime is solid — Phases 0–5 stay
  focused on Exec. This is the second major track, not part of v1.

### Benchmark matrix (separate the two variables)

Pushdown and caching are independent; benchmark a **2×2**, not a 4-point line. The
load-bearing cell is host-side-sampling-over-a-local-cache — if it's competitive, the
pushdown claim is mostly a caching claim.

|              | always-RADOS         | local cache                    |
|--------------|----------------------|--------------------------------|
| host sample  | B2 (raw fetch)       | **B2′ (the cell that threatens us)** |
| pushdown     | B3 (cold executor)   | B4 (warm executor)             |

Plus B1 (GIDS/BaM local-NVMe ceiling). Metrics: samples/s/GPU, **host+OSD cores per
GPU fed** (pushdown relocates CPU to the storage node — count it), net bytes/mini-batch,
p50/p99 Exec latency. Isolate the memoization claim to the **cross-consumer** case (it
ties or loses to a single warm engine).

---

## 7. Risks

| Risk | Read | Mitigation |
|---|---|---|
| Off-reactor dispatch destabilizes the polled fast path | Hardest part of Phase 1 | Worker pool + async completion; measure Retrieve p99 under Exec load early |
| memory64 kills the WASM perf estimate | Real | 64-bit linear memory can't use guard-page bounds-check elision; sampling is the worst case (random access). Cap partition size <4 GiB and lean on the partitioner |
| Cut-edges break "one request, one result" | First-class, not a footnote | Power-law graphs cut heavily; frontier round-trip is the correct path; the local-only shortcut biases sampling. Fix the cut-edge policy and validate golden tests *against that policy*; PG-affinity keeps more frontier on-host |
| Result cache useless for the flagship it's pitched with | Known | Seeds vary per epoch → ~0 hit for sampling; it's a *KV-transform* feature. Scope it there; frame it as read-only-compatible memoization |
| Locality is best-effort | Acceptable | Stale front map loses locality, not correctness; self-heals on map refresh |
| Cold-fill copy via loopback messenger | Amortized | Once per partition per host; hot path is local zero-copy DMA |
| SPDK upstream appetite for wasmtime | Unknown | Out-of-tree kvdev module first; Phase-0 RFC is the temperature check |

---

## 8. Open questions for the RFC

Carried forward (largely unchanged by the pivot):
1. Exec request payloads > SQE: inline-in-DPTR (shared request/response buffer) vs a
   staging key. Leaning DPTR-shared.
2. Result-size negotiation: true length in CQE DW0 + caller-provided max, mirroring
   Retrieve and its torn-read fence.
3. **Resolved:** modules in arbitrary namespaces (operator- or
   tenant-curated), bound by `namespace:key:sha256`; the sidecar ships hash + caps,
   never bytes. Authority is the hash; `namespace:key` is only the cold-fetch locator.
4. **Resolved:** per-invocation caps (on the binding) ship in slice #1 as the safety
   exit (runaway killed, OOM contained); the per-namespace aggregate budget (fairness:
   fuel-rate, concurrency, worker pools) is a fast-follow once tenants are co-hosted.
5. memory64 maturity in wasmtime for >4 GiB, or cap partition size (couples to the
   risk row above).

New from the pivot:
6. Front's OSDMap subscription: minimal cephx caps, and how aggressively to track map
   epochs vs. tolerate transient locality loss.
7. Inter-tier connection fan-out: the front reimplements Objecter-like map-watch +
   connection management at the NVMe layer — bound the complexity or reuse more of
   librados.
8. Replicated-only for partitions, or support EC with an explicit "reconstruction on
   cold fill" cost accepted.
9. Result-cache key canonicalization (Arrow IPC framing must be byte-canonical to be a
   safe hash key) — the correctness surface of the memoization feature.
10. Velox as a sibling runtime (not nested): embeddable single-threaded mode,
   zero-copy Arrow→Velox import, plan serialization (Substrait or native) as the
   artifact, footprint next to SPDK. WASM stays the first path; Velox targets the
   columnar/relational slice (feature gather, KV-transforms).

---

## 9. Success criteria

1. A GPU, from `__device__` code, requests a result by op-ID + key + request, and the
   packed Arrow result P2P-DMAs into VRAM — no host CPU in the hot path, no tenant code
   trusted natively, no Ceph daemon patched.
2. Warm executor feeds a GPU at ≥ host-CPU baseline samples/s while consuming an order
   of magnitude fewer cores (B4 vs B2), **counting OSD-node cores**.
3. Net bytes/mini-batch reduced ≥10× vs raw-fetch baseline, with the caching and
   pushdown contributions reported separately (B2′ vs B4).
4. The Exec ABI survives Phases 1–5 without a tenant-visible change — single-tier and
   two-tier are the same contract.
5. At least one productive argument on ceph-devel.
