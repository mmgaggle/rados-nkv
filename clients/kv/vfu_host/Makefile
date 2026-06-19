#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../spdk)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

APP = nkv_vfu_host
C_SRCS = nkv_vfu_host.c

# Option A: drive the vfio-user NVMe-KV controller directly via SPDK's raw
# host vfio-user client (lib/vfio_user), NOT the lib/nvme initiator.
SPDK_LIB_LIST = vfio_user

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
