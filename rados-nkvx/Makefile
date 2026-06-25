# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Standalone build for the rados-nkvx executor service skeleton (Slice C2) and
# its test client. NOT part of the SPDK build graph: the executor is non-SPDK by
# design (docs/design/slice-c-exec-rpc-mercury.md §1). Both binaries link the
# shared contract (../target/nkvx_exec_rpc.c) directly against Mercury and
# include our KV-Exec ABI header (../target/include/spdk/kvdev.h, status enum +
# Exec ABI). SPDK headers come from an UNMODIFIED SPDK tree (Gerrit 28298 +
# memory_domain), referenced via SPDK_ROOT/include only.
#
# Slice F (spdk-7sr.17.7): the shared wire contract + wasm runtime core used to
# live in the (now-gone) in-tree SPDK module module/kvdev/rados. They have moved
# out-of-tree to ../target. This Makefile therefore pulls the contract
# (nkvx_exec_rpc.c) and the wasm core (kvdev_rados_nkvx_wasm.c) from ../target,
# our spdk/kvdev.h from ../target/include, and the wasmtime C-API headers from
# ../target/wasmtime/include — while SPDK_ROOT points at an unmodified SPDK tree
# used only for its public headers (spdk/stdinc.h, spdk/log.h, spdk/util.h, ...).
#
# Usage:
#   make -f Makefile [MERCURY_PREFIX=<mercury-install>]      # build
#   make -f Makefile run                                     # build + loopback test
#   make -f Makefile clean
# MERCURY_PREFIX defaults to the worktree's vendored install (scripts/vendor-mercury.sh).
# SPDK_ROOT defaults to the rados-nkv submodule ../spdk; point it at a built,
# unmodified SPDK tree (e.g. SPDK_ROOT=/path/to/unmodified/spdk) when the
# submodule is not built in this worktree.

SPDK_ROOT       ?= $(abspath $(CURDIR)/../spdk)
TARGET_DIR      := $(abspath $(CURDIR)/../target)

# Mercury is vendored under the rados-nkv submodule's spdk/ tree
# (scripts/vendor-mercury.sh installs it at spdk/vendor/mercury-install). When
# SPDK_ROOT is the submodule (the default) that vendored install lives at
# $(CURDIR)/../spdk/vendor/mercury-install. But Slice F lets SPDK_ROOT point at
# an UNMODIFIED SPDK tree that carries no vendored Mercury, so the default cannot
# be tied to SPDK_ROOT. Default to the submodule's mercury-install, and if it is
# not built in this worktree fall back to the rados-nkv repo's vendored install
# (the one the acceptance/--help run uses). Override MERCURY_PREFIX=<dir> to point
# elsewhere.
MERCURY_SUBMODULE_PREFIX := $(abspath $(CURDIR)/../spdk)/vendor/mercury-install
MERCURY_FALLBACK_PREFIX  := /home/kyle/src/rados-nkv/spdk/vendor/mercury-install
ifneq ($(wildcard $(MERCURY_SUBMODULE_PREFIX)/lib/pkgconfig/mercury.pc),)
MERCURY_PREFIX  ?= $(MERCURY_SUBMODULE_PREFIX)
else
MERCURY_PREFIX  ?= $(MERCURY_FALLBACK_PREFIX)
endif

MERCURY_CFLAGS  := $(shell PKG_CONFIG_PATH=$(MERCURY_PREFIX)/lib/pkgconfig pkg-config --cflags mercury)
MERCURY_LIBS    := $(shell PKG_CONFIG_PATH=$(MERCURY_PREFIX)/lib/pkgconfig pkg-config --libs mercury)

# -std=gnu11 is LOAD-BEARING: the wire-status SPDK_STATIC_ASSERTs in
# ../target/nkvx_exec_rpc.c only fire when <assert.h> defines static_assert as a
# macro (true under gnu11, NOT under the compiler default C23). The executor MUST
# build the contract under gnu11 or the drift guard is decorative (design §3).
# -Wno-comment: the reused kvdev_rados_nkvx_wasm.h carries a benign "*x/*y"-style
# comment that trips -Wcomment under -Werror; SPDK's own build does not gate on it.
# -DNKVX_WITH_MERCURY: our out-of-tree Mercury build knob (Slice D). The executor
# always links Mercury, so it is always set here.
# -DNKVX_WITH_WASM: our out-of-tree WASM build knob (Slice K). Unmodified SPDK has
# no wasm configure knob, so the reused wasm core (kvdev_rados_nkvx_wasm.c) is gated
# on this macro; without it the core compiles to its NOT_SUPPORTED stub and real
# KV-Exec returns -7. The executor IS the real-wasm runtime, so it is always set
# here (libwasmtime.so is still dlopen'd lazily — no link dependency).
# Include search order: ../target (shared contract + wasm headers, quoted
# includes), ../target/include (our spdk/kvdev.h — NOT present in unmodified
# SPDK), ../target/wasmtime/include (wasmtime C-API, dlopen'd), then the
# unmodified SPDK public headers under SPDK_ROOT/include.
CFLAGS  := -g -O0 -std=gnu11 -Wall -Wextra -Werror -Wno-comment -DNKVX_WITH_MERCURY -DNKVX_WITH_WASM \
           -I$(TARGET_DIR) -I$(TARGET_DIR)/include -I$(TARGET_DIR)/wasmtime/include \
           -I$(SPDK_ROOT)/include $(MERCURY_CFLAGS)

