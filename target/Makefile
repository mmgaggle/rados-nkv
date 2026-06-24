#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 rados-nkv contributors.
#  All rights reserved.
#
# Out-of-tree build for the KV bdev forwarder (bdev_kvrados) and a custom
# NVMe-oF target app that loads it. Built against an UNMODIFIED, pre-built
# SPDK configured --with-shared (Gerrit 28298 + memory_domain). No SPDK
# rebuild is performed here; we link the combined libspdk.so following the
# pattern in <spdk>/test/external_code/{passthru,hello_world}/Makefile.
#
# Default SPDK_ROOT is the rados-nkv submodule (../spdk). Because that
# submodule may not be built in every worktree, override on the command line:
#
#   make -C target SPDK_ROOT=/path/to/prebuilt/spdk
#
# Verified with:
#   make -C target SPDK_ROOT=/home/kyle/src/rados-nkv-wt/slice-a/spdk

SPDK_ROOT ?= $(abspath $(CURDIR)/../spdk)

# ---- SPDK locations (combined shared object layout) -------------------------
SPDK_HEADER_DIR    ?= $(SPDK_ROOT)/include
SPDK_BUILD_HEADER_DIR ?= $(SPDK_ROOT)/build/include
SPDK_LIB_DIR       ?= $(SPDK_ROOT)/build/lib
PKG_CONFIG_PATH    := $(SPDK_LIB_DIR)/pkgconfig

# DPDK + system libraries needed alongside the combined libspdk.so. We pull
# them straight from the SPDK pkg-config files so we track however this SPDK
# tree was configured (e.g. an external --with-dpdk path).
DPDK_LIB  := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_env_dpdk)
SYS_LIB   := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static spdk_syslibs)

# ---- Our out-of-tree dependencies -------------------------------------------
# KV-Exec ABI header lives under target/include (provides spdk/kvdev.h).
# Mercury is the inter-tier RPC transport; enabled via our NKVX_WITH_MERCURY
# knob. wasmtime headers are dlopen-only (no link dependency).
TARGET_DIR       := $(CURDIR)
TARGET_INC_DIR   := $(TARGET_DIR)/include
WASMTIME_INC_DIR := $(TARGET_DIR)/wasmtime/include

MERCURY_ROOT     ?= /home/kyle/src/rados-nkv/spdk/vendor/mercury-install
MERCURY_PKG_DIR  := $(MERCURY_ROOT)/lib/pkgconfig
MERCURY_CFLAGS   := $(shell PKG_CONFIG_PATH="$(MERCURY_PKG_DIR)" pkg-config --cflags mercury)
MERCURY_LIBS     := $(shell PKG_CONFIG_PATH="$(MERCURY_PKG_DIR)" pkg-config --libs mercury)

# ---- Compile / link flags ---------------------------------------------------
CC ?= cc

CFLAGS += -std=gnu11 -fPIC -g -Wall
CFLAGS += -DNKVX_WITH_MERCURY
CFLAGS += -I$(SPDK_HEADER_DIR)
CFLAGS += -I$(SPDK_BUILD_HEADER_DIR)
CFLAGS += -I$(TARGET_INC_DIR)
CFLAGS += -I$(TARGET_DIR)
CFLAGS += -I$(WASMTIME_INC_DIR)
CFLAGS += $(MERCURY_CFLAGS)

# Some entries in spdk_syslibs.pc name a library (e.g. -lvfio-user) without
# carrying its own -L path, so add the standard external-build search dirs
# (mirrors <spdk>/test/external_code/Makefile).
VFIO_LIB_DIR        ?= $(SPDK_ROOT)/build/libvfio-user/usr/local/lib
ISAL_LIB_DIR        ?= $(SPDK_ROOT)/isa-l/.libs
ISAL_CRYPTO_LIB_DIR ?= $(SPDK_ROOT)/isa-l-crypto/.libs

# Combined SPDK shared object + its DPDK and system library deps, plus our
# Mercury transport. wasmtime is intentionally NOT linked (dlopen at runtime).
LDFLAGS += -L$(SPDK_LIB_DIR) -Wl,-rpath=$(SPDK_LIB_DIR)
LDFLAGS += -L$(VFIO_LIB_DIR) -L$(ISAL_LIB_DIR) -L$(ISAL_CRYPTO_LIB_DIR)
LIBS    += -Wl,--no-as-needed -lspdk -Wl,--whole-archive -Wl,--no-whole-archive
LIBS    += $(DPDK_LIB)
LIBS    += $(MERCURY_LIBS)
LIBS    += $(SYS_LIB)
LIBS    += -ldl -lpthread

# ---- Module (front) sources -------------------------------------------------
MODULE_SRCS := \
	bdev_kvrados.c \
	bdev_kvrados_rpc.c \
	kvdev_rados_nkvx_front.c \
	nkvx_front_client.c \
	nkvx_exec_rpc.c

MODULE_OBJS := $(MODULE_SRCS:.c=.o)

APP_SRC  := nkv_tgt_main.c
APP_OBJ  := $(APP_SRC:.c=.o)
APP      := nkv_tgt

.PHONY: all clean
.DEFAULT_GOAL := all

# Load path: the module objects are linked directly into a custom nvmf target
# app. The module's SPDK_BDEV_MODULE_REGISTER / SPDK_RPC_REGISTER constructors
# self-register at load, so bdev_kvrados is usable over JSON-RPC immediately.
all: $(APP)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(APP): $(APP_OBJ) $(MODULE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(APP_OBJ) $(MODULE_OBJS) $(LIBS)

clean:
	rm -f $(APP) $(APP_OBJ) $(MODULE_OBJS)
