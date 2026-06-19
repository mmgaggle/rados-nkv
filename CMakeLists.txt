#  Copyright (C) 2026 IBM, Inc.
#
#  This is free software; you can redistribute it and/or modify it under the
#  terms of the GNU Lesser General Public License version 3, as published by
#  the Free Software Foundation.  See file COPYING.
#
# Superbuild for the ceph-gpu-initiated e2e demo.
#
# This does not compile anything itself — it orchestrates each submodule's own
# native build (SPDK ./configure+make, Ceph do_cmake+ninja, NIXL meson+ninja,
# rocm-xio/qemu, the weights venv) as ExternalProjects, wired in dependency
# order, and adds helper targets (vstart/stop, the rados-nkv KV target).
#
# Drive it through the top-level Makefile wrapper:
#     make init           # fetch all submodules
#     make build          # build everything, dependency-ordered
#     make build-spdk      # just one component
#     make vstart / stop   # throwaway Ceph cluster
#     make up / down       # SPDK NVMe-KV target (scripts/rados-nkv)
#
# Select a subset at configure time, e.g.:
#     make configure CMAKE_ARGS='-DWITH_QEMU=OFF -DWITH_CEPH=OFF'

cmake_minimum_required(VERSION 3.20)
project(ceph_gpu_initiated NONE)

include(ExternalProject)
include(ProcessorCount)
find_package(Git REQUIRED)

# ---- component toggles -----------------------------------------------------
option(WITH_CEPH     "Build the ceph submodule (vstart cluster + librados)" ON)
option(WITH_SPDK     "Build SPDK (the NVMe-KV-on-RADOS substrate)"          ON)
option(WITH_ROCM_XIO "Build rocm-xio (Flow A: GPU-initiated)"               ON)
option(WITH_QEMU     "Build qemu (Flow A: pci-mmio-bridge)"                 ON)
option(WITH_NIXL     "Build the NIXL RADOS_NKV backend (Flow B)"            ON)
option(WITH_WEIGHTS  "Set up the vllm-weights catalog (Flow B)"                    ON)
option(WITH_QEMU_MINIMAL "Use qemu-minimal (guest VM tooling, Flow A)"      ON)
option(WITH_CLIENTS  "Build clients/ (KV host/test harness, against spdk)"  ON)
option(WITH_RADOS_NKVX "Build rados-nkvx/ (standalone Exec executor)"       ON)

# ---- knobs -----------------------------------------------------------------
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
  set(NPROC 4)
endif()
set(JOBS "${NPROC}" CACHE STRING "Parallel build jobs for the native builds")

set(SPDK_CONFIGURE_OPTS "--with-rbd" CACHE STRING "Extra ./configure options for SPDK")
set(ROCM_XIO_PRESET     "release"    CACHE STRING "rocm-xio CMake preset to build")
set(CEPH_CMAKE_OPTS     "-DWITH_RBD=ON" CACHE STRING "Extra options passed to ceph do_cmake.sh")

set(REPO "${CMAKE_SOURCE_DIR}")

# Common ExternalProject options: never download/update (the submodules + the
# init-* targets own that); build in the submodule tree; never install.
# DOWNLOAD_COMMAND is a no-op (ExternalProject requires *some* source method,
# but the real checkout is the init-<name> target this EP DEPENDS on, which runs
# before any build step).
set(EP_COMMON
  DOWNLOAD_COMMAND ${CMAKE_COMMAND} -E true
  UPDATE_COMMAND ""
  INSTALL_COMMAND ""
  BUILD_IN_SOURCE 1
  BUILD_ALWAYS 1                       # let each native tool decide what's stale
  USES_TERMINAL_CONFIGURE 1
  USES_TERMINAL_BUILD 1)

# init-<name>: populate one submodule (top-level; nested submodules are pulled
# by the component's own configure step where needed).
function(add_init name dir)
  add_custom_target(init-${name}
    COMMAND ${GIT_EXECUTABLE} -C "${REPO}" submodule update --init -- ${dir}
    COMMENT "git submodule update --init ${dir}"
    USES_TERMINAL)
endfunction()

# build-<name>: friendly alias for the ExternalProject target <name>.
function(add_build_alias name)
  add_custom_target(build-${name})
  add_dependencies(build-${name} ${name})
endfunction()

set(BUILD_TARGETS "")
set(INIT_TARGETS "")

# ---- ceph: vstart cluster + librados (the rados kvdev backend links it) -----
if(WITH_CEPH)
  add_init(ceph ceph)
  ExternalProject_Add(ceph
    SOURCE_DIR "${REPO}/ceph"
    CONFIGURE_COMMAND ${GIT_EXECUTABLE} submodule update --init
              COMMAND ./do_cmake.sh ${CEPH_CMAKE_OPTS}
    BUILD_COMMAND ninja -C build vstart
    DEPENDS init-ceph
    ${EP_COMMON})
  add_build_alias(ceph)
  list(APPEND BUILD_TARGETS ceph)
  list(APPEND INIT_TARGETS init-ceph)
