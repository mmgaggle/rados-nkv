// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM Corporation. All rights reserved.
//! Build script for rados-nkv (bead spdk-jhk.7.1).
//!
//! 1. Compiles csrc/nkvx_shim.c (which includes the modified ../../vfu_host/
//!    nkv_vfu.h driver) via the `cc` crate with the SPDK include paths.
//! 2. Emits cargo:rustc-link-arg lines reproducing build_gpu.sh's static-lib
//!    link recipe (lines 23-37) EXACTLY, but WITHOUT -lamdhip64 (no HIP in the
//!    CPU CLI build).
//!
//! Worktrees do NOT carry build/ outputs, so the prebuilt SPDK artifacts are
//! located via the NKVX_SPDK_BUILD env var, defaulting to the integration
//! worktree's build dir. The SPDK root is <build>/.. (matches build_gpu.sh's
//! $WT layout: include/, build/, dpdk/build/lib/, isa-l/.libs/ all under root).

use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-changed=csrc/nkvx_shim.c");
    println!("cargo:rerun-if-changed=csrc/nkvx_shim.h");
    println!("cargo:rerun-if-changed=csrc/nkvx_gpu.hip");
    println!("cargo:rerun-if-changed=csrc/nkvx_gpu.h");
    println!("cargo:rerun-if-changed=../vfu_host/nkv_vfu.h");
    println!("cargo:rerun-if-env-changed=NKVX_SPDK_BUILD");
    println!("cargo:rerun-if-env-changed=NKVX_HIPCC");

    // The `gpu-native` cargo feature compiles + links the in-process native HIP
    // datapath (csrc/nkvx_gpu.hip). When OFF (default), nothing here changes:
    // no HIP, no ROCm, and `--gpu` delegates to the nkv_vfu_gpu subprocess.
    let gpu_native = std::env::var_os("CARGO_FEATURE_GPU_NATIVE").is_some();

    // <build> = NKVX_SPDK_BUILD or, by default, the spdk submodule's build dir.
    // This crate lives at <umbrella>/clients/kv/rados-nkv, so the submodule build
    // is three levels up + spdk/build.
    let build = PathBuf::from(std::env::var("NKVX_SPDK_BUILD").unwrap_or_else(|_| {
        format!("{}/../../../spdk/build", env!("CARGO_MANIFEST_DIR"))
    }));
    // SPDK root = <build>/.. (build_gpu.sh's $WT).
    let root = build
        .parent()
        .expect("NKVX_SPDK_BUILD must have a parent (SPDK root)")
        .to_path_buf();

    let dpdk_lib = root.join("dpdk/build/lib");
    let vfio_user_inc = build.join("libvfio-user/usr/local/include");
    let vfio_user_lib = build.join("libvfio-user/usr/local/lib");

    // isa-l include dirs (mirrors build_gpu.sh:14-16 INC for isa-l).
    let isal_inc = root.join("isa-l/..");
    let isalbuild_inc = root.join("isalbuild");
    let isalcrypto_inc = root.join("isa-l-crypto/..");
    let isalcryptobuild_inc = root.join("isalcryptobuild");

    let isal_a = root.join("isa-l/.libs/libisal.a");
    let isalcrypto_a = root.join("isa-l-crypto/.libs/libisal_crypto.a");
    let env_dpdk_a = build.join("lib/libspdk_env_dpdk.a");

    // --- 1. Compile the CPU shim ---
    // When gpu-native is on, the HIP TU owns the single nvfu_produce symbol, so
    // compile the CPU shim's copy out via -DNKVX_GPU_NATIVE to avoid a duplicate.
    let mut shim = cc::Build::new();
    shim.file("csrc/nkvx_shim.c")
        .include(root.join("include"))
        .include(&vfio_user_inc)
        .include(&isal_inc)
        .include(&isalbuild_inc)
        .include(&isalcrypto_inc)
        .include(&isalcryptobuild_inc)
        .define("_GNU_SOURCE", None)
        .flag_if_supported("-Wno-array-bounds");
    if gpu_native {
        shim.define("NKVX_GPU_NATIVE", None);
    }
    shim.compile("nkvx_shim");

    // --- 1b. (gpu-native only) Compile the HIP GPU datapath with hipcc and
    // archive it into a static lib for rustc to link. Mirrors build_gpu.sh's
    // hipcc compile line (-x hip -std=c++17 -D_GNU_SOURCE) plus -DNKVX_GPU_NATIVE
    // and the same SPDK/isa-l include set as the CPU shim. ---
    if gpu_native {
        let out_dir = PathBuf::from(std::env::var("OUT_DIR").expect("OUT_DIR"));
        let hipcc = std::env::var("NKVX_HIPCC").unwrap_or_else(|_| "hipcc".to_string());
        let obj = out_dir.join("nkvx_gpu.o");
        let incs = [
            root.join("include"),
            vfio_user_inc.clone(),
            isal_inc.clone(),
            isalbuild_inc.clone(),
            isalcrypto_inc.clone(),
            isalcryptobuild_inc.clone(),
        ];
        let mut cc_cmd = Command::new(&hipcc);
        cc_cmd
            .arg("-x")
            .arg("hip")
            .arg("-std=c++17")
            .arg("-D_GNU_SOURCE")
            .arg("-DNKVX_GPU_NATIVE")
            .arg("-Wno-array-bounds")
            .arg("-fPIC")
            .arg("-c")
            .arg("csrc/nkvx_gpu.hip")
            .arg("-o")
            .arg(&obj);
        for inc in &incs {
            cc_cmd.arg(format!("-I{}", inc.display()));
        }
        // Header lives in csrc/ alongside the .hip.
        cc_cmd.arg("-Icsrc");
        let status = cc_cmd
            .status()
            .unwrap_or_else(|e| panic!("running {hipcc}: {e} (gpu-native needs hipcc on PATH or NKVX_HIPCC)"));
        if !status.success() {
            panic!("hipcc compile of csrc/nkvx_gpu.hip failed ({status})");
        }
        // Archive the single object into libnkvx_gpu.a so rustc links it.
        let lib = out_dir.join("libnkvx_gpu.a");
        let ar_status = Command::new("ar")
            .arg("crs")
            .arg(&lib)
            .arg(&obj)
            .status()
            .unwrap_or_else(|e| panic!("running ar: {e}"));
        if !ar_status.success() {
            panic!("ar archive of nkvx_gpu.o failed ({ar_status})");
        }
        println!("cargo:rustc-link-search=native={}", out_dir.display());
        println!("cargo:rustc-link-lib=static=nkvx_gpu");
    }

    // --- 2. Link args, mirroring build_gpu.sh:23-37 minus -lamdhip64 ---
    let arg = |s: &str| println!("cargo:rustc-link-arg={s}");
    let path_arg = |p: &Path| println!("cargo:rustc-link-arg={}", p.display());

    // (line 24) hardening
    arg("-pthread");
    arg("-Wl,-z,relro,-z,now");
    arg("-Wl,-z,noexecstack");

    // (line 25) -L paths
    arg(&format!("-L{}", vfio_user_lib.display()));
    arg(&format!("-L{}", build.join("lib").display()));

    // (lines 26-28) whole-archive SPDK libs
    arg("-Wl,--whole-archive");
    arg("-Wl,--no-as-needed");
    arg("-lspdk_vfio_user");
    arg("-lspdk_util");
    arg("-lspdk_log");
    arg("-Wl,--no-whole-archive");

    // (line 29) env_dpdk static archive (by path, not whole-archive)
    path_arg(&env_dpdk_a);

    // (lines 30-32) whole-archive all dpdk librte_*.a
    arg("-Wl,--whole-archive");
    let mut rte: Vec<PathBuf> = std::fs::read_dir(&dpdk_lib)
        .unwrap_or_else(|e| panic!("read_dir {}: {e}", dpdk_lib.display()))
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| {
            let n = p.file_name().and_then(|s| s.to_str()).unwrap_or("");
            n.starts_with("librte_") && n.ends_with(".a")
        })
        .collect();
    rte.sort();
    if rte.is_empty() {
        panic!("no librte_*.a found in {}", dpdk_lib.display());
    }
    for p in &rte {
        path_arg(p);
    }
    arg("-Wl,--no-whole-archive");

    // (line 33) system libs
    arg("-lnuma");
    arg("-ldl");
    arg("-libverbs");
    arg("-lrdmacm");

    // (line 34) isa-l static archives
    path_arg(&isal_a);
    path_arg(&isalcrypto_a);

    // (lines 35-36) remaining libs (line 35 starts with -lvfio-user; -pthread
    // already emitted above and again here matches the script's repetition).
    arg("-lvfio-user");
    arg("-ljson-c");
    arg("-pthread");
    arg("-lrt");
    arg("-luuid");
    arg("-lssl");
    arg("-lcrypto");
    arg("-lm");
    arg("-llz4");
    arg("-lfuse3");
    arg("-lkeyutils");

    // (line 37) -lamdhip64: ONLY when gpu-native is on. The default build omits
    // it entirely so no ROCm runtime is pulled in. We add the HIP lib dir to the
    // search path (resolved from hipconfig -p, else /usr; override via
    // NKVX_HIP_LIBDIR) and link libamdhip64.
    if gpu_native {
        let libdir = hip_libdir();
        arg(&format!("-L{}", libdir.display()));
        // Embed an rpath so the in-process HIP runtime is found at run time
        // without LD_LIBRARY_PATH (sudo env_reset drops it anyway).
        arg(&format!("-Wl,-rpath,{}", libdir.display()));
        arg("-lamdhip64");
    }

    // -lpthread (plan: trailing -lpthread; -pthread already covers it but keep
    // explicit to match the plan's stated tail).
    arg("-lpthread");

    // libc LAST: rustc links with -nodefaultlibs, so libc is not auto-appended
    // after these whole-archive DPDK static libs. librte_telemetry.a references
    // glibc's atexit, which the linker only resolves if libc follows the DPDK
    // archives on the command line.
    arg("-lc");
}