# Explicit librados library-path knob (slice spdk-7sr.17.11 / J). The host's
# system /usr/lib64/librados.so is a PARTIAL STUB (~1.4MB) that lacks symbols the
# executor needs; the real cluster ships a full librados under the Ceph build
# tree. Default EMPTY (use the system librados via the linker's default search),
# but allow RADOS_LIB_DIR=/home/kyle/src/ceph/build/lib to link + rpath the real
# library so the executor serves base KV verbs against the live pool.
RADOS_LIB_DIR ?=
ifneq ($(strip $(RADOS_LIB_DIR)),)
RADOS_LDFLAGS := -L$(RADOS_LIB_DIR) -Wl,-rpath,$(RADOS_LIB_DIR)
else
RADOS_LDFLAGS :=
endif

LDFLAGS := $(MERCURY_LIBS) -Wl,-rpath,$(MERCURY_PREFIX)/lib

# Slice C8 (bead spdk-xmu.8): ASan build of the executor for the on-rig verbs
# acceptance run (abort/cancel ASan-clean over verbs). `make ASAN=1` adds
# AddressSanitizer + frame pointers to BOTH compile and link so the produced
# nkvx_service is the instrumented binary the C8 harness drives under verbs.
# Off-rig we dry-validate the same binary on na+sm:// and ofi+tcp://127.0.0.1.
# The ASan build lands in the SAME path (nkvx_service) — a normal `make` (no
# ASAN=1) overwrites it back to the uninstrumented binary, so the two builds are
# not co-resident; the C8 harness rebuilds with the flavor it needs.
ifeq ($(ASAN),1)
CFLAGS  += -fsanitize=address -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address
endif

CONTRACT := $(TARGET_DIR)/nkvx_exec_rpc.c
HEADER   := $(TARGET_DIR)/nkvx_exec_rpc.h

# Slice C5a: the executor backend cold-fills objects from RADOS (cold path only,
# ADR-0009), so the service binary links librados. C5a.2 additionally reuses the
# pure wasm runtime core (kvdev_rados_nkvx_wasm.c) for real-wasm Exec: it needs
# the wasmtime C-API headers (dlopen'd at runtime, no link dep), -lcrypto for the
# sha256 gate, -ldl for dlopen, and a tiny spdk_log shim (the only SPDK runtime
# symbol the core references). The client never touches RADOS or wasm.
SERVICE := nkvx_service
CLIENT  := nkvx_exec_client
SERVICE_SRCS := nkvx_service.c nkvx_executor.c nkvx_log_shim.c \
                $(TARGET_DIR)/kvdev_rados_nkvx_wasm.c $(CONTRACT)
SERVICE_LIBS := -lrados -lcrypto -ldl -lpthread

# Fast, dependency-free unit tests for the pure ADR-0014 contract helpers
# (nkvx_oid.h). Deliberately links NOTHING — no Mercury, librados, or wasm — so
# `make -f Makefile test` runs on any host with a C compiler (CI, a laptop). The
# end-to-end shell suite (nkvx_*_test.sh, driven by `make run` / the top-level
# `make check-integration`) covers the I/O-bound paths these helpers feed.
UNIT_TEST    := nkvx_unit_test
UNIT_CFLAGS  := -g -O0 -std=gnu11 -Wall -Wextra -Werror

all: $(SERVICE) $(CLIENT)

$(SERVICE): $(SERVICE_SRCS) nkvx_executor.h $(HEADER)
	$(CC) $(CFLAGS) -o $@ $(SERVICE_SRCS) $(SERVICE_LIBS) $(RADOS_LDFLAGS) $(LDFLAGS)

