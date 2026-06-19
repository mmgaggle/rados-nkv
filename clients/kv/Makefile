#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../spdk)

# This directory builds more than one standalone KV host: kv_host (round-trip /
# KV Exec / List tests) and kv_ro_host (read-only namespace enforcement test,
# ADR-0008). mk/nvme.libtest.mk only builds a single APP, so when APP is unset
# we recurse once per host with APP set; the recursive invocations then fall
# through to the real build below.
KV_APPS = kv_host kv_ro_host

ifndef APP

.PHONY: all clean
all clean:
	$(Q)for app in $(KV_APPS); do \
		$(MAKE) -f $(CURDIR)/Makefile APP=$$app $@ || exit 1; \
	done

else

SPDK_LIB_LIST = nvme

include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk

endif
