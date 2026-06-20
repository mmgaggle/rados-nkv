# rados-nkv.spec — the project-native NVMe-KV(+Exec)-over-RADOS datapath.
#
# Decisions recorded in bead spdk-jhk.14 (ADR). In one line: ONE SRPM -> three
# binary RPMs (rados-nkv / -libs / -devel); SPDK RPMs come from SPDK's OWN spec
# (see `make export-rpms`), never re-authored here; Mercury + wasmtime are
# BUNDLED because neither is a base-distro package.
#
# Built --build-in-place from the checkout (like SPDK's rpm.sh), so it assumes
# the CMake superbuild has already built SPDK in ./spdk (headers, the in-tree
# kvdev wasm core the executor compiles in, and the vendored Mercury under
# spdk/vendor/mercury-install). In the container that is exactly the builder
# stage's state (spdk-jhk.17).
#
# Version/release are injected by `make rpm` (-D version / -D release), mirroring
# spdk/rpmbuild/rpm.sh. They default here so `rpmbuild -ba` works standalone too.

%{!?version:  %define version 0.1.0}
%{!?release:  %define release 1}

# No debuginfo for now — matches spdk.spec; revisit once the build is stable.
%define debug_package %{nil}

# Absolute path to the checkout. We never rely on the %build CWD: `make rpm`
# passes --define "checkout <repo>" and every input path below is rooted at it.
# The fallback keeps standalone rpmbuild from silently using the wrong tree.
%{!?checkout: %define checkout %{_builddir}}

# Where the builder staged the bundled shared libs. Defaults follow from the
# checkout's vendored Mercury (scripts/vendor-mercury.sh) and the conventional
# wasmtime install; the container may override either.
%{!?mercury_libdir:  %define mercury_libdir %{checkout}/spdk/vendor/mercury-install/lib}
%{!?wasmtime_libdir: %define wasmtime_libdir /usr/local/lib}

# Host shim shared-library names (used in the build/install/files sections).
%global shim_soname libkv_host_shim.so
%global shim_ver    %{shim_soname}.%{version}

Name:           rados-nkv
Version:        %{version}
Release:        %{release}%{?dist}
Summary:        NVMe Key-Value (+Exec) over RADOS — datapath service
License:        BSD-3-Clause
URL:            https://github.com/mmgaggle/rados-nkv
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig
BuildRequires:  librados-devel
BuildRequires:  libfabric-devel
BuildRequires:  openssl-devel
# spdk-devel (from `make export-rpms`) provides the public SPDK headers the
# shim links against; the executor uses the in-tree spdk/ checkout directly.
BuildRequires:  systemd-rpm-macros
# patchelf strips the build-tree Mercury rpath (we resolve via ld.so.conf.d).
BuildRequires:  patchelf

# Runtime: the SPDK target (controller) is a sibling package with no soname for
# the auto-dep generator to find, so require it explicitly. librados / libfabric /
# openssl are pulled in automatically via the binaries' soname deps
# (librados.so.2, libfabric.so.1, libcrypto.so.3) — don't duplicate them here.
Requires:       spdk
# Mercury and wasmtime ride along in this package (ADR spdk-jhk.14 §6).
Provides:       bundled(mercury) = 2.4.1
Provides:       bundled(wasmtime)
%{?systemd_requires}

%description
The project-native datapath for NVMe Key-Value over RADOS: the rados-nkv front
(SPDK NVMe-KV target bring-up) and the rados-nkvx Exec executor (nkvx_service),
plus a single container entrypoint that selects the role via $NKV_ROLE and two
systemd units for the bare-metal path. Mercury (RPC) and wasmtime (Exec sandbox)
are bundled. Deployed primarily as a container by cephadm; this RPM is also the
bare-metal artifact.

