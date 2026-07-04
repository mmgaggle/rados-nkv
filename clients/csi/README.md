# rados-nkv-csi — CSI driver for NVMe-KV-on-RADOS

Control-plane counterpart to the generic SPDK NIXL datapath plugin. Provisions
per-tenant **NVMe-KV namespaces** on a [`rados-nkv`](../../) target and (later)
presents them to pods as a local vfio-user socket or a DPU-presented device.

- **Epic:** `spdk-csi` · **Design:** [`docs/csi-driver-plan.md`](../../docs/csi-driver-plan.md)
- **Greenlight:** `spdk-k8s.1` (2026-07-02) locked: control API = **SPDK JSON-RPC**;
  tenant→(pool,ns,cap) mapping home = **ConfigMap + StorageClass params** (CRD
  migration path documented); SELinux MCS labeling = **node-plugin `chcon`**.

## Status (bead `spdk-csi.1` — controller)

Implemented and unit-tested:

- CSI **Identity** service (GetPluginInfo / GetPluginCapabilities / Probe).
- CSI **Controller** service:
  - `CreateVolume` → `bdev_kvrados_create` → `nvmf_create_subsystem` (idempotent)
    → `nvmf_subsystem_add_ns`, driving the sequence from `scripts/rados-nkv`.
    Idempotent (deterministic bdev name from the CSI volume name; EEXIST tolerated;
    NSID recovered via `nvmf_get_subsystems`).
  - `DeleteVolume` → `nvmf_subsystem_remove_ns` → `bdev_kvrados_delete`; idempotent.
  - `ValidateVolumeCapabilities`, `ControllerGetCapabilities` (CREATE_DELETE_VOLUME).
  - StorageClass params parsed: `computeContextSeed`, `isolationClass`,
    `transport`, `namespaceTenancy`, `cephxScope` (+ `executorEndpoint`,
    `subsystemNqn`, sizing). Capacity mapped to **quota** semantics, not allocation.
  - **volumeMode=Block is rejected** (Filesystem carrier only).
  - Idempotency is **check-then-act** via `nvmf_get_subsystems`, not error-string
    matching: a live target revealed base-nvmf reports "already exists" as a
    generic `-32603` whose message lacks "exist", so create/delete key off the
    subsystem listing (`HasSubsystem`/`FindNSID`).

- **Validated end-to-end against a live `nkv_tgt` + mem executor** (`TestE2E_*`,
  env-gated on `NKV_RPC_SOCK`/`NKV_EXECUTOR`): create → idempotent re-create →
  verify → delete → idempotent delete → verify-gone all pass over real SPDK
  JSON-RPC.

Not yet done (follow-ups):

- **Controller→front reachability / provisioning topology** — with a per-node
  front DaemonSet (`spdk-k8s.3`), which front the cluster-singleton controller
  provisions on (late binding / `WaitForFirstConsumer` + topology) is open.
- **Node service** (`spdk-csi.2`): NodePublish materializes the per-tenant
  vfio-user socket and `chcon`s it to the pod's MCS level.
- Per-tenant **cephx** cap scoping from the tenant-mapping ConfigMap (`spdk-k8s.4`).
- Controller Deployment + external-provisioner sidecar + RBAC (deferred until the
  topology above is settled).

## Build & test

Requires Go (this repo installs it to `~/.local/go`):

```bash
export PATH=$HOME/.local/go/bin:$PATH
cd clients/csi
go build ./...
go test ./...          # hermetic; the live e2e self-skips
go build -o rados-nkv-csi ./cmd/rados-nkv-csi
```

Live e2e (needs a running `nkv_tgt` + executor): start the mem-backed executor
(`rados-nkvx/nkvx_service --listen ofi+tcp://HOST:PORT --mem-object seed=x`) and a
front (`nkv_tgt -r SOCK --no-huge --no-pci -s 1024`), then:

```bash
NKV_RPC_SOCK=SOCK NKV_EXECUTOR=ofi+tcp://HOST:PORT \
  go test ./internal/driver -run TestE2E -v
```

## Run (controller-only)

```bash
rados-nkv-csi \
  --endpoint unix:///csi/csi.sock \
  --rpc-sock /var/run/spdk.sock \
  --executor-endpoint ofi+tcp://127.0.0.1:1234
```

## Layout

```
cmd/rados-nkv-csi/     main: flags, gRPC server bootstrap
internal/driver/       Identity + Controller services, StorageClass params
internal/spdkrpc/      SPDK JSON-RPC client + typed rados-nkv provisioning verbs
deploy/                CSIDriver, StorageClass, tenant-mapping ConfigMap examples
```
