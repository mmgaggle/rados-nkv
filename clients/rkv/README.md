# rados-nkv

Ergonomic Rust CLI for the NVMe-KV (`rados-nkvx`) datapath. It FFIs the proven
raw vfio-user NVMe-KV driver (`../vfu_host/nkv_vfu.h`) through a thin C shim
(`csrc/nkvx_shim.c`) for datapath ops (`store`/`get`/`exec`), and speaks
JSON-RPC to the target's control socket for namespace management
(`ns create`/`ns allowlist`). Friendly `ns/key` paths and named exec ops are
resolved from `~/.rados-nkv.conf`. A `--gpu` flag delegates the
store/get/exec subset to the GPU-initiated client (`nkv_vfu_gpu`).

```
rados-nkv store myns/k1 -i file     echo data | rados-nkv store myns/k1
rados-nkv get   myns/k1 myns/k2     rados-nkv list myns
rados-nkv exec  bytecount myns/k1   rados-nkv ns create myns2 -o pool=kvpool
rados-nkv store myns/k1 -o ttl=24h  rados-nkv ns allowlist myns2 -i policy.yaml
```

## Install / prerequisites

- **Rust** (stable; built with 1.96). Install via `rustup` and
  `source $HOME/.cargo/env`.
- **Prebuilt SPDK** static libraries (the crate links them directly; see
  *Build* below). The **default** build does **not** require ROCm/HIP — `--gpu`
  delegates to the separately built `nkv_vfu_gpu` binary as a subprocess. The
  optional `gpu-native` feature (`--features gpu-native`) instead links HIP for
  an in-process native `--gpu`; only that build needs `hipcc` + `libamdhip64`
  (see *Native HIP `--gpu`* below).
- **Datapath/control ops need DPDK hugepage privileges.** `sudo` with
  `env_reset` drops `HOME` (which the config loader needs), so run the CLI as:
  ```
  sudo -n env HOME=$HOME ./target/release/rados-nkv ...
  ```
  This applies to `store`/`get`/`exec` (vfio-user) **and** `ns ...` (JSON-RPC),
  because the JSON-RPC client still initialises the SPDK/DPDK env.

### Per-invocation EAL/attach cost (optional daemon)

`spdk_env_init` runs **once per process**, so each invocation opens one
vfio-user session, performs all its ops (e.g. `get k1 k2 k3` retrieves three
keys on one session), then closes — amortising the DPDK EAL init + controller
attach cost *within* an invocation. **Cross-process pipe chains otherwise pay
attach each time** (`rados-nkv store … | rados-nkv get …` would attach twice).

