# Deployment tiers and transports

RADOS-NKV runs in one of two shapes — **single-tier** or **two-tier** — and the
choice is driven by one hard rule about the tenant transport. This document
describes the tiers, the transports between them, where each component runs, and
how to place them relative to a GPU and the RADOS data.

## The transport rule

There are three transports in play, and they are not interchangeable:

| Transport | Spans | Carries | Used for |
|-----------|-------|---------|----------|
| **vfio-user** | one host only | NVMe-KV (incl. bidirectional Exec) | the tenant edge — GPU / host client ↔ controller |
| **NVMe-oF** (TCP / RDMA) | a fabric | unidirectional KV (Store / Retrieve / …) | a remote client driving plain KV |
| **RPC over Mercury** (RDMA in production) | a fabric | bidirectional Exec | the inter-tier front → executor hop |

The load-bearing fact is the first row: **vfio-user is local to a single host.**
It emulates PCIe over a Unix-domain socket, and its data path is the controller
**DMA-ing directly into the client's memory** through memory handles the client
passes over that socket. The GPU's doorbell is the same — fd-backed *local*
shared memory the GPU rings directly. None of this has a network form: a memory
handle cannot be handed to another machine for it to DMA into. So:

> **The controller is always co-located with whatever drives it over vfio-user.**
> Remoteness lives *below* the controller — never across the vfio-user edge.

