#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../spdk)

APP = kv_shim_test

SPDK_LIB_LIST = nvme

# The app is built from both the test driver and the reusable shim. nvme.libtest.mk
# derives C_SRCS from APP (a single .c); expand it here so the shim is compiled
# and linked into the same app.
NVME_DIR := $(SPDK_ROOT_DIR)/lib/nvme

C_SRCS := kv_shim_test.c kv_host_shim.c

include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

SPDK_LIB_LIST += $(SOCK_MODULES_LIST) nvme vmd

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk

# ---- Shared library form (rados-nkv-libs RPM; bead spdk-jhk.19) -------------
# libkv_host_shim.so is what downstream consumers (the NIXL RADOS_NKV backend,
# vllm-weights native, rkv) link at runtime to attach to a rados-nkv target.
# kv_host_shim.c also builds INTO kv_shim_test above (via C_SRCS); this target
# additionally produces a PIC shared object linked against the installed SPDK
# nvme stack. The link list is whatever spdk_nvme.pc pulls in (validated:
# spdk_nvme keyring sock sock_posix trace rpc jsonrpc json util dma vfio_user log).
SHIM_VERSION  ?= 0.1.0
SHIM_SOMAJOR  := 0
SHIM_SONAME   := libkv_host_shim.so
SHIM_PKGCONF  := $(SPDK_ROOT_DIR)/build/lib/pkgconfig

$(SHIM_SONAME).$(SHIM_VERSION): kv_host_shim.c kv_host_shim.h
	$(CC) -shared -fPIC -Wl,-soname,$(SHIM_SONAME).$(SHIM_SOMAJOR) \
	  -I$(SPDK_ROOT_DIR)/include -o $@ kv_host_shim.c \
	  $$(PKG_CONFIG_PATH=$(SHIM_PKGCONF) pkg-config --cflags --libs spdk_nvme)

.PHONY: shared
shared: $(SHIM_SONAME).$(SHIM_VERSION)
