# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Standalone build for the rados-nkvx executor service skeleton (Slice C2) and
# its test client. NOT part of the SPDK build graph: the executor is non-SPDK by
# design (docs/design/slice-c-exec-rpc-mercury.md §1). Both binaries link the
# shared C3 contract (../nkvx_exec_rpc.c) directly against Mercury and include
# the in-tree spdk/kvdev.h (status enum only).
#
# Usage:
#   make -f Makefile [MERCURY_PREFIX=<mercury-install>]      # build
#   make -f Makefile run                                     # build + loopback test
#   make -f Makefile clean
# MERCURY_PREFIX defaults to the worktree's vendored install (scripts/vendor-mercury.sh).

SPDK_ROOT       ?= $(realpath $(CURDIR)/../spdk)
RADOS_DIR       := $(SPDK_ROOT)/module/kvdev/rados
MERCURY_PREFIX  ?= $(SPDK_ROOT)/vendor/mercury-install

MERCURY_CFLAGS  := $(shell PKG_CONFIG_PATH=$(MERCURY_PREFIX)/lib/pkgconfig pkg-config --cflags mercury)
MERCURY_LIBS    := $(shell PKG_CONFIG_PATH=$(MERCURY_PREFIX)/lib/pkgconfig pkg-config --libs mercury)

# -std=gnu11 is LOAD-BEARING: the C3 wire-status SPDK_STATIC_ASSERTs in
# ../nkvx_exec_rpc.c only fire when <assert.h> defines static_assert as a macro
# (true under gnu11, NOT under the compiler default C23). The executor MUST build
# the contract under gnu11 or the drift guard is decorative (design §3).
# -Wno-comment: the reused kvdev_rados_nkvx_wasm.h carries a benign "*x/*y"-style
# comment that trips -Wcomment under -Werror; SPDK's own build does not gate on it.
CFLAGS  := -g -O0 -std=gnu11 -Wall -Wextra -Werror -Wno-comment \
           -I$(SPDK_ROOT)/include -I$(RADOS_DIR) -I$(RADOS_DIR)/wasmtime/include $(MERCURY_CFLAGS)
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

CONTRACT := $(RADOS_DIR)/nkvx_exec_rpc.c
HEADER   := $(RADOS_DIR)/nkvx_exec_rpc.h

# Slice C5a: the executor backend cold-fills objects from RADOS (cold path only,
# ADR-0009), so the service binary links librados. C5a.2 additionally reuses the
# pure wasm runtime core (kvdev_rados_nkvx_wasm.c) for real-wasm Exec: it needs
# the wasmtime C-API headers (dlopen'd at runtime, no link dep), -lcrypto for the
# sha256 gate, -ldl for dlopen, and a tiny spdk_log shim (the only SPDK runtime
# symbol the core references). The client never touches RADOS or wasm.
SERVICE := nkvx_service
CLIENT  := nkvx_exec_client
SERVICE_SRCS := nkvx_service.c nkvx_executor.c nkvx_log_shim.c \
                $(RADOS_DIR)/kvdev_rados_nkvx_wasm.c $(CONTRACT)
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
	$(CC) $(CFLAGS) -o $@ $(SERVICE_SRCS) $(SERVICE_LIBS) $(LDFLAGS)

$(CLIENT): nkvx_exec_client.c $(CONTRACT) $(HEADER)
	$(CC) $(CFLAGS) -o $@ nkvx_exec_client.c $(CONTRACT) $(LDFLAGS)

$(UNIT_TEST): nkvx_unit_test.c nkvx_oid.h
	$(CC) $(UNIT_CFLAGS) -o $@ nkvx_unit_test.c

# Build + run the unit tests (the fast, hermetic check).
test: $(UNIT_TEST)
	./$(UNIT_TEST)

run: all
	MERCURY_PREFIX=$(MERCURY_PREFIX) ./nkvx_service_test.sh

clean:
	rm -f $(SERVICE) $(CLIENT) $(UNIT_TEST)

.PHONY: all test run clean
