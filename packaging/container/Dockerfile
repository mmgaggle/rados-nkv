# rados-nkv — OUT-OF-TREE datapath image (Slice I, spdk-7sr.17.10).
#
# One image, two roles (entrypoint dispatches on $NKV_ROLE = nkv | nkvx):
#   nkv   the front/target: the custom `nkv_tgt` app loads the OUT-OF-TREE
#         bdev_kvrados forwarder module, then `rados-nkv up` wires the KV ns.
#   nkvx  the executor: nkvx_service (Mercury Exec target; mem or RADOS backend).
#
# This REPLACES the former in-tree build (which compiled the whole SPDK fork and
# rpmbuilt spdk*/rados-nkv* RPMs). The KV forwarder is no longer baked into SPDK:
# SPDK is an UNMODIFIED, PRE-BUILT base (commit 45359c5916 = Gerrit 28298 KV
# series + memory_domain + the 28868 vfio-user PRP fix), and the forwarder +
# executor build OUT-OF-TREE against it (target/Makefile, rados-nkvx/Makefile).
#
# Because the base SPDK is a LOCAL commit (not on any remote) and its shared
# objects/RUNPATHs bake in absolute host paths, the build context STAGES the
# prebuilt SPDK tree, DPDK, and vendored Mercury at their ORIGINAL absolute
# paths, and this image places them back at those same paths so every RUNPATH
# and the libspdk.so GROUP linker script resolve unchanged. build-clean.sh stages
# the context; see its header for what lands where.
#
#   packaging/container/build-clean.sh        # stage context + build the image
#
# Layout inside the staged context (mirrors host absolute paths):
#   spdk-base/   -> the prebuilt SPDK X tree            (= /home/kyle/src/rados-nkv-wt/slice-a/spdk)
#   dpdk/        -> the DPDK build the base links        (= /home/kyle/src/rados-nkv/spdk/dpdk/build)
#   mercury/     -> the vendored Mercury install         (= /home/kyle/src/rados-nkv/spdk/vendor/mercury-install)
#   src/         -> this repo's out-of-tree sources (target/, rados-nkvx/, scripts/, packaging/)

ARG FEDORA_VERSION=44

# Fixed in-container paths == the host paths the prebuilt SPDK's RUNPATHs encode.
ARG SPDK_BASE_DIR=/home/kyle/src/rados-nkv-wt/slice-a/spdk
ARG DPDK_DIR=/home/kyle/src/rados-nkv/spdk/dpdk/build
ARG MERCURY_DIR=/home/kyle/src/rados-nkv/spdk/vendor/mercury-install
ARG WASMTIME_VERSION=36.0.2

# ---------------------------------------------------------------------------
FROM fedora:${FEDORA_VERSION} AS builder
ARG SPDK_BASE_DIR
ARG DPDK_DIR
ARG MERCURY_DIR
ARG WASMTIME_VERSION

# Toolchain + the link-time devel packages the out-of-tree binaries need.
# librados-devel: the executor links -lrados (cold-fill + real KV). The rest are
# the SPDK syslibs the combined libspdk.so pulls (json-c/uuid/openssl/lz4/fuse3/
# keyutils/numa/aio/libfabric/isa-l deps).
RUN dnf install -y \
        gcc gcc-c++ make cmake git findutils which xz \
        python3 python3-pyelftools \
        libaio-devel numactl-devel openssl-devel \
        libuuid-devel zlib-devel json-c-devel \
        lz4-devel fuse3-devel keyutils-libs-devel \
        libfabric-devel librados-devel \
        CUnit libfdt \
    && dnf clean all

# wasmtime C-API shared lib (dlopen'd by the Exec path; soname libwasmtime.so).
RUN curl -fsSL --retry 6 --retry-delay 5 --retry-all-errors -o /tmp/wasmtime.tar.xz \
      "https://github.com/bytecodealliance/wasmtime/releases/download/v${WASMTIME_VERSION}/wasmtime-v${WASMTIME_VERSION}-x86_64-linux-c-api.tar.xz" \
    && tar -C /tmp -xf /tmp/wasmtime.tar.xz \
    && install -D -m0755 /tmp/wasmtime-v${WASMTIME_VERSION}-x86_64-linux-c-api/lib/libwasmtime.so \
         /usr/local/lib/libwasmtime.so \
    && ldconfig && rm -rf /tmp/wasmtime*

# Place the prebuilt SPDK base, DPDK, and Mercury at their ORIGINAL absolute
# paths so RUNPATHs / the libspdk.so GROUP script resolve with no patching.
COPY spdk-base ${SPDK_BASE_DIR}
COPY dpdk      ${DPDK_DIR}
COPY mercury   ${MERCURY_DIR}

# The out-of-tree repo sources (target/, rados-nkvx/, scripts/, packaging/, …).
WORKDIR /src
COPY src /src

