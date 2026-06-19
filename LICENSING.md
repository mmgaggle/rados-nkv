# Licensing

This repository is **LGPL-3.0** (see [`COPYING`](COPYING)), aligning with the
Ceph project / ceph-nvmeof convention for the intended Ceph-organization home.

It mixes a small number of permissively-licensed, SPDK-derived components — this
is intentional and compatible (BSD-3-Clause is permissive and may be combined
into an LGPL-3.0 work; the BSD notices are retained):

| Path | License | Why |
|------|---------|-----|
| umbrella code (`scripts/`, `provisioning/`, `CMakeLists.txt`, `Makefile`) | **LGPL-3.0** | project-native glue |
| `clients/` (SPDK-derived harnesses) | **BSD-3-Clause** | SPDK NVMe-KV host/test harnesses, derived from the SPDK tree; also carried in the `spdk` submodule |
| `clients/vllm-weights/` | **LGPL-3.0** | weights catalog (vLLM publisher + loader), project-native |
| `rados-nkvx/` | **BSD-3-Clause** | the Exec executor, originated in the SPDK tree (`module/kvdev/rados/nkvx_service`) |
| `spdk/` (submodule) | **BSD-3-Clause** | SPDK + our NVMe-KV/RADOS work, tracked at `mmgaggle/spdk@devel` |
| other submodules (`ceph`, `nixl`, …) | their own | upstream |

Per-file SPDX / copyright headers are authoritative for each file.
