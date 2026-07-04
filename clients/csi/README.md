# rados-nkv-csi — CSI driver for NVMe-KV-on-RADOS

Control-plane counterpart to the generic SPDK NIXL datapath plugin. Provisions
per-tenant **NVMe-KV namespaces** on a [`rados-nkv`](../../) target and (later)
presents them to pods as a local vfio-user socket or a DPU-presented device.

- **Epic:** `spdk-csi` · **Design:** [`docs/csi-driver-plan.md`](../../docs/csi-driver-plan.md)
- **Greenlight:** `spdk-k8s.1` (2026-07-02) locked: control API = **SPDK JSON-RPC**;
  tenant→(pool,ns,cap) mapping home = **ConfigMap + StorageClass params** (CRD
  migration path documented); SELinux MCS labeling = **node-plugin `chcon`**.
- **Subsystem tenancy:** (2026-07-04) the NVMe **subsystem boundary = the k8s
  namespace** — one subsystem per k8s namespace (NQN derived from the PVC/pod
  namespace); the **socket is per-pod**, MCS-labeled + bind-mounted into only that
  pod. A vfio-user listener exposes its subsystem's namespaces, so this makes the
  NSID-visibility boundary match k8s's own tenant boundary.

## Status (`spdk-csi.1` controller + `spdk-csi.2` node plugin)

Implemented, unit-tested, and live-validated against a real `nkv_tgt`:

- CSI **Identity** service (GetPluginInfo / GetPluginCapabilities / Probe).
- CSI **Controller** service (`--controller-service`):
  - `CreateVolume` → `bdev_kvrados_create` → `nvmf_create_subsystem` (idempotent)
    → `nvmf_subsystem_add_ns`. The subsystem **NQN is derived from the PVC's k8s
    namespace** (`csi.storage.k8s.io/pvc/namespace`); falls back to the default NQN.
    Idempotent via **check-then-act** on `nvmf_get_subsystems` (base-nvmf reports
    "already exists" as a generic `-32603` whose message lacks "exist", so error
    strings are unreliable).
  - `DeleteVolume` → `nvmf_subsystem_remove_ns` → `bdev_kvrados_delete`; idempotent.
  - StorageClass params parsed; capacity → **quota**; **volumeMode=Block rejected**.
- CSI **Node** service (`--node-service`):
  - `NodePublishVolume`: derive a per-pod socket dir under `--muser-root`, ensure
    the VFIOUSER transport, **add a per-pod vfio-user listener** on the volume's
    (per-namespace) subsystem, `chcon` it to the pod's SELinux MCS level
    (**fail-closed** unless `--require-mcs=false`), and bind-mount it into the pod.
  - `NodeUnpublishVolume`: unmount, remove the listener, clean up (state stashed at
    publish, since unpublish gets only volume-id + target-path).

- **Live e2e** (`TestE2E*`, env-gated on `NKV_RPC_SOCK`[/`NKV_EXECUTOR`]): controller
  create/idempotent/delete and node listener add→socket→remove both pass over real
  SPDK JSON-RPC.

Not yet done (follow-ups):

- **MCS resolver**: NodePublish currently reads the pod's MCS level from a
  `seLinuxMcsLevel` volume-context key (webhook/test-injected). The k8s-API
  resolver (pod `seLinuxOptions.level` / namespace `openshift.io/sa.scc.mcs`) is a
  follow-up (needs client-go).
- **Controller→front topology** (`spdk-k8s.3`): which per-node front the
  cluster-singleton controller provisions on (late binding / `WaitForFirstConsumer`).
- Per-tenant **cephx** cap scoping (`spdk-k8s.4`); controller Deployment + RBAC.

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
cmd/rados-nkv-csi/     main: flags, gRPC server bootstrap (controller and/or node)
internal/driver/       Identity + Controller + Node services, params, host mount/chcon ops
internal/spdkrpc/      SPDK JSON-RPC client + typed controller/node provisioning verbs
deploy/                CSIDriver, StorageClass, tenant-mapping ConfigMap, node DaemonSet
```
