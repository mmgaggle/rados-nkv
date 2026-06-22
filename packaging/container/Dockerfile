# rados-nkv — multi-stage container image (ADR bead spdk-jhk.14 §4, spdk-jhk.17).
#
# One image, two roles: the entrypoint dispatches on $NKV_ROLE (nkv | nkvx).
# Stage `builder` reproduces the validated host build — vendor the (patched)
# Mercury, fetch wasmtime, build SPDK + the rados-nkvx executor, then rpmbuild
# the spdk* and rados-nkv* RPMs (the ceph-nvmeof export-rpms pattern). Stage
# `runtime` is a thin image that just dnf-installs those RPMs.
#
# Build context MUST contain the spdk submodule populated recursively, e.g.:
#   git submodule update --init --recursive spdk
#   podman build -f packaging/container/Dockerfile -t rados-nkv:devel_v0.1.0 .
# (Ceph is NOT built — SPDK --with-rbd links the system librados-devel.)

ARG FEDORA_VERSION=44

# ---------------------------------------------------------------------------
FROM fedora:${FEDORA_VERSION} AS builder
ARG WASMTIME_VERSION=36.0.2

# SPDK + Mercury + rados-nkvx build deps (librados/librbd come from the distro;
# we do NOT build Ceph). rpm-build + patchelf are for the spec.
RUN dnf install -y \
        gcc gcc-c++ make cmake git patch xz findutils which \
        autoconf automake libtool help2man \
        rpm-build patchelf \
        python3-devel python3-pyelftools python3-jinja2 python3-tabulate \
        uv procps-ng \
        meson ninja-build \
        libaio-devel numactl-devel ncurses-devel openssl-devel \
        libuuid-devel zlib-devel CUnit-devel \
        json-c-devel libcmocka-devel \
        fuse3-devel keyutils-libs-devel \
        libfabric-devel boost-devel \
        librados-devel librbd-devel \
        nasm \
    && dnf clean all

WORKDIR /src
COPY . /src

# wasmtime C-API shared lib (dlopen'd by the Exec path; soname libwasmtime.so).
# Pinned to the version of the vendored wasmtime.h in the spdk submodule.
RUN curl -fsSL --retry 6 --retry-delay 5 --retry-all-errors -o /tmp/wasmtime.tar.xz \
      "https://github.com/bytecodealliance/wasmtime/releases/download/v${WASMTIME_VERSION}/wasmtime-v${WASMTIME_VERSION}-x86_64-linux-c-api.tar.xz" \
    && tar -C /tmp -xf /tmp/wasmtime.tar.xz \
    && install -D -m0755 /tmp/wasmtime-v${WASMTIME_VERSION}-x86_64-linux-c-api/lib/libwasmtime.so \
         /usr/local/lib/libwasmtime.so \
    && ldconfig

# Vendor the patched Mercury (vendor-mercury.sh git-applies the dma-buf patch).
# Invoked via bash so the build does not depend on the script's exec bit
# surviving the build context COPY (it ships 0600 from the spdk submodule).
RUN bash spdk/scripts/vendor-mercury.sh

# Build SPDK in-place (flags must match `make export-rpms` SPDK_RPM_CONFIGURE).
RUN cd spdk \
    && ./configure --with-rbd --with-vfio-user \
         --with-mercury=/src/spdk/vendor/mercury-install \
    && make -j"$(nproc)"

# Build the standalone rados-nkvx executor (links the vendored Mercury).
RUN make -C rados-nkvx -j"$(nproc)" \
        MERCURY_PREFIX=/src/spdk/vendor/mercury-install

# Produce the RPMs: SPDK's own spec (export-rpms) + our rados-nkv spec.
# SPDK's rpm.sh derives its version from `git describe`, which resolves to
# whatever tag is nearest in the submodule history rather than the real release.
# Pin it deterministically from the committed version.h via SPDK_VERSION (honored
# by rpm.sh: version=${SPDK_VERSION:-$(get_version)}).
RUN SPDK_VERSION="$(awk '$1=="#define" && $2=="SPDK_VERSION_MAJOR"{a=$3} $1=="#define" && $2=="SPDK_VERSION_MINOR"{b=$3} $1=="#define" && $2=="SPDK_VERSION_PATCH"{c=$3} END{print a"."b"."c}' spdk/include/spdk/version.h)" \
    && echo "Using SPDK_VERSION=$SPDK_VERSION" \
    && export SPDK_VERSION \
    && make export-rpms && make rpm \
    && mkdir -p /rpms \
    && cp -v /root/rpmbuild/rpm/x86_64/spdk*.rpm /rpms/ \
    && cp -v /root/rpmbuild/RPMS/x86_64/rados-nkv*.rpm /rpms/

# ---------------------------------------------------------------------------
FROM fedora:${FEDORA_VERSION} AS runtime
LABEL org.opencontainers.image.title="rados-nkv" \
      org.opencontainers.image.description="NVMe Key-Value (+Exec) over RADOS — datapath (nkv/nkvx)" \
      org.opencontainers.image.source="https://github.com/mmgaggle/rados-nkv"

COPY --from=builder /rpms /tmp/rpms
# dnf resolves the runtime deps of the RPMs (librados2, librbd1, libfabric,
# openssl-libs, numactl-libs, …) from the distro.
RUN dnf install -y /tmp/rpms/spdk-*.rpm /tmp/rpms/rados-nkv-*.rpm \
    && dnf clean all && rm -rf /tmp/rpms && ldconfig

# Role is supplied at run time: NKV_ROLE=nkv (front/target) | nkvx (executor).
# cephadm / compose set it per daemon; there is no safe default.
ENTRYPOINT ["/usr/libexec/rados-nkv/entrypoint"]
