#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 rados-nkv contributors. All rights reserved.
#
# Out-of-tree build + run for the five KV test suites, against an UNMODIFIED,
# pre-built SPDK (Gerrit 28298 + memory_domain) and this repo's target/ module.
# No SPDK source is modified and there is NO dependency on an in-tree SPDK
# module/kvdev/rados directory.
#
# The five suites (Slice G, bead spdk-7sr.17.8):
#   kvrados_ut             CUnit; #includes target/bdev_kvrados.c          -> 25/25
#   kvdev_rados_nkvx_ut    CUnit; executor + dlopen wasm core, REAL wasm   -> 42/42
#   nkvx_exec_rpc_ut       standalone (own main); Mercury contract         -> 271 checks
#   nkvx_kv_verbs_test     standalone; executor KV verbs (no Mercury/Ceph) -> PASS
#   nkvx_kv_retrieve_test  standalone CLIENT (needs a running service)     -> BUILD only
# Plus the dma-buf bulk-path proof, BUILD only here:
#   nkvx_front_dmabuf_test standalone                                      -> BUILD only
#
# The two CUnit suites use SPDK's mk/spdk.unittest.mk harness via the per-suite
# stub makefiles kvrados_ut.mk / kvdev_rados_nkvx_ut.mk (recursive make). The
# standalone suites are plain $(CC) recipes mirroring the original in-tree
# Makefile.ut / Makefile.dmabuf.ut / nkvx_service/Makefile link sets.
#
# Usage (SPDK_ROOT is the pre-built UNMODIFIED SPDK; override per worktree):
#   make -C target/test SPDK_ROOT=/home/kyle/src/rados-nkv-wt/slice-a/spdk all
#   make -C target/test SPDK_ROOT=/home/kyle/src/rados-nkv-wt/slice-a/spdk run
#   make -C target/test clean

# ---- Locations --------------------------------------------------------------
TEST_DIR     := $(CURDIR)
TARGET_DIR   := $(abspath $(TEST_DIR)/..)
UMBRELLA_DIR := $(abspath $(TARGET_DIR)/..)
RADOS_NKVX   := $(UMBRELLA_DIR)/rados-nkvx
WASM_DIR     := $(RADOS_NKVX)/wasm

# Pre-built UNMODIFIED SPDK (carries mk/spdk.unittest.mk + the CUnit harness +
# the shared libspdk libs). spdk.unittest.mk wants SPDK_ROOT_DIR.
SPDK_ROOT     ?= $(abspath $(UMBRELLA_DIR)/spdk)
SPDK_ROOT_DIR := $(SPDK_ROOT)

# wasmtime C-API headers (dlopen'd at runtime; no link dep).
WASMTIME_INC_DIR := $(TARGET_DIR)/wasmtime/include

# Mercury inter-tier RPC transport (vendored install).
MERCURY_PREFIX  ?= /home/kyle/src/rados-nkv/spdk/vendor/mercury-install
MERCURY_CFLAGS  := $(shell PKG_CONFIG_PATH=$(MERCURY_PREFIX)/lib/pkgconfig pkg-config --cflags mercury)
MERCURY_LIBS    := $(shell PKG_CONFIG_PATH=$(MERCURY_PREFIX)/lib/pkgconfig pkg-config --libs mercury)

# libwasmtime.so install dir (dlopen'd by the wasm core at runtime).
WASMTIME_LIB_DIR ?= /usr/local/lib

# Shared SPDK libs the CUnit binaries dlopen/link at runtime (--with-shared).
SPDK_LIB_DIR  := $(SPDK_ROOT)/build/lib
VFIO_LIB_DIR  := $(SPDK_ROOT)/build/libvfio-user/usr/local/lib
DPDK_LIB_DIR  := $(SPDK_ROOT)/dpdk/build/lib
SPDK_RUN_LDPATH := $(SPDK_LIB_DIR):$(VFIO_LIB_DIR):$(DPDK_LIB_DIR)

