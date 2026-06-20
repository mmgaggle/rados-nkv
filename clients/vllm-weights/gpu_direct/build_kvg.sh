#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Build libkvg_gpu.so: the GPU-direct NVMe-KV host shim (bead spdk-p9k.1.1,
# Rung 2). Compiles kvg_gpu.hip with hipcc, linking the PREBUILT SPDK from the
# primary checkout, the raw vfio-user host client (nkv_vfu.h), HIP (libamdhip64)
# and HSA (libhsa-runtime64) for the dma-buf export. The .so exposes the C ABI
# the ctypes binding (src/rados_nkv_weights/_kvshim_gpu.py) loads.
#
# Mirrors gpu_direct/build.sh but produces a -shared library instead of an
# executable. -fgpu-rdc is NOT used; a single TU keeps it simple.
set -euo pipefail

HERE=$(readlink -f "$(dirname "$0")")
WT="${SPDK_ROOT:-/home/kyle/src/rados-nkv/spdk}"
VFU_HOST="${VFU_HOST_DIR:-/home/kyle/src/rados-nkv/clients/nvme-kv/kv/vfu_host}"
OUT="${OUT:-$HERE/libkvg_gpu.so}"

[ -d "$WT/build/lib" ] || { echo "ERROR: prebuilt SPDK not found at $WT/build/lib"; exit 1; }

INC="-I$WT/include \
 -I$WT/build/libvfio-user/usr/local/include \
 -I$VFU_HOST \
 -I$WT/lib/vfio_user/host \
 -I$WT/isa-l/.. -I$WT/isalbuild -I$WT/isa-l-crypto/.. -I$WT/isalcryptobuild"

LINK_LIBS="-pthread -Wl,-z,relro,-z,now -Wl,-z,noexecstack \
	-L$WT/build/libvfio-user/usr/local/lib -L$WT/build/lib \
	-Wl,--whole-archive -Wl,--no-as-needed \
	-lspdk_vfio_user -lspdk_util -lspdk_log \
	-Wl,--no-whole-archive \
	$WT/build/lib/libspdk_env_dpdk.a \
	-Wl,--whole-archive \
	$WT/dpdk/build/lib/librte_*.a \
	-Wl,--no-whole-archive \
	-lnuma -ldl -libverbs -lrdmacm \
	$WT/isa-l/.libs/libisal.a $WT/isa-l-crypto/.libs/libisal_crypto.a \
	-lvfio-user -ljson-c -pthread -lrt -luuid -lssl -lcrypto -lm \
	-llz4 -lfuse3 -lkeyutils \
	-lamdhip64 -lhsa-runtime64"

echo "== compiling kvg_gpu.hip (-fPIC) =="
hipcc -x hip -std=c++17 -D_GNU_SOURCE -Wno-array-bounds -fPIC \
	-c "$HERE/kvg_gpu.hip" -o "$HERE/kvg_gpu.o" $INC

echo "== linking $OUT (shared) =="
hipcc -shared "$HERE/kvg_gpu.o" -o "$OUT" $LINK_LIBS

echo "built $OUT"
