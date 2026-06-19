// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM Corporation. All rights reserved.
//! In-process native HIP `--gpu` datapath (bead spdk-jhk.7.15).
//!
//! Built ONLY with `--features gpu-native`. Mirrors the public surface of the
//! subprocess delegation in [`crate::gpu`] (`store` / `retrieve` / `exec` with
//! the same signatures), so `src/main.rs` can route `--gpu` through either path
//! by a single `#[cfg(feature = "gpu-native")]` switch with no other changes.
//!
//! The heavy lifting lives in `csrc/nkvx_gpu.hip`: the GPU builds the NVMe SQE
//! into the IO SQ ring and rings the doorbell from a device kernel (exactly the
//! nkv_vfu_gpu mechanism), reusing the shared nkv_vfu.h driver. Here we just own
//! the FFI session, redirect attach chatter off stdout, and apply the same
//! size-probe + re-read short-buffer recovery the CPU datapath uses (no silent
//! truncation; beads .9/.10).

use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};

use anyhow::{anyhow, bail, Context, Result};

use crate::config::Config;
use crate::datapath::StdoutToStderr;

/// First-pass receive/result buffer; the true length is learned from the
/// returned count and a larger value triggers one re-read. Mirrors
/// `datapath::RETRIEVE_HINT`.
const RETRIEVE_HINT: usize = 4 * 1024 * 1024;
/// Hard ceiling matching the controller max_io_size the SGL path targets.
const MAX_VALUE: usize = 64 * 1024 * 1024;

#[repr(C)]
struct NkvxGpuSession {
    _private: [u8; 0],
}

extern "C" {
    fn nkvx_gpu_open(traddr_dir: *const c_char) -> *mut NkvxGpuSession;
    fn nkvx_gpu_close(s: *mut NkvxGpuSession);
    fn nkvx_gpu_store(
        s: *mut NkvxGpuSession,
        nsid: u32,
        key: *const c_char,
        val: *const c_void,
        len: u32,
    ) -> c_int;
    fn nkvx_gpu_retrieve(
        s: *mut NkvxGpuSession,
        nsid: u32,
        key: *const c_char,
        out: *mut c_void,
        out_len: u32,
        got: *mut u32,
    ) -> c_int;
    fn nkvx_gpu_exec(
        s: *mut NkvxGpuSession,
        nsid: u32,
        key: *const c_char,
        op_id: u32,
        input: *const c_void,
        in_len: u32,
        out: *mut c_void,
        out_len: u32,
        rlen: *mut u32,
    ) -> c_int;
}

/// RAII owner of the native GPU session (closes the FFI handle on drop).
struct GpuSession {
    raw: *mut NkvxGpuSession,
}

impl GpuSession {
    fn open(traddr: &str) -> Result<Self> {
        let c = CString::new(traddr).context("traddr contains NUL")?;
        let _quiet = StdoutToStderr::new();
        // SAFETY: c is a valid NUL-terminated string for the call's duration.
        let raw = unsafe { nkvx_gpu_open(c.as_ptr()) };
        if raw.is_null() {
            bail!(
                "nkvx_gpu_open({traddr}) failed (no HIP device, target down, or \
                 attach/GPU-map error)"
            );
        }
        Ok(GpuSession { raw })
    }

    fn store(&self, nsid: u32, key: &str, val: &[u8]) -> Result<()> {
        let k = CString::new(key).context("key contains NUL")?;
        // SAFETY: handle valid; k and val outlive the call; len matches val.
        let rc = unsafe {
            nkvx_gpu_store(
                self.raw,
                nsid,
                k.as_ptr(),
                val.as_ptr() as *const c_void,
                val.len() as u32,
            )
        };
        if rc != 0 {
            return Err(anyhow!(
                "nkvx_gpu_store(nsid={nsid}, key={key}) failed: rc={rc}"
            ));
        }
        Ok(())
    }

    fn retrieve(&self, nsid: u32, key: &str) -> Result<Vec<u8>> {
        let k = CString::new(key).context("key contains NUL")?;
        let mut buf = vec![0u8; RETRIEVE_HINT];
        let true_len = self.retrieve_raw(nsid, &k, &mut buf)? as usize;
        if true_len <= buf.len() {
            buf.truncate(true_len);
            return Ok(buf);
        }
        if true_len > MAX_VALUE {
            bail!(
                "value for nsid={nsid} key={key} is {true_len} bytes, exceeds the \
                 {MAX_VALUE}-byte single-value ceiling; refusing to truncate"
            );
        }
        let mut buf = vec![0u8; true_len];
        let got2 = self.retrieve_raw(nsid, &k, &mut buf)? as usize;
        if got2 != true_len {
            bail!(
                "value for nsid={nsid} key={key} changed size between probe \
                 ({true_len}) and re-read ({got2}); aborting"
            );
        }
        buf.truncate(got2);
        Ok(buf)
    }

    fn retrieve_raw(&self, nsid: u32, k: &CString, out: &mut [u8]) -> Result<u32> {
        let mut got: u32 = 0;
        // SAFETY: handle valid; k and out outlive the call; out_len matches out.
        let rc = unsafe {
            nkvx_gpu_retrieve(
                self.raw,
                nsid,
                k.as_ptr(),
                out.as_mut_ptr() as *mut c_void,
                out.len() as u32,
                &mut got as *mut u32,
            )
        };
        if rc != 0 {
            return Err(anyhow!("nkvx_gpu_retrieve(nsid={nsid}) failed: rc={rc}"));
        }
        Ok(got)
    }

