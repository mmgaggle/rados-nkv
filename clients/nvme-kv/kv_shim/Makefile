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