$(CLIENT): nkvx_exec_client.c $(CONTRACT) $(HEADER)
	$(CC) $(CFLAGS) -o $@ nkvx_exec_client.c $(CONTRACT) $(LDFLAGS)

$(UNIT_TEST): nkvx_unit_test.c nkvx_oid.h
	$(CC) $(UNIT_CFLAGS) -o $@ nkvx_unit_test.c

# Slice spdk-7sr.4 (S3): the RETRIEVE round-trip driver. Drives the SAME
# nkvx_front_client.c the integrated front uses, against an --mem-object service.
# Sources come from ../target (front client + contract) and ../target/test (the
# driver); no RADOS link (it talks to the service over na+sm).
KV_RETRIEVE_TEST := nkvx_kv_retrieve_test
$(KV_RETRIEVE_TEST): $(TARGET_DIR)/test/nkvx_kv_retrieve_test.c $(TARGET_DIR)/nkvx_front_client.c \
                     $(TARGET_DIR)/nkvx_front_client.h $(CONTRACT) $(HEADER)
	$(CC) $(CFLAGS) -o $@ $(TARGET_DIR)/test/nkvx_kv_retrieve_test.c \
	      $(TARGET_DIR)/nkvx_front_client.c $(CONTRACT) $(LDFLAGS)

# Slice spdk-7sr.17.18 ("B"): the STORE round-trip driver. Stores a value of a given
# length and retrieves it back byte-exact through the SAME nkvx_front_client.c the
# integrated front uses; for value_len > NKVX_INLINE_MAX this exercises the zero-copy
# value_bulk RDMA-PULL (store) and the result_sink PUSH (retrieve) against an
# --mem-object service. Same source set as the RETRIEVE driver; no RADOS link.
KV_STORE_TEST := nkvx_kv_store_test
$(KV_STORE_TEST): $(TARGET_DIR)/test/nkvx_kv_store_test.c $(TARGET_DIR)/nkvx_front_client.c \
                  $(TARGET_DIR)/nkvx_front_client.h $(CONTRACT) $(HEADER)
	$(CC) $(CFLAGS) -o $@ $(TARGET_DIR)/test/nkvx_kv_store_test.c \
	      $(TARGET_DIR)/nkvx_front_client.c $(CONTRACT) $(LDFLAGS)

# Slice spdk-7sr.5 (S4): direct executor-verb test. Drives nkvx_executor_kv()
# against the in-memory backend for STORE/DELETE/EXIST/LIST + SIKE/SINKE + the
# read-only gate, no Mercury and no Ceph cluster on the data path. Links the
# executor backend, the reused wasm core (cache-invalidate path) + its log shim,
# and librados (the wasm core's link dep; the data path uses the mem backend).
KV_VERBS_TEST := nkvx_kv_verbs_test
# -I$(CURDIR): nkvx_kv_verbs_test.c (in ../target/test) quote-includes
# "nkvx_executor.h", which lives HERE in rados-nkvx, so the standalone executor
# header must be on the search path even though the test source is out-of-dir.
$(KV_VERBS_TEST): $(TARGET_DIR)/test/nkvx_kv_verbs_test.c nkvx_executor.c nkvx_executor.h \
                  nkvx_log_shim.c $(TARGET_DIR)/kvdev_rados_nkvx_wasm.c $(CONTRACT) $(HEADER)
	$(CC) $(CFLAGS) -I$(CURDIR) -o $@ $(TARGET_DIR)/test/nkvx_kv_verbs_test.c nkvx_executor.c nkvx_log_shim.c \
	      $(TARGET_DIR)/kvdev_rados_nkvx_wasm.c $(CONTRACT) $(SERVICE_LIBS) $(RADOS_LDFLAGS) $(LDFLAGS)

# Build + run the unit tests (the fast, hermetic check).
test: $(UNIT_TEST)
	./$(UNIT_TEST)

# Build + run the S4 executor-verb test (no Mercury/Ceph data path needed).
verbs-test: $(KV_VERBS_TEST)
	./$(KV_VERBS_TEST)

run: all
	MERCURY_PREFIX=$(MERCURY_PREFIX) ./nkvx_service_test.sh

# Build + run the slice-B STORE round-trip over na+sm (zero-copy value_bulk PULL).
store-test: $(SERVICE) $(KV_STORE_TEST)
	MERCURY_PREFIX=$(MERCURY_PREFIX) ./nkvx_kv_store_test.sh

clean:
	rm -f $(SERVICE) $(CLIENT) $(UNIT_TEST) $(KV_RETRIEVE_TEST) $(KV_VERBS_TEST) $(KV_STORE_TEST)

.PHONY: all test verbs-test store-test run clean