# ---- Standalone (non-CUnit) compile/link flags ------------------------------
CC ?= cc
# -std=gnu11 is LOAD-BEARING for the contract's wire-status SPDK_STATIC_ASSERTs
# (they only fire when <assert.h> defines static_assert as a macro: true under
# gnu11, NOT under the compiler-default C23). -Wno-comment: a benign "*x/*y"
# comment in the reused wasm header trips -Wcomment under -Werror.
STD_CFLAGS := -g -O0 -std=gnu11 -Wall -Wextra -Werror -Wno-comment -DNKVX_WITH_MERCURY \
              -I$(TARGET_DIR) -I$(TARGET_DIR)/include -I$(WASMTIME_INC_DIR) \
              -I$(RADOS_NKVX) -I$(SPDK_ROOT)/include $(MERCURY_CFLAGS)
STD_LDFLAGS := $(MERCURY_LIBS) -Wl,-rpath,$(MERCURY_PREFIX)/lib

# Slice C8 (bead spdk-xmu.8): `make ASAN=1 [CC=clang]` instruments the standalone
# FRONT driver(s) (origin-side cancel/teardown) with AddressSanitizer so the abort
# path is ASan-clean alongside the executor under the C8 harness. Mirrors the old
# in-tree Makefile.front.ut / Makefile.dmabuf.ut ASAN toggle. The ASan and normal
# builds share the output path, so they are not co-resident (the harness rebuilds
# with the flavor it needs).
ifeq ($(ASAN),1)
STD_CFLAGS  += -fsanitize=address -fno-omit-frame-pointer
STD_LDFLAGS += -fsanitize=address
endif

# ---- Shared out-of-tree sources ---------------------------------------------
CONTRACT      := $(TARGET_DIR)/nkvx_exec_rpc.c
WASM_CORE     := $(TARGET_DIR)/kvdev_rados_nkvx_wasm.c
FRONT_CLIENT  := $(TARGET_DIR)/nkvx_front_client.c
EXECUTOR      := $(RADOS_NKVX)/nkvx_executor.c
LOG_SHIM      := $(RADOS_NKVX)/nkvx_log_shim.c

# ---- Suite binaries ---------------------------------------------------------
CUNIT_SUITES      := kvrados_ut kvdev_rados_nkvx_ut
STANDALONE_SUITES := nkvx_exec_rpc_ut nkvx_kv_verbs_test nkvx_kv_retrieve_test \
                     nkvx_front_client_test nkvx_front_dmabuf_test

.PHONY: all run clean $(CUNIT_SUITES)
.DEFAULT_GOAL := all

all: $(CUNIT_SUITES) $(STANDALONE_SUITES)

# ---- CUnit suites (recursive make through SPDK's unittest harness) ----------
kvrados_ut:
	$(MAKE) -f kvrados_ut.mk SPDK_ROOT_DIR=$(SPDK_ROOT_DIR) TARGET_DIR=$(TARGET_DIR)

kvdev_rados_nkvx_ut:
	$(MAKE) -f kvdev_rados_nkvx_ut.mk SPDK_ROOT_DIR=$(SPDK_ROOT_DIR) \
	        TARGET_DIR=$(TARGET_DIR) WASM_DIR=$(WASM_DIR)

# ---- Standalone suites ------------------------------------------------------
# nkvx_exec_rpc_ut: own main, links the Mercury contract only (Makefile.ut).
nkvx_exec_rpc_ut: nkvx_exec_rpc_ut.c $(CONTRACT) $(TARGET_DIR)/nkvx_exec_rpc.h
	$(CC) $(STD_CFLAGS) -o $@ nkvx_exec_rpc_ut.c $(CONTRACT) $(STD_LDFLAGS)

