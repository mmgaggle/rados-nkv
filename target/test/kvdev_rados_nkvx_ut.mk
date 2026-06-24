#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 rados-nkv contributors. All rights reserved.
#
# Out-of-tree CUnit build for kvdev_rados_nkvx_ut, which exercises the off-reactor
# Exec executor AND the dlopen-backed wasm runtime core in one translation unit.
# Driven by the top-level target/test/Makefile (SPDK_ROOT_DIR = unmodified SPDK,
# TARGET_DIR = this module, WASM_DIR = the checked-in .wasm fixtures).
#
# The UT #includes (as source) kvdev_rados_nkvx.c, kvdev_rados_nkvx_wasm.c, and
# kvdev_rados.h — all now under $(TARGET_DIR) — so -I$(TARGET_DIR) resolves the
# quoted includes and -I$(TARGET_DIR)/include resolves our out-of-tree spdk/kvdev.h.
#
# Real-wasm path: unmodified SPDK has no wasm configure knob, so the wasm core is
# gated on OUR out-of-tree macro NKVX_WITH_WASM (Slice K, parallel to
# NKVX_WITH_MERCURY). We define it here to compile the live wasmtime path (vs the
# NOT_SUPPORTED stub). -I.../wasmtime/include for the dlopen'd C-API
# headers, -ldl for dlopen, -lcrypto for the sha256 verify gate, and
# -DNKVX_UT_WASM_DIR pointing at the checked-in .wasm fixtures. libwasmtime.so is
# dlopen'd at run time (LD_LIBRARY_PATH must include its dir, e.g. /usr/local/lib).

TARGET_DIR ?= $(abspath $(CURDIR)/..)
WASM_DIR   ?= $(abspath $(CURDIR)/../../rados-nkvx/wasm)

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

TEST_FILE = kvdev_rados_nkvx_ut.c

CFLAGS += -I$(TARGET_DIR)
CFLAGS += -I$(TARGET_DIR)/include
CFLAGS += -I$(TARGET_DIR)/wasmtime/include
CFLAGS += -DNKVX_WITH_WASM
CFLAGS += -DNKVX_UT_WASM_DIR=\"$(WASM_DIR)\"
LDFLAGS += -ldl
LIBS += -lcrypto

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
