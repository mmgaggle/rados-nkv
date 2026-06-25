// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM Corporation. All rights reserved.
//! Safe RAII wrapper over the FFI shim (bead spdk-jhk.7.2).
//!
//! `spdk_env_init` runs once per process, so the CLI opens ONE [`Session`] per
//! invocation, performs N ops, and closes on drop. Each op threads an explicit
//! `nsid` (resolved from the config's `[namespaces]` table by the caller).

use std::ffi::CString;
use std::os::raw::c_void;
use std::ptr;

use anyhow::{anyhow, bail, Context, Result};

use crate::ffi;

const STDOUT_FD: i32 = 1;
const STDERR_FD: i32 = 2;

/// RAII guard that redirects the process stdout (fd 1) onto stderr (fd 2) for its
/// lifetime, restoring the original stdout on drop. The driver attaches with
/// chatty C `printf` calls to stdout; we route them to stderr so a `get` keeps a
/// clean, byte-exact stdout suitable for scripting/pipes.
pub(crate) struct StdoutToStderr {
    saved: i32,
}

impl StdoutToStderr {
    pub(crate) fn new() -> Self {
        // Flush any pending Rust + C stdio so buffered bytes land before the swap.
        let _ = std::io::Write::flush(&mut std::io::stdout());
        // SAFETY: NULL flushes all C streams; pure FD ops below have defined ABI.
        unsafe { ffi::fflush(ptr::null_mut()) };
        let saved = unsafe { ffi::dup(STDOUT_FD) };
        if saved >= 0 {
            unsafe { ffi::dup2(STDERR_FD, STDOUT_FD) };
        }
        StdoutToStderr { saved }
    }
}

impl Drop for StdoutToStderr {
    fn drop(&mut self) {
        // SAFETY: flush C stdio written during the guarded region, then restore.
        unsafe { ffi::fflush(ptr::null_mut()) };
        if self.saved >= 0 {
            unsafe {
                ffi::dup2(self.saved, STDOUT_FD);
                ffi::close(self.saved);
            }
        }
    }
}

/// First-pass receive buffer for a retrieve. The shim does not report a value's
/// length before the read, so we allocate this once, learn the TRUE length from
/// the returned `got`, and — if the value was larger — re-read into a right-sized
/// buffer (size-probe + retry). This is only a starting size, NOT a hard cap:
/// most values fit in one pass; larger ones cost a second round-trip but are
/// returned in full (bead spdk-jhk.7.9 — no silent truncation).
const RETRIEVE_HINT: usize = 4 * 1024 * 1024;

/// Hard ceiling for a single value, matching the controller's `max_io_size` that
/// the region-bounded SGL store/retrieve path (`nvfu_kv_xfer_sgl`) targets. A
/// value whose TRUE length exceeds this is reported as a clear error rather than
/// truncated, so a `get` never silently drops bytes.
const MAX_VALUE: usize = 64 * 1024 * 1024;

/// A safe owner of an nkvx vfio-user session; closes the FFI handle on drop.
pub struct Session {
    raw: *mut ffi::NkvxSession,
}

impl Session {
    /// Open a session against the vfio-user listener dir (containing `cntrl`).
    /// Attach diagnostics the driver prints to stdout are redirected to stderr so
    /// stdout stays clean for value bytes.
    pub fn open(traddr: &str) -> Result<Self> {
        let c = CString::new(traddr).context("traddr contains NUL")?;
        let _quiet = StdoutToStderr::new();
        // SAFETY: c is a valid NUL-terminated string for the duration of the call.
        let raw = unsafe { ffi::nkvx_open(c.as_ptr()) };
        if raw.is_null() {
            bail!("nkvx_open({traddr}) failed (target down or attach error)");
        }
        Ok(Session { raw })
    }

    /// KV Store `val` under `key` in `nsid` with default (no) store options.
    pub fn store(&self, nsid: u32, key: &str, val: &[u8]) -> Result<()> {
        self.store_opts(nsid, key, val, 0, 0)
    }