# nkvx_kv_verbs_test (S4): direct executor-verb test against the in-memory
# backend; links the executor + wasm core + log shim + contract (no Mercury/Ceph
# needed to run, but the executor's link deps still apply). Mirrors the
# nkvx_service Makefile's KV_VERBS_TEST recipe.
nkvx_kv_verbs_test: nkvx_kv_verbs_test.c $(EXECUTOR) $(LOG_SHIM) $(WASM_CORE) $(CONTRACT)
	$(CC) $(STD_CFLAGS) -o $@ nkvx_kv_verbs_test.c $(EXECUTOR) $(LOG_SHIM) \
	      $(WASM_CORE) $(CONTRACT) -lrados -lcrypto -ldl -lpthread $(STD_LDFLAGS)

# nkvx_kv_retrieve_test (S3): a CLIENT that drives the front client against a
# running --mem-object service. BUILD-only here (full run is slice H). Mirrors the
# nkvx_service Makefile's KV_RETRIEVE_TEST recipe.
nkvx_kv_retrieve_test: nkvx_kv_retrieve_test.c $(FRONT_CLIENT) $(CONTRACT)
	$(CC) $(STD_CFLAGS) -o $@ nkvx_kv_retrieve_test.c $(FRONT_CLIENT) \
	      $(CONTRACT) $(STD_LDFLAGS)

# nkvx_front_client_test: the REAL front Mercury client driver (Slice C4/C6/C6b/C7/
# C8 acceptance). Mirrors the old in-tree Makefile.front.ut link set (front client +
# contract directly against Mercury, -lcrypto for the C7 delivered-bytes sha gate).
# The nkvx_c6/c6b/c6b_f1f2/c7/c8 harnesses drive this binary against a live executor.
nkvx_front_client_test: nkvx_front_client_test.c $(FRONT_CLIENT) $(CONTRACT)
	$(CC) $(STD_CFLAGS) -o $@ nkvx_front_client_test.c $(FRONT_CLIENT) \
	      $(CONTRACT) -lcrypto $(STD_LDFLAGS)

# nkvx_front_dmabuf_test: the non-GPU dma-buf bulk-path proof. BUILD-only here.
# Mirrors Makefile.dmabuf.ut (front client + contract, -lcrypto for the sha gate).
nkvx_front_dmabuf_test: nkvx_front_dmabuf_test.c $(FRONT_CLIENT) $(CONTRACT)
	$(CC) $(STD_CFLAGS) -o $@ nkvx_front_dmabuf_test.c $(FRONT_CLIENT) \
	      $(CONTRACT) -lcrypto $(STD_LDFLAGS)

# ---- Run the four runnable suites (retrieve + dmabuf are BUILD-only) ---------
run: all
	@echo "=== kvrados_ut ==="
	LD_LIBRARY_PATH=$(SPDK_RUN_LDPATH):$(LD_LIBRARY_PATH) ./kvrados_ut
	@echo "=== kvdev_rados_nkvx_ut ==="
	LD_LIBRARY_PATH=$(SPDK_RUN_LDPATH):$(WASMTIME_LIB_DIR):$(LD_LIBRARY_PATH) ./kvdev_rados_nkvx_ut
	@echo "=== nkvx_exec_rpc_ut ==="
	LD_LIBRARY_PATH=$(MERCURY_PREFIX)/lib:$(LD_LIBRARY_PATH) ./nkvx_exec_rpc_ut
	@echo "=== nkvx_kv_verbs_test ==="
	LD_LIBRARY_PATH=$(MERCURY_PREFIX)/lib:$(WASMTIME_LIB_DIR):$(LD_LIBRARY_PATH) ./nkvx_kv_verbs_test

clean:
	$(MAKE) -f kvrados_ut.mk SPDK_ROOT_DIR=$(SPDK_ROOT_DIR) TARGET_DIR=$(TARGET_DIR) clean
	$(MAKE) -f kvdev_rados_nkvx_ut.mk SPDK_ROOT_DIR=$(SPDK_ROOT_DIR) TARGET_DIR=$(TARGET_DIR) clean
	rm -f $(STANDALONE_SUITES)