%package libs
Summary:        Host NVMe-KV shim runtime library (kv_host_shim)
License:        BSD-3-Clause
Requires:       spdk
%description libs
libkv_host_shim — the in-process NVMe-KV host (initiator) shim that host
consumers (the NIXL RADOS_NKV backend, vllm-weights native, rkv) link at runtime
to attach to a rados-nkv target over VFIOUSER and issue Store/Retrieve/Exist/
Delete. No daemon, no Exec executor.

%package devel
Summary:        Headers to build host consumers against the KV shim
License:        BSD-3-Clause
Requires:       %{name}-libs = %{version}-%{release}
Requires:       spdk-devel
%description devel
The public C ABI (kv_host_shim.h) plus the rados-nkvx contract headers, for
building host consumers against libkv_host_shim.

%prep
# --build-in-place: nothing to unpack. Source0 is a placeholder (see rpm.sh).

%build
# Executor side: nkvx_service + nkvx_exec_client (links vendored Mercury,
# librados, dlopen'd wasmtime). Reuses the standalone rados-nkvx/Makefile.
# Built in the checkout (in-place); all paths absolute via %{checkout}.
make -C %{checkout}/rados-nkvx %{?_smp_mflags} \
     MERCURY_PREFIX=%{checkout}/spdk/vendor/mercury-install

# Host shim shared library (libkv_host_shim.so) — built by the kv_shim Makefile's
# `shared` target (PIC, linked against the installed SPDK nvme stack via
# spdk_nvme.pc). Lands at clients/nvme-kv/kv_shim/%{shim_ver}.
make -C %{checkout}/clients/nvme-kv/kv_shim shared SHIM_VERSION=%{version}

# Stage license/doc into the build CWD so %license can find them — but only if
# CWD isn't already the checkout (rpm 6 in-place builds there; -ef guards the
# copy-onto-self).
for f in COPYING LICENSING.md; do
  [ "%{checkout}/$f" -ef "./$f" ] || cp -f "%{checkout}/$f" .
done

%install
rm -rf %{buildroot}

# --- main: rados-nkv --------------------------------------------------------
install -D -m0755 %{checkout}/rados-nkvx/nkvx_service     %{buildroot}%{_bindir}/nkvx_service
install -D -m0755 %{checkout}/rados-nkvx/nkvx_exec_client %{buildroot}%{_bindir}/nkvx_exec_client
install -D -m0755 %{checkout}/scripts/rados-nkv           %{buildroot}%{_bindir}/rados-nkv
install -D -m0755 %{checkout}/packaging/rpm/rados-nkv-entrypoint.sh \
                  %{buildroot}%{_libexecdir}/%{name}/entrypoint

# systemd units (bare-metal path; skeletons owned by spdk-jhk.16)
install -D -m0644 %{checkout}/packaging/rpm/rados-nkv.service  %{buildroot}%{_unitdir}/rados-nkv.service
install -D -m0644 %{checkout}/packaging/rpm/rados-nkvx.service %{buildroot}%{_unitdir}/rados-nkvx.service

# memlock drop-in for the executor's verbs/irdma pinning
install -D -m0644 %{checkout}/rados-nkvx/deploy/99-nkvx-memlock.conf \
                  %{buildroot}%{_sysconfdir}/security/limits.d/99-nkvx-memlock.conf

# EnvironmentFile examples for the bare-metal units (optional; -prefixed in unit)
install -D -m0644 %{checkout}/packaging/rpm/rados-nkv.sysconfig \
                  %{buildroot}%{_sysconfdir}/sysconfig/rados-nkv
install -D -m0644 %{checkout}/packaging/rpm/rados-nkvx.sysconfig \
                  %{buildroot}%{_sysconfdir}/sysconfig/rados-nkvx

