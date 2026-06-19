# rados-nkv / rados-nkvx — NVMe Key-Value (+Exec) over RADOS

The vocabulary for the NVMe Key-Value data plane over RADOS (`rados-nkv`) and the
sandboxed near-data computation extension (`rados-nkvx`). This glossary exists to
keep our terms distinct from Ceph's own overloaded vocabulary (object-class, target)
and from the control-plane "sidecar."

## Language

### Components

**rados-nkv**:
The tenant/GPU-facing NVMe-KV endpoint: serves Store/Retrieve/List/Delete, receives
Exec, CRUSH-routes Exec to the owning storage host (two-tier), and P2P-DMAs results
into VRAM. The existing target, evolved.
_Avoid_: gateway, front, target (every NVMe-oF endpoint is a "target")

**rados-nkvx**:
The executor: a per-OSD-host process that *implements* Exec — a sandboxed WASM
runtime over a content-addressed local-NVMe cache, cold-filled from RADOS via
librados. Restartable; never linked into ceph-osd.
_Avoid_: sidecar (that is the control plane), compute host, cls-executor

**sidecar**:
The privileged gRPC control-plane service that manages the per-namespace Exec
allowlist (op-ID → module binding). Not in the datapath, not a compute host.
_Avoid_: using "sidecar" for rados-nkvx

### Execution

**Exec**:
The NVMe-KV vendor opcode that runs an allowlisted module against a value where it
rests, **read-only** — it never mutates the stored value, so it is permitted on
read-only namespaces (still gated by the per-namespace allowlist). It is the frozen
ABI between rados-nkv (decode + route) and rados-nkvx
(execute), which is what makes the execution site relocatable without a
tenant-visible change.

**module**:
The read-only WebAssembly unit an Exec runs, identified by the sha256 of its
`.wasm` binary. No imports; fuel/epoch/memory bounded.
_Avoid_: class, cls, computational class (in Ceph, "object class / cls" is the
in-OSD C++ mechanism we deliberately do **not** use)

**binding**:
A control-plane entry `(subsystem, exec-namespace, op-ID) → (runtime, module-namespace,
module-key, artifact sha256, per-invocation caps)`. `runtime` selects the backend
(wasmtime in v1; a Velox plan is a future kind — ADR-0012). The **sha256 is the sole
authorization + integrity anchor**; `(module-namespace, module-key)` is only the
cold-fetch locator, consulted on a content-cache miss. Per-op, finer than
per-namespace; the same module may be bound with different caps in different
namespaces. Bounds a single Exec.
_Avoid_: allowlist entry (that is the whole table), op binding

**namespace budget**:
The aggregate, per-tenant caps `(subsystem, namespace) → (concurrency, fuel-rate,
resident-memory/instance quota, worker pool)`. Governs contention *across* bindings
and concurrent calls — what a single binding cannot see. This is where fairness
lives; deferred until tenants are co-hosted.
_Avoid_: per-namespace limits (ambiguous with per-invocation caps)

**cold fill**:
The once-per-partition librados read that populates an executor's local cache from
RADOS. Amortized to ~zero across a training run; the only time the OSD is in the
path. The hot path is zero-copy DMA out of the executor's own namespace.

### Write tiers

**durable write**:
A Store **without** a TTL: write-through to RADOS with full replication/durability
guarantees, originated host-local on the primary's host.
_Avoid_: persistent write

**ephemeral write**:
A Store **with** a TTL: never written to RADOS — it lives in the owning-host
executor's resident store and ages out at the TTL. No durability, replication, or
migration; lost on host failure or CRUSH remap (lifetime = `min(TTL, time-to-remap)`).
The KV-cache tier. The TTL is the storage-tier selector (ADR-0011).
_Avoid_: scratch write, soft write, cache write

### Deployment

**single-tier**:
rados-nkv with rados-nkvx linked into the same process; no inter-tier hop. The
first tracer-bullet slice. Pushdown benefit comes only from the local cache.

**two-tier**:
A rados-nkv front that CRUSH-routes Exec to per-OSD-host rados-nkvx executors, so
large immutable inputs never cross the network — only the dense result does. The
target topology; earns its complexity in the too-big-to-cache regime.

### Flagship data

**partition**:
An immutable, content-addressed graph-partition object (Arrow IPC: CSR adjacency +
feature matrix + ID map + cut-edge table). Versioned keys; never mutated, so the
cache needs no invalidation.
