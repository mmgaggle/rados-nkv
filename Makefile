#  Copyright (C) 2026 IBM, Inc.
#
#  This is free software; you can redistribute it and/or modify it under the
#  terms of the GNU Lesser General Public License version 3, as published by
#  the Free Software Foundation.  See file COPYING.
#
# Top-level convenience wrapper around the CMake superbuild (see CMakeLists.txt).
#
#   make init            # fetch all submodules
#   make build           # build everything, dependency-ordered
#   make build-spdk      # build one component (ceph spdk rocm-xio qemu nixl weights)
#   make init-spdk       # init one submodule
#   make vstart / stop   # throwaway Ceph cluster
#   make up / down / status   # SPDK NVMe-KV target (scripts/rados-nkv)
#   make distclean       # remove the build/ directory
#
# Build a subset by passing component toggles at configure time:
#   make configure CMAKE_ARGS='-DWITH_QEMU=OFF -DWITH_CEPH=OFF'
#   make build
# (run `make distclean` first if you're changing toggles on an existing build/).

BUILD_DIR  ?= build
CMAKE      ?= cmake
CMAKE_ARGS ?=

COMPONENTS := ceph spdk rocm-xio qemu nixl

# Targets handled by the CMake superbuild, forwarded verbatim as build targets.
FORWARD := build init vstart stop up down status deps-ceph \
           vm vm-run vm-run-spdk vm-vfio-rules init-qemu-minimal \
           $(COMPONENTS) weights \
           $(addprefix build-,$(COMPONENTS)) build-weights \
           $(addprefix init-,$(COMPONENTS))

.DEFAULT_GOAL := help

# One-time configure (regenerated if CMakeLists.txt changes).
$(BUILD_DIR)/CMakeCache.txt: CMakeLists.txt
	$(CMAKE) -B $(BUILD_DIR) -S . $(CMAKE_ARGS)

.PHONY: configure
configure: $(BUILD_DIR)/CMakeCache.txt

.PHONY: $(FORWARD)
$(FORWARD): | $(BUILD_DIR)/CMakeCache.txt
	$(CMAKE) --build $(BUILD_DIR) --target $@

# ---- Test aggregation (independent of the CMake superbuild) ----------------
# `make check` runs the fast, hermetic unit suites — no live nkvx_service, no
# Mercury/librados, no rig:
#   - check-c       rados-nkvx pure-C ADR-0014 contract helpers (nkvx_oid.h)
#   - check-rust    rkv (Rust) `cargo test`
#   - check-python  vllm-weights (Python) pytest
# Each is a standalone target so a missing toolchain only blocks that one suite;
# `check` runs all three and fails if any fails. The I/O-bound end-to-end tests,
# which need a built executor + Mercury and a running service, are opt-in under
# `make check-integration`.
.PHONY: check check-c check-rust check-python check-integration

check: check-c check-rust check-python
	@echo '=== all unit suites passed ==='

check-c:
	@echo '=== rados-nkvx C unit tests ==='
	$(MAKE) -C rados-nkvx test

check-rust:
	@echo '=== rkv (Rust) unit tests ==='
	cd clients/rkv && cargo test

check-python:
	@echo '=== vllm-weights (Python) unit tests ==='
	cd clients/vllm-weights && python3 -m pytest

# End-to-end / loopback suites. These need the standalone executor built against
# Mercury (see rados-nkvx/Makefile) and, for the live paths, librados + a running
# service — so they are NOT part of `make check`.
check-integration:
	@echo '=== rados-nkvx end-to-end (Mercury + nkvx_service required) ==='
	$(MAKE) -C rados-nkvx run