    /// KV Store with explicit Store Option bits and TTL (spdk-jhk.7.14).
    ///
    /// `store_opt` is the CDW11 Store Option byte (TTL_VALID / EPHEMERAL / TOUCH
    /// and the conditional SIKE/SINKE bits); `ttl` the CDW12 TTL in seconds,
    /// honoured by the target only when the TTL_VALID bit is set in `store_opt`.
    pub fn store_opts(&self, nsid: u32, key: &str, val: &[u8], store_opt: u8, ttl: u32) -> Result<()> {
        let k = CString::new(key).context("key contains NUL")?;
        // SAFETY: handle is valid; k and val outlive the call; len matches val.
        let rc = unsafe {
            ffi::nkvx_store(
                self.raw,
                nsid,
                k.as_ptr(),
                val.as_ptr() as *const c_void,
                val.len() as u32,
                store_opt,
                ttl,
            )
        };
        if rc != 0 {
            return Err(anyhow!("nkvx_store(nsid={nsid}, key={key}) failed: rc={rc}"));
        }
        Ok(())
    }

    /// KV Retrieve `key` from `nsid`, returning the FULL value bytes.
    ///
    /// The shim reports the value's TRUE length in `got` even when our buffer was
    /// too small (the target completes a short read with SUCCESS and the full
    /// length in cdw0). So we do a size-probe + retry: read into a hint-sized
    /// buffer; if the true length exceeded it, re-read into an exactly-sized
    /// buffer. This never silently truncates — a value above `MAX_VALUE` is a hard
    /// error instead (bead spdk-jhk.7.9).
    pub fn retrieve(&self, nsid: u32, key: &str) -> Result<Vec<u8>> {
        let k = CString::new(key).context("key contains NUL")?;

        // First pass: hint-sized buffer learns the true length.
        let mut buf = vec![0u8; RETRIEVE_HINT];
        let true_len = self.retrieve_raw(nsid, &k, &mut buf)? as usize;

        if true_len <= buf.len() {
            buf.truncate(true_len);
            return Ok(buf);
        }

        // Value was larger than the first-pass buffer (it was truncated into it).
        // Refuse to silently drop bytes: bail above the hard ceiling, else re-read
        // into a right-sized buffer.
        if true_len > MAX_VALUE {
            bail!(
                "value for nsid={nsid} key={key} is {true_len} bytes, exceeds the \
                 {MAX_VALUE}-byte single-value ceiling (controller max_io_size); \
                 refusing to return a truncated result"
            );
        }
        let mut buf = vec![0u8; true_len];
        let got2 = self.retrieve_raw(nsid, &k, &mut buf)? as usize;
        if got2 != true_len {
            bail!(
                "value for nsid={nsid} key={key} changed size between probe \
                 ({true_len}) and re-read ({got2}); aborting to avoid a partial result"
            );
        }
        // Defensive: the second buffer was sized to the probe length, so the read
        // must have filled it exactly; truncate is a no-op but keeps intent clear.
        buf.truncate(got2);
        Ok(buf)
    }

    /// One FFI retrieve into `out`, returning the TRUE value length the target
    /// reported (may exceed `out.len()`, signalling a short buffer).
    fn retrieve_raw(&self, nsid: u32, k: &CString, out: &mut [u8]) -> Result<u32> {
        let mut got: u32 = 0;
        // SAFETY: handle valid; k and out outlive the call; out_len matches out.
        let rc = unsafe {
            ffi::nkvx_retrieve(
                self.raw,
                nsid,
                k.as_ptr(),
                out.as_mut_ptr() as *mut c_void,
                out.len() as u32,
                &mut got as *mut u32,
            )
        };
        if rc != 0 {
            return Err(anyhow!(
                "nkvx_retrieve(nsid={nsid}) failed: rc={rc}"
            ));
        }
        Ok(got)
    }

    /// Retrieve into a caller-provided buffer, returning the byte count written.
    /// Kept for the foundation smoke test / callers that size their own buffer.
    /// Errors (rather than truncating) if the value is larger than `out`, since
    /// the caller chose the size and a short read would lose data silently.
    #[allow(dead_code)]
    pub fn retrieve_into(&self, nsid: u32, key: &str, out: &mut [u8]) -> Result<usize> {
        let k = CString::new(key).context("key contains NUL")?;
        let cap = out.len();
        let true_len = self.retrieve_raw(nsid, &k, out)? as usize;
        if true_len > cap {
            bail!(
                "value for nsid={nsid} key={key} is {true_len} bytes but the \
                 caller buffer is only {cap}; would truncate"
            );
        }
        Ok(true_len)
    }

