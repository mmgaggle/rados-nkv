// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM Corporation. All rights reserved.
//! Raw `extern "C"` declarations for the nkvx FFI shim (csrc/nkvx_shim.c).
//!
//! The shim wraps the proven raw vfio-user NVMe-KV driver (nkv_vfu.h) behind an
//! opaque session handle. Every function returns 0 on success, negative errno on
//! failure. These are unsafe FFI primitives; safe wrappers live elsewhere.

use std::os::raw::{c_char, c_int, c_void};

// Minimal libc FD primitives, declared directly to avoid a `libc` crate
// dependency. Stable C ABI. Used to redirect the driver's attach chatter
// (C `printf` to stdout) onto stderr so `get` keeps a clean, scriptable stdout.
extern "C" {
    pub fn dup(oldfd: c_int) -> c_int;
    pub fn dup2(oldfd: c_int, newfd: c_int) -> c_int;
    pub fn close(fd: c_int) -> c_int;
    /// Flush a C stdio stream; passing NULL flushes all open streams.
    pub fn fflush(stream: *mut c_void) -> c_int;
}

/// Opaque handle to a vfio-user NVMe-KV session (wraps `struct nvfu_dev`).
#[repr(C)]
pub struct NkvxSession {
    _private: [u8; 0],
}

extern "C" {
    pub fn nkvx_open(traddr_dir: *const c_char) -> *mut NkvxSession;
    pub fn nkvx_close(s: *mut NkvxSession);

    pub fn nkvx_store(
        s: *mut NkvxSession,
        nsid: u32,
        key: *const c_char,
        val: *const c_void,
        len: u32,
        // CDW11 Store Option byte (SIKE/SINKE, TTL_VALID, EPHEMERAL, TOUCH).
        store_opt: u8,
        // CDW12 TTL in seconds (honoured only when TTL_VALID set in store_opt).
        ttl: u32,
    ) -> c_int;

    pub fn nkvx_retrieve(
        s: *mut NkvxSession,
        nsid: u32,
        key: *const c_char,
        out: *mut c_void,
        out_len: u32,
        got: *mut u32,
    ) -> c_int;

    pub fn nkvx_exec(
        s: *mut NkvxSession,
        nsid: u32,
        key: *const c_char,
        op_id: u32,
        input: *const c_void,
        in_len: u32,
        out: *mut c_void,
        out_len: u32,
        rlen: *mut u32,
    ) -> c_int;

    pub fn nkvx_exist(
        s: *mut NkvxSession,
        nsid: u32,
        key: *const c_char,
        // out: 1 if the key is present, 0 if absent.
        present: *mut c_int,
        // out: the stored value's full length (cpl.cdw0) when present, else 0.
        len: *mut u32,
    ) -> c_int;
}