/// Locate the directory holding libamdhip64 for the gpu-native link. Honors
/// NKVX_HIP_LIBDIR, else asks `hipconfig -p` (ROCm root) and probes its lib
/// dirs, else falls back to common system locations.
fn hip_libdir() -> PathBuf {
    if let Ok(d) = std::env::var("NKVX_HIP_LIBDIR") {
        return PathBuf::from(d);
    }
    // hipconfig -p prints the ROCm install path (e.g. /usr or /opt/rocm-X.Y).
    if let Ok(out) = Command::new("hipconfig").arg("-p").output() {
        if out.status.success() {
            let root = String::from_utf8_lossy(&out.stdout).trim().to_string();
            if !root.is_empty() {
                for cand in [PathBuf::from(&root).join("lib"), PathBuf::from(&root).join("lib64")] {
                    if cand.join("libamdhip64.so").exists() || cand.join("libamdhip64.so.7").exists() {
                        return cand;
                    }
                }
            }
        }
    }
    for cand in ["/usr/lib64", "/usr/lib", "/opt/rocm/lib"] {
        let p = PathBuf::from(cand);
        if p.join("libamdhip64.so").exists() || p.join("libamdhip64.so.7").exists() {
            return p;
        }
    }
    // Last resort: let the linker's default search path try.
    PathBuf::from("/usr/lib64")
}