    /// KV Exec `op_id` against `key` in `nsid` with optional `input`, returning
    /// the FULL result bytes (bead spdk-jhk.7.3, used by the `exec` command).
    ///
    /// The shim reports the result's TRUE length in `rlen` even when our output
    /// buffer was too small (the target completes with the full length in cdw0;
    /// the shim leaves it unclamped — see `nkvx_exec` in nkvx_shim.c). So we do
    /// the same size-probe + retry as [`Session::retrieve`]: run into a hint-sized
    /// buffer; if the true result exceeded it, re-run into an exactly-sized one.
    /// This never silently truncates — a result above `MAX_VALUE` is a hard error
    /// instead (bead spdk-jhk.7.10, same class as the .9 `get` fix).
    pub fn exec(&self, nsid: u32, key: &str, op_id: u32, input: &[u8]) -> Result<Vec<u8>> {
        let k = CString::new(key).context("key contains NUL")?;

        // First pass: probe for the true result length.
        //
        // The forwarder derives the EXEC input length from the request's value-region
        // length (payload after the in-payload key head), NOT from a separate vsize
        // field, and the value region serves as BOTH the input source and the result
        // sink (one DPTR span). So if osize (the sink size) exceeds the input, the
        // forwarder gathers osize bytes of input — zero-padded past the real input —
        // and an input-echoing op (`inputecho`) would echo the padded length, not the
        // caller's input (bead spdk-4i7). To probe WITHOUT padding the input, size the
        // first pass to the input length when there is input (so input == result for
        // `inputecho` round-trips exactly); use the retrieve hint only when the input is
        // empty (e.g. `identity`/`bytecount`, which ignore the input and read the stored
        // object). If the true result still exceeds the probe, the BUFFER_TOO_SMALL
        // size-probe below re-runs into an exactly-sized buffer (identity/bytecount
        // ignore the input, so the larger re-run sink is harmless).
        let probe_len = if input.is_empty() {
            RETRIEVE_HINT
        } else {
            input.len()
        };
        let mut buf = vec![0u8; probe_len.max(1)];
        let true_len = self.exec_raw(nsid, &k, op_id, input, &mut buf)? as usize;

        if true_len <= buf.len() {
            buf.truncate(true_len);
            return Ok(buf);
        }

        // Result was larger than the first-pass buffer (it was truncated into it).
        // Refuse to silently drop bytes: bail above the hard ceiling, else re-run
        // into a right-sized buffer.
        if true_len > MAX_VALUE {
            bail!(
                "exec result for nsid={nsid} key={key} op_id={op_id} is {true_len} \
                 bytes, exceeds the {MAX_VALUE}-byte single-value ceiling \
                 (controller max_io_size); refusing to return a truncated result"
            );
        }
        let mut buf = vec![0u8; true_len];
        let got2 = self.exec_raw(nsid, &k, op_id, input, &mut buf)? as usize;
        if got2 != true_len {
            bail!(
                "exec result for nsid={nsid} key={key} op_id={op_id} changed size \
                 between probe ({true_len}) and re-run ({got2}); aborting to avoid \
                 a partial result"
            );
        }
        // Defensive: the second buffer was sized to the probe length, so the run
        // must have filled it exactly; truncate is a no-op but keeps intent clear.
        buf.truncate(got2);
        Ok(buf)
    }

    /// One FFI exec into `out`, returning the TRUE result length the target
    /// reported (may exceed `out.len()`, signalling a short buffer).
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
            ffi::nkvx_exec(
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
                "nkvx_exec(nsid={nsid}, op_id={op_id}) failed: rc={rc}"
            ));
        }
        Ok(rlen)
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        // SAFETY: raw came from nkvx_open and is closed exactly once.
        unsafe { ffi::nkvx_close(self.raw) };
    }
}
