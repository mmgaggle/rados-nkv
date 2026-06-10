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

COMPONENTS := ceph spdk rocm-xio qemu nixl weights

# Targets handled by the CMake superbuild, forwarded verbatim as build targets.
FORWARD := build init vstart stop up down status deps-ceph \
           vm vm-run vm-run-spdk vm-vfio-rules init-qemu-minimal \
           $(COMPONENTS) \
           $(addprefix build-,$(COMPONENTS)) \
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
	@echo '  make distclean           remove $(BUILD_DIR)/'
	@echo
	@echo '  components: $(COMPONENTS)'
	@echo "  subset:     make configure CMAKE_ARGS='-DWITH_QEMU=OFF' && make build"
