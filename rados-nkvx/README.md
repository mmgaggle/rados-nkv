# rados-nkvx — standalone Exec executor

The restartable wasmtime-sandbox executor that runs Exec modules on the storage
host (in a sandboxed executor, **never** linked into `ceph-osd`).
Moved out of the SPDK tree (it is non-SPDK by design) to keep the spdk fork
small — the analog of `clients/` for the executor side.

| File | What |
|------|------|
| `nkvx_service.c`, `nkvx_executor.{c,h}` | The executor service: cold-fills objects from RADOS, runs the wasmtime sandbox (dlopen C-API). |
| `nkvx_exec_client.c` | Test client for the Exec RPC contract. |
| `nkvx_log_shim.c` | Minimal `spdk_log` shim (the only SPDK runtime symbol the reused wasm core needs). |
| `nkvx_*_test.sh`, `deploy/` | Slice C acceptance harnesses + memlock deploy config. |

## Build

Non-SPDK build; links Mercury + librados (not SPDK libs). It reuses a few shared
sources from the spdk submodule's `module/kvdev/rados/` (the Exec RPC contract
`nkvx_exec_rpc.c`, the wasm core `kvdev_rados_nkvx_wasm.c`, and the vendored
`wasmtime/include/` headers — these stay in the fork because the in-tree
`kvdev_rados` SPDK module compiles them too). The `Makefile` defaults
`SPDK_ROOT` to the `../spdk` submodule; Mercury defaults to
`$SPDK_ROOT/vendor/mercury-install`.

```bash
make                                  # -> nkvx_service, nkvx_exec_client
make SPDK_ROOT=/path/to/spdk          # override the spdk tree
make MERCURY_PREFIX=/path/to/mercury  # override the Mercury install
make ASAN=1                           # AddressSanitizer build (Slice C8)
```

> The `kvdev_rados` backend itself (and the `nkvx_front_client` driver these
> scripts build via `Makefile.front.ut`) is **SPDK module code** and stays in
> the spdk fork — it compiles into `nvmf_tgt`.