    fn exec(&self, nsid: u32, key: &str, op_id: u32, input: &[u8]) -> Result<Vec<u8>> {
        let k = CString::new(key).context("key contains NUL")?;
        let mut buf = vec![0u8; RETRIEVE_HINT];
        let true_len = self.exec_raw(nsid, &k, op_id, input, &mut buf)? as usize;
        if true_len <= buf.len() {
            buf.truncate(true_len);
            return Ok(buf);
        }
        if true_len > MAX_VALUE {
            bail!(
                "exec result for nsid={nsid} key={key} op_id={op_id} is {true_len} \
                 bytes, exceeds the {MAX_VALUE}-byte ceiling; refusing to truncate"
            );
        }
        let mut buf = vec![0u8; true_len];
        let got2 = self.exec_raw(nsid, &k, op_id, input, &mut buf)? as usize;
        if got2 != true_len {
            bail!(
                "exec result for nsid={nsid} key={key} op_id={op_id} changed size \
                 between probe ({true_len}) and re-run ({got2}); aborting"
            );
        }
        buf.truncate(got2);
        Ok(buf)
    }

    fn exec_raw(
        &self,
        nsid: u32,
        k: &CString,
        op_id: u32,
        input: &[u8],
        out: &mut [u8],
    ) -> Result<u32> {
        let mut rlen: u32 = 0;
        // SAFETY: handle valid; k, input, out outlive the call; lengths match.
        let rc = unsafe {
            nkvx_gpu_exec(
                self.raw,
                nsid,
                k.as_ptr(),
                op_id,
                input.as_ptr() as *const c_void,
                input.len() as u32,
                out.as_mut_ptr() as *mut c_void,
                out.len() as u32,
                &mut rlen as *mut u32,
            )
        };
        if rc != 0 {
            return Err(anyhow!(
                "nkvx_gpu_exec(nsid={nsid}, op_id={op_id}) failed: rc={rc}"
            ));
        }
        Ok(rlen)
    }
}

impl Drop for GpuSession {
    fn drop(&mut self) {
        // SAFETY: raw came from nkvx_gpu_open and is closed exactly once.
        unsafe { nkvx_gpu_close(self.raw) };
    }
}

/// `--gpu store ns/key` (native in-process path). The native HIP entrypoints
/// thread nsid through to the SQE (bead spdk-jhk.10), so any namespace is
/// addressable (default nsid 1 when none is configured).
pub fn store(cfg: &Config, nsid: u32, key: &str, val: &[u8]) -> Result<()> {
    let sess = GpuSession::open(cfg.traddr())?;
    sess.store(nsid, key, val)
}

/// `--gpu get` for one-or-more keys sharing `nsid` (native in-process path,
/// bead spdk-jhk.11). The native HIP path keeps PER-KEY submission for v1 (each
/// key is one GPU-rung Retrieve); the wavefront/one-doorbell batch lives in the
/// subprocess `nkv_vfu_gpu retrieve-batch` path (src/gpu.rs). A not-found key
/// surfaces as a hard error here (the native FFI has no not-found sentinel), so
/// callers map it the same way the CPU datapath does. Returns one value per key
/// in input order. Documented single-session reuse: one open() for the batch.
pub fn retrieve_batch(cfg: &Config, nsid: u32, keys: &[String]) -> Result<Vec<Option<Vec<u8>>>> {
    if keys.is_empty() {
        return Ok(Vec::new());
    }
    let sess = GpuSession::open(cfg.traddr())?;
    let mut out = Vec::with_capacity(keys.len());
    for k in keys {
        out.push(Some(sess.retrieve(nsid, k)?));
    }
    Ok(out)
}

/// `--gpu exec name ns/key` (native in-process path). The native path supports
/// an exec input payload (unlike the subprocess binary, which takes none), so
/// `input_present` is informational here and accepted. Returns bytes formatted
/// to match nkv_vfu_gpu's stdout for op 10 (bytecount) / 11 (identity), keeping
/// the user-visible `--gpu` output identical across native/subprocess paths.
pub fn exec(
    cfg: &Config,
    nsid: u32,
    key: &str,
    op_id: u32,
    _input_present: bool,
) -> Result<Vec<u8>> {
    let sess = GpuSession::open(cfg.traddr())?;
    let raw = sess.exec(nsid, key, op_id, &[])?;
    // Mirror nkv_vfu_gpu's pretty-print so `--gpu exec` output matches the
    // subprocess path byte-for-byte at the line level.
    let line = if op_id == 10 {
        let mut le = [0u8; 8];
        let n = raw.len().min(8);
        le[..n].copy_from_slice(&raw[..n]);
        let count = u64::from_le_bytes(le);
        format!(
            "EXEC ok: GPU exec op {op_id} (nkvx:bytecount) key='{key}' -> count={count}\n"
        )
    } else {
        format!(
            "EXEC ok: GPU exec op {op_id} key='{key}' -> {} bytes: {}\n",
            raw.len(),
            String::from_utf8_lossy(&raw)
        )
    };
    Ok(line.into_bytes())
}
