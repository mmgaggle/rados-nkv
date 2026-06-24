#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 rados-nkv contributors. All rights reserved.
#
# Out-of-tree CUnit build for kvrados_ut, the bdev_kvrados forwarder unit test.
# Driven by the top-level target/test/Makefile, which passes SPDK_ROOT_DIR (an
# UNMODIFIED, pre-built SPDK tree carrying mk/spdk.unittest.mk + the CUnit
# harness) and TARGET_DIR (this repo's out-of-tree module). Mirrors the original
# in-tree test/unit/lib/bdev/kvrados.c/Makefile, but the module under test now
# lives at $(TARGET_DIR)/bdev_kvrados.c instead of bdev/kvrados/bdev_kvrados.c.
#
# kvrados_ut.c #includes bdev_kvrados.c directly (the SPDK unit-test idiom), so
# we add -I$(TARGET_DIR) for that quoted include and -I$(TARGET_DIR)/include for
# our out-of-tree spdk/kvdev.h (absent from unmodified SPDK). No NKVX_WITH_MERCURY:
# like the in-tree UT, the module compiles its deps-disabled path here.

TARGET_DIR ?= $(abspath $(CURDIR)/..)

TEST_FILE = kvrados_ut.c

CFLAGS += -I$(TARGET_DIR)
CFLAGS += -I$(TARGET_DIR)/include

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
