#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-only
# Copyright (c) 2026, IBM Corporation. All rights reserved.
#
# Build libradosnkv_kvshim.so: the in-process NVMe-KV host shim
# (kv_host_shim.c from the SPDK tree) linked against the SPDK + DPDK static
# libraries, exposed as a shared library that the ctypes wrapper in
# src/rados_nkv_weights/_kvshim.py loads.
#
# The SPDK link set (whole-archive SPDK libs + libspdk_env_dpdk.a + the DPDK
# static archives + isa-l + system libs) is modelled on how kv_shim_test links
# (see $SPDK_ROOT/test/nvmf/kv_shim/Makefile and the `make V=1` LINK line). We
# compile only kv_host_shim.c (not the test driver) into a -shared object so the
# shim's public C ABI (kv_host_shim_open/close/dma/store/retrieve/exist/delete,
# max_value_len/max_key_len) is callable from Python.
#
# Usage:  SPDK_ROOT=/mnt/spdk ./native/build.sh
# Env:    SPDK_ROOT  (default /mnt/spdk)   — root of a built SPDK tree
#         CC         (default cc)
#         OUT        (default <this dir>/libradosnkv_kvshim.so)

set -euo pipefail

SPDK_ROOT="${SPDK_ROOT:-/mnt/spdk}"
CC="${CC:-cc}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE/libradosnkv_kvshim.so}"

SHIM_DIR="$SPDK_ROOT/test/nvmf/kv_shim"
SHIM_SRC="$SHIM_DIR/kv_host_shim.c"

if [[ ! -f "$SHIM_SRC" ]]; then
	echo "build.sh: cannot find kv_host_shim.c at $SHIM_SRC" >&2
	echo "build.sh: set SPDK_ROOT to a built SPDK tree (have: SPDK_ROOT=$SPDK_ROOT)" >&2
	exit 1
fi
if [[ ! -f "$SPDK_ROOT/build/lib/libspdk_nvme.a" ]]; then
	echo "build.sh: SPDK does not look built ($SPDK_ROOT/build/lib/libspdk_nvme.a missing)" >&2
	exit 1
fi

DPDK_LIB="$SPDK_ROOT/dpdk/build/lib"

# --- DPDK static archives (whole-archive), mirroring the kv_shim_test LINK line.
DPDK_ARCHIVES=(
	librte_argparse librte_bus_pci librte_cryptodev librte_dmadev librte_eal
	librte_ethdev librte_hash librte_kvargs librte_log librte_mbuf
	librte_mempool librte_mempool_ring librte_meter librte_net librte_pci
	librte_power librte_power_acpi librte_power_amd_pstate librte_power_cppc
	librte_power_intel_pstate librte_power_intel_uncore librte_power_kvm_vm
	librte_rcu librte_ring librte_telemetry librte_timer librte_vhost
)
dpdk_args=()
for a in "${DPDK_ARCHIVES[@]}"; do
	f="$DPDK_LIB/$a.a"
	# Not all DPDK builds ship every optional archive; include those present.
	if [[ -f "$f" ]]; then
		dpdk_args+=("$f")
	fi
done

CFLAGS=(
	-fPIC -g -O2 -DNDEBUG -pthread -fno-strict-aliasing
	-D_GNU_SOURCE -fstack-protector -fno-common -std=gnu11
	-fno-lto
	-I"$SPDK_ROOT/include"
	-I"$SHIM_DIR"
	-I"$SPDK_ROOT/build/libvfio-user/usr/local/include"
)

LDFLAGS=(
	-shared -fno-lto
	-Wl,-z,relro,-z,now -Wl,-z,noexecstack -fuse-ld=bfd
	-L"$SPDK_ROOT/build/libvfio-user/usr/local/lib"
)

# SPDK libraries (whole-archive so all referenced symbols are pulled in).
SPDK_WHOLE=(
	-lspdk_sock_posix -lspdk_nvme -lspdk_keyring -lspdk_sock -lspdk_trace
	-lspdk_rpc -lspdk_jsonrpc -lspdk_json -lspdk_dma -lspdk_vfio_user
	-lspdk_vmd -lspdk_util -lspdk_log
)

echo "build.sh: SPDK_ROOT=$SPDK_ROOT"
echo "build.sh: compiling $SHIM_SRC -> $OUT"

"$CC" "${CFLAGS[@]}" -c "$SHIM_SRC" -o "$HERE/kv_host_shim.o"

"$CC" -o "$OUT" \
	"${CFLAGS[@]}" \
	"${LDFLAGS[@]}" \
	"$HERE/kv_host_shim.o" \
	-L"$SPDK_ROOT/build/lib" \
	-Wl,--whole-archive -Wl,--no-as-needed "${SPDK_WHOLE[@]}" -Wl,--no-whole-archive \
	"$SPDK_ROOT/build/lib/libspdk_env_dpdk.a" \
	-Wl,--whole-archive "${dpdk_args[@]}" -Wl,--no-whole-archive \
	-lnuma -ldl \
	"$SPDK_ROOT/isa-l/.libs/libisal.a" \
	"$SPDK_ROOT/isa-l-crypto/.libs/libisal_crypto.a" \
	-lvfio-user -ljson-c \
	-pthread -lrt -luuid -lssl -lcrypto -lm -llz4 -lfuse3 -lkeyutils -laio \
	-lrados -lrbd

echo "build.sh: wrote $OUT"