# bundled Mercury + wasmtime under a private dir on the default search path
install -d -m0755 %{buildroot}%{_libdir}/%{name}
install -m0755 %{mercury_libdir}/libmercury*.so*   %{buildroot}%{_libdir}/%{name}/
install -m0755 %{mercury_libdir}/libna*.so*        %{buildroot}%{_libdir}/%{name}/
install -m0755 %{mercury_libdir}/libmchecksum*.so* %{buildroot}%{_libdir}/%{name}/
install -m0755 %{wasmtime_libdir}/libwasmtime.so*  %{buildroot}%{_libdir}/%{name}/
# ld.so.conf.d entry so the daemons (and the dlopen'd wasmtime soname) resolve it
install -d -m0755 %{buildroot}%{_sysconfdir}/ld.so.conf.d
echo "%{_libdir}/%{name}" > %{buildroot}%{_sysconfdir}/ld.so.conf.d/%{name}.conf

# --- libs: rados-nkv-libs ---------------------------------------------------
install -D -m0755 %{checkout}/clients/nvme-kv/kv_shim/%{shim_ver} \
                  %{buildroot}%{_libdir}/%{shim_ver}
ln -sf %{shim_ver} %{buildroot}%{_libdir}/%{shim_soname}.0

# --- devel: rados-nkv-devel -------------------------------------------------
install -D -m0644 %{checkout}/clients/nvme-kv/kv_shim/kv_host_shim.h \
                  %{buildroot}%{_includedir}/%{name}/kv_host_shim.h
install -m0644 %{checkout}/rados-nkvx/nkvx_oid.h       %{buildroot}%{_includedir}/%{name}/
install -m0644 %{checkout}/rados-nkvx/nkvx_executor.h  %{buildroot}%{_includedir}/%{name}/
ln -sf %{shim_soname}.0 %{buildroot}%{_libdir}/%{shim_soname}

# --- strip build-tree rpaths --------------------------------------------------
# The rados-nkvx Makefile links Mercury with -Wl,-rpath=<vendor>/lib, and the
# vendored Mercury libs carry the same. That build-tree path can't ship; the
# bundle is resolved at runtime via %{_sysconfdir}/ld.so.conf.d/%{name}.conf.
patchelf --remove-rpath %{buildroot}%{_bindir}/nkvx_service
patchelf --remove-rpath %{buildroot}%{_bindir}/nkvx_exec_client
find %{buildroot}%{_libdir}/%{name} -type f -name '*.so*' \
     -exec patchelf --remove-rpath {} +

%files
%license COPYING LICENSING.md
%{_bindir}/nkvx_service
%{_bindir}/nkvx_exec_client
%{_bindir}/rados-nkv
%{_libexecdir}/%{name}/entrypoint
%{_unitdir}/rados-nkv.service
%{_unitdir}/rados-nkvx.service
%config(noreplace) %{_sysconfdir}/security/limits.d/99-nkvx-memlock.conf
%config(noreplace) %{_sysconfdir}/sysconfig/rados-nkv
%config(noreplace) %{_sysconfdir}/sysconfig/rados-nkvx
%dir %{_libdir}/%{name}
%{_libdir}/%{name}/*.so*
%{_sysconfdir}/ld.so.conf.d/%{name}.conf

%files libs
%license COPYING
%{_libdir}/%{shim_soname}.0
%{_libdir}/%{shim_ver}

%files devel
%dir %{_includedir}/%{name}
%{_includedir}/%{name}/*.h
%{_libdir}/%{shim_soname}

# Two units, two roles; neither enabled by default (cephadm owns deployment).
%post
%systemd_post rados-nkv.service rados-nkvx.service
/sbin/ldconfig

%preun
%systemd_preun rados-nkv.service rados-nkvx.service

%postun
%systemd_postun_with_restart rados-nkv.service rados-nkvx.service
/sbin/ldconfig

%post libs -p /sbin/ldconfig
%postun libs -p /sbin/ldconfig

%changelog
* Fri Jun 19 2026 Kyle Bader <kyle.bader@gmail.com> - 0.1.0-1
- Initial packaging per ADR spdk-jhk.14 (rados-nkv / -libs / -devel).
