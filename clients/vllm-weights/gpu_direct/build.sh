#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Build the host-side GPU-direct probe (bead spdk-p9k.1). Links the PREBUILT
# SPDK from the primary checkout (this worktree only has git-tracked files), the
# vfu_host header, HIP (libamdhip64) and HSA (libhsa-runtime64) for the dma-buf
# export probe.
set -euo pipefail

HERE=$(readlink -f "$(dirname "$0")")
# Prefer the primary checkout's prebuilt SPDK artifacts.
WT="${SPDK_ROOT:-/home/kyle/src/rados-nkv/spdk}"
VFU_HOST="${VFU_HOST_DIR:-/home/kyle/src/rados-nkv/clients/nvme-kv/kv/vfu_host}"

[ -d "$WT/build/lib" ] || { echo "ERROR: prebuilt SPDK not found at $WT/build/lib"; exit 1; }

# -I.../host gives the dma-buf-map probe access to vfio_user_internal.h
# (vfio_user_dev_dma_map_unmap + struct vfio_memory_region).
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

for prog in gpu_direct_probe gpu_direct_dmamap; do
	echo "== compiling $prog.hip =="
	hipcc -x hip -std=c++17 -D_GNU_SOURCE -Wno-array-bounds \
		-c "$HERE/$prog.hip" -o "$HERE/$prog.o" $INC
	echo "== linking $prog =="
	hipcc "$HERE/$prog.o" -o "$HERE/$prog" $LINK_LIBS
	echo "built $HERE/$prog"
done