To eliminate that, start the **opt-in persistent session daemon**
(`rados-nkv daemon start`): it attaches **once** and serves store/get/exec for
all later invocations over a unix socket. With no daemon running, behaviour is
identical to the direct path. See [Session daemon](#session-daemon-rados-nkv-daemon)
below.

## Build

Worktrees do **not** carry SPDK `build/` outputs, so `build.rs` locates the
prebuilt SPDK static libraries via an env var:

```
NKVX_SPDK_BUILD=/path/to/spdk/build cargo build --release
```

Default if unset: `<umbrella>/spdk/build (the spdk submodule)`.
The SPDK root is `<build>/..` (it must contain `include/`, `dpdk/build/lib/`,
`isa-l/.libs/`, `isa-l-crypto/.libs/`, and `build/libvfio-user/...`). The link
recipe mirrors `../vfu_host/build_gpu.sh` minus HIP (`-lamdhip64`).

## Smoke test (hidden subcommand)

Against a live target (`<traddr>/cntrl` must exist):

```
./target/release/rados-nkv selftest /tmp/nkvx/muser/0 smoke-key 'hello world'
```

Opens one vfio-user session, KV Store (nsid=1), KV Retrieve, byte-compares, and
prints `OK` (exit 0) or `FAIL` (nonzero). DPDK hugepage setup may require
elevated privileges.

## Config schema (`~/.rados-nkv.conf`)

INI. Loaded from `$HOME/.rados-nkv.conf`; a missing file falls back to default
`traddr`/`rpc_sock` but invents no namespaces (an unknown `ns` is a hard error).

```ini
[target]
traddr   = /tmp/nkvx/muser/0          # vfio-user cntrl dir (datapath)
rpc_sock = /tmp/nkvx/rpc.sock         # JSON-RPC unix socket (control plane)
nqn      = nqn.2026-06.io.spdk:nkvx   # subsystem NQN for ns RPCs
gpu_bin  = /path/to/nkv_vfu_gpu       # optional; else nkv_vfu_gpu from PATH
daemon_sock = /run/user/1000/nkv.sock # optional; else $HOME/.rados-nkv.sock

[namespaces]                          # friendly name -> nvme nsid (fallback cache)
default = 1                           # used only when the target has no kv_name
myns    = 1

[exec]                                # exec op name -> op_id
bytecount = 10
identity  = 11

[opcodes]                             # raw NVMe opcode -> label; informational
83 = exec                             # only; preserved on save, no behaviour
```

* `[target]` — datapath + control endpoints. `traddr`/`rpc_sock` default to the
  values above when unset.
* `[namespaces]` — client-side `name -> nsid` **fallback cache**. As of
  spdk-jhk.7.12 the target keeps an optional server-side friendly name per KV
  namespace (`kv_name`), so name resolution prefers the server and only falls
  back to this table when the target is unreachable or predates server-side
  names. `ns create` registers the name on the target *and* writes the assigned
  nsid back here for offline/compat use. See "Server-side namespace names" below.
* `[exec]` — resolves `exec <name>` to an `op_id` for the KV-Exec command.
* `[opcodes]` — **informational only** in v1 (a passthrough label table from the
  gist). It is parsed and preserved across `ns create` config rewrites but drives
  no behaviour.

`ns create` is the only command that **writes** the config (it persists the new
`name = nsid`); the rewrite preserves all other sections and values.

## Server-side namespace names (spdk-jhk.7.12)

A KV namespace can carry an **optional server-side friendly name**. The NVMe
nsid stays the canonical identifier; the name is additive metadata stored on the
target.

* `rados-nkv ns create <name> -o pool=<POOL>` registers the friendly name on the
  target via `nvmf_kv_ns_create_by_name` (also persisted to the durable omap
  registry; see "Namespace create").
* The target reports it in `nvmf_get_subsystems` as a per-namespace `kv_name`
  field (omitted when no name was registered).
* Resolution of `name/key` for `store`/`get`/`exec` prefers the server: the CLI
  asks the target for the configured nqn's `kv_name`->nsid map and uses it
  first, **without** needing a `[namespaces]` entry. The client-side
  `[namespaces]` cache is a fallback for offline/compat (target unreachable, no
  `nqn` configured, or an older target/build with no `kv_name`).

Fully backward compatible: nsid-only flows and existing `[namespaces]`-driven
configs keep working unchanged; the server lookup is best-effort and silently
falls through to the local cache on any RPC failure.

Equivalent raw RPC (via `scripts/rpc.py`):

```
rpc.py nvmf_subsystem_add_kv_ns <nqn> <KvDev> --name myns   # register name
rpc.py nvmf_get_subsystems <nqn>   # namespaces[].kv_name shows "myns"
```

## `list` — namespaces and key enumeration

```
rados-nkv list            # configured namespaces:  name<TAB>nsid  (stdout)
rados-nkv list myns       # keys in the namespace, one per line  (stdout)
```

A bare `list` prints the `[namespaces]` table from `~/.rados-nkv.conf`, one
`name<TAB>nsid` per line, sorted. `list <ns>` resolves the namespace to its nsid
(an unknown name still errors with the `ns create` hint), then enumerates the
namespace's keys via the target's `nvmf_ns_kv_list` JSON-RPC method and prints
them to stdout — one key per line, in the backend's stable iteration order. An
empty namespace prints nothing.

Keys are 1-16 binary bytes; the target returns them hex-encoded and the CLI
prints each as UTF-8 when it is printable ASCII, otherwise as a `0x…` hex literal
so binary keys remain unambiguous. Large key sets are paged transparently (the
RPC reports a `more` flag and the CLI resumes from the last key).

Enumeration is implemented by the kvdev backend's List op. The in-memory backend
(`kvdev_mem`) enumerates its key map. The rados backend cannot enumerate by key
(objects are named by key-hash, with no stored key index), so `nvmf_ns_kv_list`
against a rados namespace returns a clear "key enumeration not supported by this
kvdev backend" error rather than fabricating results.

## Store / get object options (`-o`)

`store` and `get` accept repeatable `-o KEY[=VAL]` options:

```
rados-nkv store myns/key -o ttl=24h -o ephemeral   # store options
rados-nkv get   myns/key -o prefetch               # get option
```

* **store**: `ephemeral`, `ttl=DURATION`, `touch`, `prefetch`
* **get**:   `prefetch`

`DURATION` is `<n><unit>` with unit `s`/`m`/`h`/`d` (e.g. `30s`, `15m`, `24h`,
`7d`); the unit is mandatory, so `ttl=1` is rejected rather than silently meaning
"1 second". Options are **parsed and validated** — unknown keys, malformed
durations, and stray values on flag-style options (`-o ephemeral=1`) are hard
errors.

As of spdk-jhk.7.14 the in-memory target **honours** three of the four options,
encoding them in the KV Store command (CDW11 Store Option bits + CDW12 TTL
seconds, per `enum spdk_nvme_kv_store_option`):

* **`ttl=DURATION`** — the entry expires after the duration. Lazy expiry: once the
  deadline passes, the key is absent from `get` (reports key-does-not-exist) and
  omitted from `list`, and is reaped on the next access. `ttl=0s` expires on the
  next whole-second tick.
* **`ephemeral`** — marks the entry non-durable. The in-memory backend is wholly
  volatile, so this is a recorded, introspectable property (visible via the
  `kvdev_mem_get_entry` RPC) that excludes the entry from any durable snapshot;
  it does not change read/write behaviour on this backend.
* **`touch`** — refreshes an **existing** key's TTL and last-access time WITHOUT
  changing its value (the store value payload is ignored). With `-o ttl=` it sets
  the new TTL; without it the existing TTL is cleared (the key becomes
  non-expiring). `touch` on an absent key is an error.

`prefetch` is a **read hint with no in-memory meaning**, so it remains a
DOCUMENTED no-op on both `store` and `get`: the CLI prints
`note: ... [prefetch] is a documented no-op` to stderr rather than claiming a
semantic it lacks. Honoured options instead print
`note: applied store option(s) [...] (server-side)`.

The honoured options ride the CPU datapath's KV Store CDW fields. They are **not**
available on the `--gpu` path (which cannot set those fields) — combining them
with `--gpu` is a hard error rather than a silent drop. Because the persistent
session daemon's wire format does not carry the option bits, a `store` with a
honoured option bypasses the daemon fast-path and opens a direct session.

Backward compatible: a `store`/`get` with no `-o` option behaves exactly as
before (CDW11 Store Option byte = 0, CDW12 = 0).

## Namespace management (`ns`, control-plane)

`ns` subcommands talk JSON-RPC 2.0 to the target's control socket
(`[target] rpc_sock`, default `/tmp/nkvx/rpc.sock`), not the vfio-user datapath.
They still need the same DPDK/privilege wrapper as datapath ops; run them as:

```
sudo -n env HOME=$HOME ./target/release/rados-nkv ns ...
```

### `ns create <name> -o pool=POOL [-o cluster=NAME] [-o namespace=NS] [--nsid N]`

Calls `nvmf_kv_ns_create_by_name(nqn, name, pool, ...)` (epic spdk-jhk.8). The
**target** creates (or idempotently resolves) a rados-backed KV namespace: it
spins up a kvdev bound to `pool` and an isolated rados namespace (default ==
`<name>`, override with `-o namespace=`), adds it to the subsystem with the
friendly name as its `kv_name`, and **durably records** `name -> {nsid,
rados_namespace}` as an omap entry on a deterministic registry object
(`nkv.ns.registry`, in the reserved rados namespace `nkv.registry`) inside the
pool. It prints the nsid (stdout) and caches `name = nsid` under `[namespaces]`
for offline resolution. The durable registry — surfaced via `kv_name` — is the
source of truth, so resolution works even with no local config.

The CLI never touches rados; all registry/kvdev work is target-side via the
target's librados cluster handle. The cluster must already be registered
(`kvdev_rados_register_cluster`); `-o cluster=` selects it (default `ceph0`).

**Idempotent:** re-running `ns create <name> -o pool=POOL` returns the same nsid
with no error (the registry hit short-circuits before touching the subsystem).

The old `--kvdev NAME` form has been **removed** (it attached an existing kvdev
rather than creating one, and broke the `-o` convention).

```
rados-nkv ns create demo -o pool=kvpool       # -> prints "1", rados ns "demo"
rados-nkv ns create demo -o pool=kvpool       # -> prints "1" again (idempotent)
```

### `ns attach <name> -o pool=POOL [-o cluster=NAME]`

Calls `nvmf_kv_ns_attach_by_name(nqn, name, pool, ...)`.
Where `ns create` **defines** a new entry, `ns attach` **binds an existing one**
back to the subsystem — the restart-replay verb. The target reads the pool's
durable registry omap for `<name>` and:

* **unknown name** -> a clear error (`unknown namespace '<name>' in pool ...; run
  'ns create' first`); attach never invents a new entry;
* **already attached** -> a clean no-op success returning the recorded nsid;
* otherwise -> creates a kvdev bound to `pool` + the **recorded** rados namespace
  and adds it to the subsystem at the **recorded** nsid, so the same nsid and the
  same data come back across a target restart or on a fresh target.

Because the nsid and rados namespace are replayed from the registry, they are
**not** settable here (`-o namespace=`/`--nsid` are rejected); only `-o pool=`
(required) and `-o cluster=` (default `ceph0`) are accepted. The returned nsid is
cached under `[namespaces]`, but the target registry stays the source of truth.

```
# After a target restart (nothing attached), rebind by name:
rados-nkv ns attach demo -o pool=kvpool       # -> prints "1" (the recorded nsid)
rados-nkv get demo/k                          # -> original value, byte-exact
rados-nkv ns attach demo -o pool=kvpool       # -> prints "1" again (no-op)
rados-nkv ns attach nope -o pool=kvpool       # -> error: run 'ns create' first
```

### `ns allowlist <name> -i policy.yaml`

Parses a YAML policy into KV-Exec allowlist entries and calls
`nvmf_ns_set_kv_exec_allowlist(nqn, nsid, allowlist)` for the namespace's nsid
(resolved via `[namespaces]`). Verify with
`scripts/rpc.py -s <sock> nvmf_ns_get_kv_exec_allowlist <nqn> <nsid>`.

#### `policy.yaml` schema

The file mirrors the RPC `allowlist` array (one mapping per entry). It may be a
top-level `allowlist:` key or a bare list. Only `op_id` is mandatory; every
other field is optional and omitted from the RPC when unset, exactly as the
target's decoder treats them. Two binding encodings are accepted:

```yaml
allowlist:
  # Structured binding (preferred):
  - op_id: 10
    runtime: cls            # "cls" or "wasm"
    module_namespace: nkvx  # object-class name / module namespace
    module_key: bytecount   # class method / wasm export
    sha256: <64 hex chars>  # optional integrity pin
    caps: 0                 # optional capability tier (uint64)

  # Legacy "class:method" (deprecated; maps to runtime=cls):
  - op_id: 11
    binding: "nkvx:identity"
```

The legacy `binding` string and the structured fields are mutually exclusive
(enforced locally and server-side). A bare top-level list is also accepted:

```yaml
- op_id: 11
  binding: "nkvx:identity"
```

## `--gpu` (GPU-initiated datapath)

The global `--gpu` flag routes `store` / `get` / `exec` through the
GPU-initiated datapath instead of the in-process CPU datapath. Rather than
linking the HIP/ROCm runtime into `rados-nkv` (which would force ROCm into the
default build), `--gpu` delegates to the prebuilt `nkv_vfu_gpu` binary
(`../vfu_host/build_gpu.sh`) as a subprocess, translating the ergonomic
arguments to its positional CLI:

```
rados-nkv --gpu store myns/g1 -i file       ->  NKVX_NSID=<nsid> nkv_vfu_gpu <traddr> store         g1 <value>
rados-nkv --gpu get   myns/g1 myns/g2 ...    ->  NKVX_NSID=<nsid> nkv_vfu_gpu <traddr> retrieve-batch <keyfile>
rados-nkv --gpu exec  bytecount myns/g1      ->  NKVX_NSID=<nsid> nkv_vfu_gpu <traddr> exec          g1 10
```

### `--gpu get`: wavefront batch retrieve (one doorbell)

`--gpu get` is the showcase of the GPU path: one-or-more keys are written
one-per-line to a temp keyfile and handed to `nkv_vfu_gpu retrieve-batch`, which
builds N KV Retrieve SQEs across N wavefront lanes into N per-lane DMA buffers
and rings the SQ doorbell **once** for the whole batch (cf. the existing
`store-batch` / `exec-batch`). The values come back length-framed (4-byte LE
length + bytes per key, sentinel `0xFFFFFFFF` for not-found) and `rados-nkv`
demuxes them to stdout **in input order**, using the same convention as the CPU
multi-key `get` (raw concatenation when piped; labelled `==> ns/key (N bytes) <==`
headers at a tty with multiple keys).

Because one wavefront drives **one** controller IO queue, all keys in a single
`--gpu get` must resolve to the **same nsid**; a request spanning namespaces is
a hard error (per-nsid splitting is a future option). Each wavefront lane carries
a **region-bounded SGL DPTR** over a per-lane buffer sized to
the controller max value (`NKVX_RETR_LANE_BUF`, 64 MiB), so a value larger than
4 KiB comes back in **full** — framed at its true length, not clamped to a page —
exactly matching the CPU `get`. (spdk-jhk.11 gave each lane a fixed 4 KiB PRP,
which truncated any value >4 KiB.)

The larger per-lane buffers make chunking **memory-bounded**, not just
depth-bounded: a chunk holds the smaller of `depth-1` keys and
`NKVX_RETR_BATCH_BUDGET / NKVX_RETR_LANE_BUF` keys (256 MiB / 64 MiB = 4 keys by
default). Each chunk is still one wavefront + one doorbell; `nkv_vfu_gpu` logs the
per-batch lane buffer size, budget, chunk size and chunk count to stderr (never a
silent cap). Single-key `--gpu get` is just the N == 1 case and also returns a
>4 KiB value in full. The native-HIP feature path (`--features gpu-native`) keeps
per-key submission for v1 (each key is one GPU-rung Retrieve via the SGL transfer
path, so it too returns full values; the subprocess path is the wavefront-batch
target).

The namespace is selected by exporting `NKVX_NSID` to the `nkv_vfu_gpu`
subprocess: `rados-nkv` resolves `myns` to its nsid (server registry or the
`[namespaces]` cache) and passes it through, so `--gpu` addresses **any**
namespace, not just nsid 1. With no namespace mapping the
default is nsid 1, identical to prior behaviour.

The binary is resolved from `[target] gpu_bin` in `~/.rados-nkv.conf`, else from
`PATH`:

```ini
[target]
gpu_bin = /path/to/nkv_vfu_gpu
```

`--gpu` exposes only the **subset** `nkv_vfu_gpu` supports; unsupported
combinations error clearly (exit 1) rather than mistranslating:

- **multi-nsid `get`** — one wavefront drives one controller IO queue; all keys
  in a `--gpu get` must share one nsid (single-nsid multi-key is supported).
- **`store` values with NUL bytes / larger than 64 KiB** — the value is passed
  as a single argv token; binary or large values need the CPU datapath.
- **`exec -i` input payload** — `nkv_vfu_gpu` passes no exec input.

Non-default namespaces (nsid > 1) **are** supported via `NKVX_NSID` (above).

For `exec`, the GPU binary pretty-prints the result itself (e.g.
`... count=25` for `bytecount`, `... -> N bytes: <value>` for `identity`);
`--gpu exec` relays that output. The CPU `exec` prints the bare integer/value.
A `--gpu store`/`get` round-trip is byte-exact with the CPU path and with a
direct `nkv_vfu_gpu` invocation.

### Native HIP `--gpu` (optional `gpu-native` feature)

`--gpu` has a second, **opt-in** implementation that runs the GPU-initiated
datapath **in-process** — no subprocess — by linking the HIP runtime directly.
It is behind the **off-by-default** `gpu-native` cargo
feature so the **default build stays ROCm-free** (the design above is preserved
unchanged):

```bash
cargo build --release                       # default: NO ROCm; --gpu => subprocess
cargo build --release --features gpu-native # links HIP; --gpu => in-process native
```

When built with `--features gpu-native`, `build.rs` compiles `csrc/nkvx_gpu.hip`
with `hipcc` (reusing `build_gpu.sh`'s recipe) and links `libamdhip64`; `--gpu`
then routes through `src/gpu_native.rs`, which builds the NVMe SQE and rings the
SQ doorbell from a **device kernel** (exactly the `nkv_vfu_gpu` mechanism:
`nvfu_ring_kernel` + `__threadfence_system`), sharing the same `nkv_vfu.h`
driver as the CPU path. The native path uses the region-bounded SGL transfer, so
it is **not** limited to the subprocess's argv-token subset: it handles binary
values, large values (verified byte-exact to 256 KiB; up to the 64 MiB
`max_io_size`), and an `exec -i` input payload. It keeps the same `--gpu` CLI
surface and threads the resolved nsid straight into the SQE (so it too addresses
any namespace, not just nsid 1); `--gpu exec` prints the same
`EXEC ok: GPU exec op …` lines as the subprocess path.

Requirements for the feature build: `hipcc` and `libamdhip64` (ROCm). The build
finds `hipcc` on `PATH` (override with `NKVX_HIPCC`) and the HIP lib dir via
`hipconfig -p` (override with `NKVX_HIP_LIBDIR`); an rpath to that dir is
embedded so the runtime loads without `LD_LIBRARY_PATH`. If `hipcc` is missing,
the **feature** build fails fast with a clear message — the **default** build is
unaffected. The single `nvfu_produce` symbol the driver expects is provided by
the HIP TU in the feature build (the CPU shim compiles its copy out under
`-DNKVX_GPU_NATIVE`); the default build keeps the CPU shim's copy and never
compiles the HIP TU.

Verified (live target, `gfx1151`): native `--gpu store`/`get`/`exec` round-trips
are **byte-exact** versus the CPU path — small values, a 256 KiB random blob
(SHA-256 match), `bytecount`→length, and `identity`→value all agree.

## Session daemon (`rados-nkv daemon`)

The datapath attach (`spdk_env_init` + vfio-user controller attach) costs ~tens
of ms per process. A single CLI invocation amortises it across all its ops, but a
**cross-process pipe chain** (`store … | get …`) pays it per stage. The **opt-in
session daemon** attaches **once** and serves store/get/exec for every later
invocation over a unix socket.

```
rados-nkv daemon start     # attaches once; detaches into the background
rados-nkv daemon status    # is a daemon answering on the socket?
rados-nkv daemon stop      # release the session cleanly and exit
```

Once a daemon is running, `store` / `get` / `exec` automatically take a **fast
path**: they probe the daemon socket, and if a daemon answers, forward the op and
stream the result back — no per-invocation attach. With **no daemon running** the
probe finds nothing live and the command falls back to the existing in-process
datapath, so behaviour is **identical to not using the daemon at all** (it is
purely opt-in; `--gpu` always uses the GPU subprocess and never the daemon).

* **Socket** — `[target] daemon_sock`, default `$HOME/.rados-nkv.sock`. The
  daemon also writes `<sock>.log` (the one-time attach chatter lands here, never
  in a client's stdout) and `<sock>.pid`.
* **Concurrency** — the controller exposes a single IO queue, so the daemon
  serialises requests against its one session: connections are accepted and each
  is handled to completion before the next. Correct for the one-op-at-a-time
  hardware; not a throughput multiplexer.
* **Lifecycle** — `daemon stop` sends a `Shutdown` frame and, if needed, escalates
  to `SIGTERM` via the recorded pid, then removes the socket/pid files. The daemon
  also shuts down gracefully on `SIGTERM`/`SIGINT`, releasing the session and
  unlinking its socket so the next probe falls back.
* **Wire protocol** — a tiny length-prefixed binary framing (`src/proto.rs`):
  `u32 body_len | u8 tag | body`, with `Store/Get/Exec/Ping/Shutdown` requests and
  `Ok(payload)/Err(msg)` responses. The codec is transport-agnostic and unit
  tested (`cargo test proto`).

Verify the amortisation: after `daemon start`, two separate `get` invocations
show exactly **one** `Attached to vfio-user` line in `<sock>.log` and **none**
per CLI; results are byte-exact with the direct path.

## Large values

`store`/`get` handle values up to the controller's 64 MiB max I/O size via a
region-bounded scatter-gather list (`spdk-jhk.7.8`). `get` first probes the
stored length (so it never silently truncates a value larger than the first-pass
buffer; `spdk-jhk.7.9`) and re-reads at the exact size. Large-file round-trips
(4 KiB … 64 MiB) are byte-exact on the CPU datapath; the `--gpu` path passes the
value as a single argv token and is therefore limited to small UTF-8 values
(see `--gpu` above).

## Status

Implemented and verified end-to-end against a live two-tier `rados-nkvx` target:
`store` / `get` (config-resolved `ns/key`, file/stdin/stdout, up to 64 MiB) with
server-honoured `-o ttl/ephemeral/touch` options + the `prefetch` read hint
(`spdk-jhk.7.14`); `exec` (named KV-Exec ops, e.g. `bytecount` /
`identity`); `list` (namespaces; per-key listing reported unsupported, never
faked); `ns create` / `ns allowlist` over JSON-RPC (`src/rpc.rs`); `--gpu`
delegation to `nkv_vfu_gpu` (and the opt-in **native HIP `--gpu`** under
`--features gpu-native`, `src/gpu_native.rs` + `csrc/nkvx_gpu.hip`;
`spdk-jhk.7.15`); and the opt-in **session daemon** that amortises the
EAL/attach cost across processes (`daemon start|stop|status`, `src/daemon.rs` +
`src/proto.rs`; `spdk-jhk.7.11`). The GPU/CPU regression
(`../vfu_host/run_tests.sh`) is green after the driver `nsid` parameterisation.

Target-side follow-ups (not in v1): a server-side namespace-name registry
(retire the client-side `name -> nsid` map); and a real `list` key-enumeration RPC on
the kvdev. Server semantics for `-o ttl/ephemeral/touch` landed in `spdk-jhk.7.14`
(`prefetch` stays a documented read hint with no in-memory meaning).
