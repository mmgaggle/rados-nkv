# clients — NVMe-KV host & test harness

Standalone KV clients and tests, moved out of the SPDK tree to keep the spdk
fork small. They build **against the `spdk` submodule** (their Makefiles set
`SPDK_ROOT_DIR := $(CURDIR)/../../spdk`), so build SPDK first. See
[`../docs/build.md`](../docs/build.md) step 1b.

| Path | What |
|------|------|
| `kv_shim/` | Reusable in-process KV client `kv_host_shim.{c,h}` + `kv_shim_test`. Consumed by `nixl` and `rados-nkv-weights`. |
| `kv/` | Standalone hosts `kv_host` / `kv_ro_host`, fio verify (`kv_verify.fio`), vfio-user + Exec test scripts. |
| `kv/rados-nkv/` | The `rados-nkv` crate (binary: `rkv`), a Rust/HIP CLI (CPU default; `gpu-native` feature needs hipcc/ROCm). |
| `kv/vfu_host/` | Raw vfio-user host (`nkv_vfu_host`) + GPU-initiated variant and two-tier bring-up scripts. |
| `kv/wasm/` | WASM Exec test modules. |
| `kv/cls/` | Ceph object-class reference (builds against the `ceph` submodule, not SPDK). |
| `kv_rados/` | RADOS-backed KV host `kv_rados_host` + fio/vfio-user scripts. |

Build paths default to the spdk submodule; override the Rust crate's SPDK build
dir with `NKVX_SPDK_BUILD=<spdk>/build`.