endif()

# ---- spdk: the NVMe-KV-on-RADOS substrate ----------------------------------
if(WITH_SPDK)
  add_init(spdk spdk)
  set(_spdk_deps init-spdk)
  if(WITH_CEPH)
    list(APPEND _spdk_deps ceph)       # build librados before SPDK --with-rbd
  endif()
  ExternalProject_Add(spdk
    SOURCE_DIR "${REPO}/spdk"
    CONFIGURE_COMMAND ${GIT_EXECUTABLE} submodule update --init
              COMMAND ./configure ${SPDK_CONFIGURE_OPTS}
    BUILD_COMMAND make -j${JOBS}
    DEPENDS ${_spdk_deps}
    ${EP_COMMON})
  add_build_alias(spdk)
  list(APPEND BUILD_TARGETS spdk)
  list(APPEND INIT_TARGETS init-spdk)
endif()

# ---- clients: KV host/test harness — builds against the spdk submodule ------
# In-repo (not a submodule), so no init target. The Makefiles default
# SPDK_ROOT_DIR to ../../spdk and the Rust crate defaults NKVX_SPDK_BUILD to
# ../../../spdk/build, so they build once spdk is built. gpu-native (hipcc/ROCm)
# is off by default — the superbuild builds the CPU clients only.
if(WITH_CLIENTS AND WITH_SPDK)
  ExternalProject_Add(clients
    SOURCE_DIR "${REPO}/clients"
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E true
    BUILD_COMMAND     make -C kv_shim -j${JOBS}
          COMMAND     make -C kv -j${JOBS}
          COMMAND     make -C kv_rados -j${JOBS}
          COMMAND     make -C kv/vfu_host -j${JOBS}
          COMMAND     cargo build --release --manifest-path rkv/Cargo.toml
    DEPENDS spdk
    ${EP_COMMON})
  add_build_alias(clients)
  list(APPEND BUILD_TARGETS clients)
endif()

# ---- rados-nkvx: standalone Exec executor (non-SPDK) -----------------------
# Links Mercury (from $SPDK_ROOT/vendor/mercury-install) + librados; reuses the
# Exec contract / wasm core / wasmtime headers from the spdk submodule's
# module/kvdev/rados (which stay in the fork as SPDK module code).
if(WITH_RADOS_NKVX AND WITH_SPDK)
  set(_nkvx_deps spdk)
  if(WITH_CEPH)
    list(APPEND _nkvx_deps ceph)         # links librados
  endif()
  ExternalProject_Add(rados-nkvx
    SOURCE_DIR "${REPO}/rados-nkvx"
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E true
    BUILD_COMMAND make -j${JOBS}
    DEPENDS ${_nkvx_deps}
    ${EP_COMMON})
  add_build_alias(rados-nkvx)
  list(APPEND BUILD_TARGETS rados-nkvx)
endif()

# ---- rocm-xio: GPU-initiated nvme-ep (Flow A) ------------------------------
if(WITH_ROCM_XIO)
  add_init(rocm-xio clients/rocm-xio)
  ExternalProject_Add(rocm-xio
    SOURCE_DIR "${REPO}/clients/rocm-xio"
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E echo "configuring rocm-xio (preset: ${ROCM_XIO_PRESET})"
              COMMAND cmake --preset ${ROCM_XIO_PRESET}
    BUILD_COMMAND cmake --build build --parallel ${JOBS}
    DEPENDS init-rocm-xio
    ${EP_COMMON})
  add_build_alias(rocm-xio)
  list(APPEND BUILD_TARGETS rocm-xio)
  list(APPEND INIT_TARGETS init-rocm-xio)
endif()

# ---- qemu: pci-mmio-bridge (Flow A) ----------------------------------------
if(WITH_QEMU)
  add_init(qemu qemu)
  ExternalProject_Add(qemu
    SOURCE_DIR "${REPO}/qemu"
    CONFIGURE_COMMAND ./configure --target-list=x86_64-softmmu --enable-kvm
    BUILD_COMMAND make -j${JOBS}
    DEPENDS init-qemu
    ${EP_COMMON})
  add_build_alias(qemu)
  list(APPEND BUILD_TARGETS qemu)
  list(APPEND INIT_TARGETS init-qemu)
endif()