Because a fabric command moves data in one direction only, the bidirectional Exec
command has no NVMe-oF expression; carrying Exec across hosts requires an **RPC**
(request/response is natively bidirectional), carried over **Mercury** — RDMA
(`ofi+verbs`) in production, with TCP (`ofi+tcp`) and shared-memory (`na+sm`)
transports for bring-up and loopback (see
[Why Exec is bidirectional](#why-exec-is-bidirectional)). This is why two-tier
exists.

## Why Exec is bidirectional

The standard KV commands move data one way: Store pushes a value (host →
controller), Retrieve pulls one (controller → host). Exec is different — it is a
**function call**, and a call has both an argument list (in) and a return value
(out). It is the only command that consumes input *and* produces output in a
single operation.

NVMe encodes the data-transfer direction in the opcode's low two bits, so the
opcode itself reflects this:

| Command | Opcode | Low bits | Direction |
|---------|--------|----------|-----------|
| Store    | `0x01` | `01` | host → controller (write) |
| Retrieve | `0x02` | `10` | controller → host (read) |
| Exec     | `0x83` | `11` | bidirectional |

For Exec:

- **In (host → controller):** the request payload — `[u16 key_len][key][input
  args]`. The key selects the data object; the input bytes are the call's
  parameters.
- **Out (controller → host):** the computed result bytes.

It is carried as a **single bidirectional buffer**: the host fills it with the
request, submits, and reads the result back from the same buffer after
completion. Over vfio-user — PCIe-style direct DMA into a mapped buffer — the
controller reads the request out of that buffer and writes the result back into
it: one DMA region, one pointer, no second buffer.

This is the property [the transport rule](#the-transport-rule) turns on. A fabric
command moves data in exactly one direction, so a bidirectional command has no
NVMe-oF expression — vfio-user can carry it because it is local direct DMA into a
shared buffer; a network cannot. When Exec must cross hosts it stops being an NVMe
command and becomes an **RPC** — request/response is natively bidirectional —
carried over **Mercury**. That is why the inter-tier hop is an RPC, not NVMe-oF.
The Mercury transport is pluggable: RDMA (`ofi+verbs`) is the production wire — it
is what enables the dense result to be RDMA-written straight into GPU memory — while
TCP (`ofi+tcp`) and shared memory (`na+sm`) are available for bring-up, loopback,
and testing.

## Single-tier

The controller and the executor are one SPDK process: the NVMe-KV vfio-user
controller with the WebAssembly executor compiled in as an in-process device
module. There is **no inter-tier hop** — an Exec is a direct in-process call.

- **Tenant edge:** vfio-user (local). A GPU or host client on the same host
  drives it; a GPU rings the controller's doorbell directly and values / Exec
  results land in its memory by direct DMA.
- **Data:** the in-process executor reads from RADOS via `librados`.
- **SPDK:** present (it is the single process).

Single-tier is the whole stack on one node. It is the right shape whenever the
tenant (e.g. a GPU) and the controller are on the same host.

## Two-tier

The tenant edge and the computation are split across hosts:

- **Front — `rados-nkv` (tier 1):** the SPDK NVMe-KV vfio-user controller. It is
  the tenant edge, local to the client/GPU. It terminates the tenant's NVMe-KV
  Exec and re-dispatches it inward.
- **Executor — `rados-nkvx` (tier 2):** a standalone service on each OSD host,
  co-placed with the OSDs. It runs the WebAssembly module over a content-cache
  cold-filled from local RADOS. It is **not** an SPDK process — it links
  `librados` (cold-fill), the WebAssembly runtime, and the RPC layer; it is never
  linked into `ceph-osd`.
- **Inter-tier link:** an **RPC over Mercury** (RDMA in production). Only the dense
  result returns over the fabric, RDMA-written into the tenant's memory (GPU VRAM
  via a dma-buf-registered region); the executor's cold-fill reads stay on the OSD
  host. **Today** the front forwards every Exec to a single configured executor
  endpoint — there is no per-object or topology-aware routing yet. *(Planned: see
  [Data locality](#data-locality) for the distribution design.)*
- **SPDK:** present on the **front only**. The executor tier is SPDK-free.

Two-tier is the shape for "compute next to the data" when the tenant is not on
the storage host.

## Where SPDK runs

SPDK is the vfio-user controller. It runs **wherever the controller runs**:

- single-tier → the one host;
- two-tier → the **front** only; the executor tier never runs SPDK.

## Placing it relative to a GPU

The vfio-user controller must be local to the GPU. What changes between
deployments is where the *data and computation* sit below it:

| GPU is… | Shape | How it works |
|---------|-------|--------------|
| on the OSD host | **single-tier on the OSD host** | GPU rings the local controller; Exec runs in-process; data is local for keys this host owns. No fabric, no inter-tier hop. The most efficient near-data config. |
| remote from the data, simple | **single-tier on the GPU host** | The whole stack runs on the GPU host; vfio-user is local; `librados` reaches the remote RADOS cluster over the network. GPU-initiated works, but the executor's cold-fill reads cross the network — no compute-next-to-data. |
| remote from the data, near-data | **two-tier** | vfio-user front local to the GPU; executor on the OSD host; the front forwards Exec to the executor as an RPC over Mercury (RDMA in production); results RDMA into VRAM. The executor's inputs stay local to its OSD host, and only the dense result crosses the fabric. |

What you cannot do is reach a *remote* controller with vfio-user: a GPU host
cannot vfio-user to an OSD host across the network. If the storage is remote,
either bring the controller to the GPU (single-tier on the GPU host, `librados`
to the cluster) or split the tiers (two-tier, an RPC over Mercury — RDMA in
production — to a remote executor).

## Data locality

Co-location alone does not make data local. Each object lives on its CRUSH
**primary** OSD, and `librados` routes there regardless of where the reader runs.
So single-tier on an OSD host gets local reads **only for keys that host owns**;
for any other key the read still goes to the owning OSD over the network.

Two-tier does not yet change this. **Today** the front forwards every Exec to a
single configured executor endpoint; there is no per-object routing, so the
executor's cold-fill enjoys local reads only for the keys its own host owns and
otherwise reads across the network — exactly as single-tier does. What two-tier
buys *now* is keeping the large immutable inputs on the executor tier (off the
GPU-side fabric) and returning only the dense result.

**Planned (not implemented).** Locality across the executor set is a goal of a
distribution design, not a property of the current single-endpoint code. The
intended model:

- nkvx executor instances **register with the Ceph mon/mgr** for liveness,
  modeled on how the `ceph-nvmeof` gateways register. The front discovers the live
  executor set from the mon/mgr.
- The front **distributes Exec across the live set by a hash** (consistent hash).
  Whether placement should follow CRUSH or a simpler scheme such as Maglev is an
  open decision.
- **Route-to-primary is deliberately *not* the model.** Under erasure coding an
  object has no single OSD holding the whole value locally — its shards are
  scattered across many OSDs — so "route to the owner" yields no real locality for
  EC pools. Hash-based distribution over the live executor set is preferred
  instead.

This design is tracked separately (beads issue `spdk-aee`).

## Result delivery into GPU memory

In both shapes the result reaches GPU memory without a host bounce, but by
different paths:

- **single-tier** — the local controller DMAs the value / Exec result directly
  into a GPU-resident buffer over vfio-user (PCIe-style direct DMA / P2P).
- **two-tier** — the executor RDMA-writes the result into a dma-buf-registered
  GPU region across the fabric.