# ---- Packaging (RPMs + the in-image datapath spec) -------------------------
# Modeled on ceph-nvmeof (ADR: bead spdk-jhk.14). Two distinct steps:
#   export-rpms : SPDK's spdk*/spdk-devel/spdk-scripts RPMs, produced by SPDK's
#                 OWN spec (spdk/rpmbuild/rpm.sh) — we author zero SPDK spec.
#   rpm         : our rados-nkv / -libs / -devel, built --build-in-place from
#                 this checkout (needs a built ./spdk: headers + vendored Mercury).
# Both land under $(RPM_TOPDIR) (default ~/rpmbuild). `rpms` does both.
VERSION       := $(shell cat $(CURDIR)/VERSION)
RPM_RELEASE   ?= 1
RPM_TOPDIR    ?= $(HOME)/rpmbuild
# SPDK configure flags must match the superbuild's SPDK build. --with-mercury
# needs the vendored prefix spelled out (rpmbuild scrubs PKG_CONFIG_PATH).
SPDK_RPM_CONFIGURE ?= --with-rbd --with-vfio-user \
                      --with-mercury=$(CURDIR)/spdk/vendor/mercury-install
# DEPS=no skips SPDK's root-requiring `yum install` of build deps — they're
# already present (the superbuild built SPDK; in the container an earlier layer
# installs them). Set DEPS=yes to let rpm.sh install them (needs root).
SPDK_RPM_DEPS ?= no

.PHONY: export-rpms rpm srpm rpms
export-rpms:
	@echo '=== SPDK RPMs via spdk/rpmbuild/rpm.sh ($(SPDK_RPM_CONFIGURE)) ==='
	BUILDDIR=$(RPM_TOPDIR) DEPS=$(SPDK_RPM_DEPS) \
	  $(CURDIR)/spdk/rpmbuild/rpm.sh $(SPDK_RPM_CONFIGURE)

rpm:
	@echo '=== rados-nkv RPMs (version $(VERSION), release $(RPM_RELEASE)) ==='
	rpmbuild --build-in-place -bb \
	  --define '_topdir $(RPM_TOPDIR)' \
	  --define 'checkout $(CURDIR)' \
	  --define 'version $(VERSION)' \
	  --define 'release $(RPM_RELEASE)' \
	  $(CURDIR)/packaging/rpm/rados-nkv.spec

srpm:
	rpmbuild --build-in-place -bs \
	  --define '_topdir $(RPM_TOPDIR)' \
	  --define 'checkout $(CURDIR)' \
	  --define 'version $(VERSION)' \
	  --define 'release $(RPM_RELEASE)' \
	  $(CURDIR)/packaging/rpm/rados-nkv.spec

rpms: export-rpms rpm

.PHONY: distclean
distclean:
	rm -rf $(BUILD_DIR)

.PHONY: help
help:
	@echo 'ceph-gpu-initiated — build orchestration'
	@echo
	@echo '  make init                fetch all submodules'
	@echo '  make build               build everything (dependency-ordered)'
	@echo '  make build-<component>    build one  (e.g. make build-spdk)'
	@echo '  make init-<component>     init one submodule'
	@echo '  make vstart | stop       throwaway Ceph cluster (MON=/OSD=/MGR= override)'
	@echo '  make up | down | status  SPDK NVMe-KV target (scripts/rados-nkv)'
	@echo '  make vm                  build the ROCm + rocm-xio guest image'
	@echo '  make vm-run              launch guest (proven pci-mmio-bridge bring-up)'
	@echo '  make vm-run-spdk         launch guest wired to the SPDK NVMe-KV target'
	@echo '  make vm-vfio-rules       install VFIO udev rules (may need sudo)'
	@echo '  make deps-ceph           install Ceph build deps (may need sudo)'
	@echo '  make export-rpms         SPDK RPMs via SPDKs own spec (spdk/rpmbuild/rpm.sh)'
	@echo '  make rpm | rpms          rados-nkv RPMs (rpms = export-rpms + rpm)'
	@echo '  make distclean           remove $(BUILD_DIR)/'
	@echo
	@echo '  components: $(COMPONENTS)'
	@echo "  subset:     make configure CMAKE_ARGS='-DWITH_QEMU=OFF' && make build"