# Build the forwarder + custom nkv_tgt app (out-of-tree against the base SPDK).
RUN make -C target \
        SPDK_ROOT=${SPDK_BASE_DIR} \
        MERCURY_ROOT=${MERCURY_DIR} \
    && test -x target/nkv_tgt

# Build the standalone rados-nkvx executor (links Mercury + the system librados).
RUN make -C rados-nkvx \
        SPDK_ROOT=${SPDK_BASE_DIR} \
        MERCURY_PREFIX=${MERCURY_DIR} \
    && test -x rados-nkvx/nkvx_service \
    && test -x rados-nkvx/nkvx_exec_client

# ---------------------------------------------------------------------------
FROM fedora:${FEDORA_VERSION} AS runtime
ARG SPDK_BASE_DIR
ARG DPDK_DIR
ARG MERCURY_DIR
LABEL org.opencontainers.image.title="rados-nkv" \
      org.opencontainers.image.description="NVMe Key-Value (+Exec) over RADOS — out-of-tree datapath (nkv/nkvx)" \
      org.opencontainers.image.source="https://github.com/mmgaggle/rados-nkv"

# Runtime shared-lib deps (the binaries link these dynamically): librados2 for
# the executor, libfabric for Mercury's ofi providers, and the SPDK syslibs.
# python3 + pyelftools for rpc.py (the bring-up calls scripts/rpc.py).
RUN dnf install -y \
        python3 python3-pyelftools \
        librados2 libfabric \
        numactl-libs libaio json-c libuuid openssl-libs \
        lz4-libs fuse3-libs keyutils-libs zlib \
        CUnit libfdt \
    && dnf clean all

# SPDK base tree (shared libs, headers, scripts/rpc.py, python pkg) + DPDK +
# Mercury at their original absolute paths so RUNPATHs resolve at run time.
COPY --from=builder ${SPDK_BASE_DIR} ${SPDK_BASE_DIR}
COPY --from=builder ${DPDK_DIR}      ${DPDK_DIR}
COPY --from=builder ${MERCURY_DIR}   ${MERCURY_DIR}
COPY --from=builder /usr/local/lib/libwasmtime.so /usr/local/lib/libwasmtime.so

# The out-of-tree binaries.
COPY --from=builder /src/target/nkv_tgt               /usr/local/bin/nkv_tgt
COPY --from=builder /src/rados-nkvx/nkvx_service      /usr/local/bin/nkvx_service
COPY --from=builder /src/rados-nkvx/nkvx_exec_client  /usr/local/bin/nkvx_exec_client

# Bring-up wrapper + the out-of-tree rpc.py plugin + the entrypoint.
COPY --from=builder /src/scripts/rados-nkv             /usr/local/bin/rados-nkv
COPY --from=builder /src/scripts/rpc_plugin_kvrados.py /usr/local/bin/rpc_plugin_kvrados.py
COPY --from=builder /src/packaging/rpm/rados-nkv-entrypoint.sh \
                    /usr/libexec/rados-nkv/entrypoint

# Put every dir holding a shared lib the binaries need on the default loader
# search path so nkv_tgt / nkvx_service resolve them without LD_LIBRARY_PATH:
#   - the SPDK combined libs (build/lib)            — libspdk*.so
#   - DPDK (external --with-dpdk, not on RUNPATH)   — librte_*.so.26
#   - vendored Mercury (private prefix)             — libmercury/na/...so.*
#   - isa-l / isa-l-crypto (.libs)                  — libisal*.so.2
#   - libvfio-user shared object                    — libvfio-user.so.0
#   - /usr/local/lib                                — libwasmtime.so
RUN printf '%s\n' \
        "${SPDK_BASE_DIR}/build/lib" \
        "${DPDK_DIR}/lib" \
        "${MERCURY_DIR}/lib" \
        "${SPDK_BASE_DIR}/isa-l/.libs" \
        "${SPDK_BASE_DIR}/isa-l-crypto/.libs" \
        "${SPDK_BASE_DIR}/build/libvfio-user/usr/local/lib" \
        /usr/local/lib \
        > /etc/ld.so.conf.d/rados-nkv.conf \
    && ldconfig

# The bring-up script + entrypoint find rpc.py and the spdk python pkg here.
ENV SPDK_ROOT=${SPDK_BASE_DIR}
ENV NVMF_TGT=/usr/local/bin/nkv_tgt
ENV PATH=/usr/local/bin:${PATH}

RUN chmod 0755 /usr/local/bin/rados-nkv /usr/libexec/rados-nkv/entrypoint

# Role is supplied at run time: NKV_ROLE=nkv (front/target) | nkvx (executor).
ENTRYPOINT ["/usr/libexec/rados-nkv/entrypoint"]