# ---- nixl: RADOS_NKV backend (Flow B) — links the SPDK shim ----------------
if(WITH_NIXL)
  add_init(nixl nixl)
  set(_nixl_deps init-nixl)
  if(WITH_SPDK)
    list(APPEND _nixl_deps spdk)
  endif()
  ExternalProject_Add(nixl
    SOURCE_DIR "${REPO}/nixl"
    CONFIGURE_COMMAND meson setup build
              -Dspdk_root=${REPO}/spdk
              -Dspdk_kv_shim_dir=${REPO}/clients/kv_shim
              -Drados_nkv_build_test=true
    BUILD_COMMAND ninja -C build
    DEPENDS ${_nixl_deps}
    ${EP_COMMON})
  add_build_alias(nixl)
  list(APPEND BUILD_TARGETS nixl)
  list(APPEND INIT_TARGETS init-nixl)
endif()

# ---- vllm-weights: weights catalog, vendored in-tree (Flow B) --------------
if(WITH_WEIGHTS)
  set(_weights_deps "")
  set(_weights_build ".venv/bin/pip install -e .")
  if(WITH_SPDK)
    list(APPEND _weights_deps spdk)
    string(APPEND _weights_build " && SPDK_ROOT=${REPO}/spdk ./native/build.sh")
  endif()
  ExternalProject_Add(weights
    SOURCE_DIR "${REPO}/clients/vllm-weights"
    CONFIGURE_COMMAND python3 -m venv .venv
    BUILD_COMMAND sh -c "${_weights_build}"
    DEPENDS ${_weights_deps}
    ${EP_COMMON})
  add_build_alias(weights)
  list(APPEND BUILD_TARGETS weights)
endif()

# ---- qemu-minimal: guest VM tooling (no compile; scripts only) -------------
if(WITH_QEMU_MINIMAL)
  add_init(qemu-minimal qemu-minimal)
  list(APPEND INIT_TARGETS init-qemu-minimal)
endif()

# ---- aggregate targets: build / init ---------------------------------------
add_custom_target(build)
if(BUILD_TARGETS)
  add_dependencies(build ${BUILD_TARGETS})
endif()

add_custom_target(init)
if(INIT_TARGETS)
  add_dependencies(init ${INIT_TARGETS})
endif()

# ---- runtime helpers: vstart / stop / KV target ----------------------------
add_custom_target(vstart
  COMMAND sh -c "cd '${REPO}/ceph/build' && MON=\${MON:-1} OSD=\${OSD:-3} MGR=\${MGR:-1} ../src/vstart.sh -n -d --without-dashboard"
  USES_TERMINAL
  COMMENT "vstart a throwaway Ceph cluster (override with MON=/OSD=/MGR=)")

add_custom_target(stop
  COMMAND sh -c "cd '${REPO}/ceph/build' && ../src/stop.sh"
  USES_TERMINAL
  COMMENT "stop the vstart Ceph cluster")

if(WITH_CEPH)
  add_dependencies(vstart ceph)        # ensure the daemons are built first
endif()

add_custom_target(deps-ceph
  COMMAND sh -c "cd '${REPO}/ceph' && ./install-deps.sh"
  USES_TERMINAL
  COMMENT "install Ceph build dependencies (may require sudo)")

add_custom_target(up
  COMMAND "${REPO}/scripts/rados-nkv" up
  USES_TERMINAL
  COMMENT "bring up the SPDK NVMe-KV target (scripts/rados-nkv up)")

add_custom_target(down
  COMMAND "${REPO}/scripts/rados-nkv" down
  USES_TERMINAL
  COMMENT "tear down the SPDK NVMe-KV target")

add_custom_target(status
  COMMAND "${REPO}/scripts/rados-nkv" status
  USES_TERMINAL
  COMMENT "show the SPDK NVMe-KV target status")

# ---- guest VM (Flow A): build the image, launch it -------------------------
add_custom_target(vm
  COMMAND "${REPO}/provisioning/build-image.sh"
  USES_TERMINAL
  COMMENT "build the ROCm + rocm-xio guest image (cloud-init)")

# Proven bring-up: GPU passthrough + pci-mmio-bridge + emulated NVMe.
add_custom_target(vm-run
  COMMAND "${REPO}/provisioning/launch-bridge-vm.sh"
  USES_TERMINAL
  COMMENT "launch the guest (vendored run-qemu-pci-mmio-bridge)")

# Full GPU->SPDK->RADOS path: qemu-minimal run-vm with the SPDK vfio-user NVMe-KV
# controller (set VFIO_USERDEV / PCI_HOSTDEV / PCI_MMIO_BRIDGE in the env).
add_custom_target(vm-run-spdk
  COMMAND "${REPO}/provisioning/run-guest.sh"
  USES_TERMINAL
  COMMENT "launch the guest wired to the SPDK NVMe-KV target (run-vm)")

add_custom_target(vm-vfio-rules
  COMMAND "${REPO}/qemu-minimal/udev/install-vfio-rules"
  USES_TERMINAL
  COMMENT "install qemu-minimal's VFIO udev rules (may require sudo)")
