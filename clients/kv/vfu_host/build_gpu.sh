#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#  Build the GPU-initiated variant (Milestone 2d) with hipcc, linking the SPDK
#  static libs. The CPU variant (nkv_vfu_host) builds via the normal SPDK
#  Makefile; only this one needs hipcc + libamdhip64.
set -euo pipefail

HERE=$(readlink -f "$(dirname "$0")")    # this vfu_host dir (the Makefile lives here)
WT=$(readlink -f "$HERE/../../../spdk")  # spdk submodule of the umbrella repo

# Ensure the SPDK libs + CPU variant are built (gives us the link inputs).
make -C "$HERE" >/dev/null

INC="-I$WT/include \
 -I$WT/build/libvfio-user/usr/local/include \
 -I$WT/isa-l/.. -I$WT/isalbuild -I$WT/isa-l-crypto/.. -I$WT/isalcryptobuild"

echo "== compiling nkv_vfu_gpu.hip with hipcc =="
hipcc -x hip -std=c++17 -D_GNU_SOURCE -Wno-array-bounds \
	-c "$HERE/nkv_vfu_gpu.hip" -o "$HERE/nkv_vfu_gpu.o" $INC

echo "== linking nkv_vfu_gpu =="
hipcc "$HERE/nkv_vfu_gpu.o" -o "$HERE/nkv_vfu_gpu" \
	-pthread -Wl,-z,relro,-z,now -Wl,-z,noexecstack \
	-L"$WT/build/libvfio-user/usr/local/lib" -L"$WT/build/lib" \
	-Wl,--whole-archive -Wl,--no-as-needed \
	-lspdk_vfio_user -lspdk_util -lspdk_log \
	-Wl,--no-whole-archive \
	"$WT/build/lib/libspdk_env_dpdk.a" \
	-Wl,--whole-archive \
	$WT/dpdk/build/lib/librte_*.a \
	-Wl,--no-whole-archive \
	-lnuma -ldl -libverbs -lrdmacm \
	"$WT/isa-l/.libs/libisal.a" "$WT/isa-l-crypto/.libs/libisal_crypto.a" \
	-lvfio-user -ljson-c -pthread -lrt -luuid -lssl -lcrypto -lm \
	-llz4 -lfuse3 -lkeyutils \
	-lamdhip64

echo "built $HERE/nkv_vfu_gpu"
