//! `--gpu` delegation to the `nkv_vfu_gpu` binary (bead spdk-jhk.7.7).
//!
//! The native GPU-initiated datapath (HIP/ROCm) lives in the separate
//! `nkv_vfu_gpu` binary built by `../vfu_host/build_gpu.sh`. Linking ROCm into
//! `rados-nkv` itself would drag the HIP runtime into the default build, so for
//! `--gpu` we instead delegate: translate the ergonomic `rados-nkv` arguments to
//! `nkv_vfu_gpu`'s positional CLI and exec it as a subprocess.
//!
//! `nkv_vfu_gpu`'s relevant surface (see `nkv_vfu_gpu.hip` usage()):
//!
//! ```text
//!   <traddr> store      <key> <value>  GPU-initiated KV Store (value as argv)
//!   <traddr> store-file <key> <file>   GPU-initiated KV Store (value from file, SGL)
//!   <traddr> retrieve   <key>          GPU-initiated KV Retrieve  (prints value)
//!   <traddr> exec       <key> <op_id>  GPU-initiated KV Exec
//! ```
//!
//! `store` delegates to `store-file` (bead spdk-jhk.13): we write the value to a
//! temp file and the GPU binary stores it over a region-bounded SGL, so values
//! >4 KiB and binary/NUL values are supported (bounded by the controller
//! max_io_size, 64 MiB) instead of capped at one argv token. The target namespace
//! is selected via the `NKVX_NSID` env var the GPU binary reads (bead
//! spdk-jhk.10): we pass the registry/config-resolved nsid through, so any
//! namespace is addressable (default nsid 1 when none is set). Unsupported
//! combinations (multi-key get, NUL/oversized values) are still rejected up front
//! with a clear error rather than silently mistranslated.

use std::io::Write;
use std::path::PathBuf;
use std::process::Command;

use anyhow::{anyhow, bail, Context, Result};

use crate::config::Config;

/// Sentinel length the GPU client frames for a not-found / failed key in the
/// `retrieve-batch` stream (mirrors NKVX_NOTFOUND_LEN in nkv_vfu_gpu.hip).
const NKVX_NOTFOUND_LEN: u32 = 0xFFFF_FFFF;

/// Resolve the `nkv_vfu_gpu` binary: `[target] gpu_bin` if set, else search
/// `PATH`. A configured path must exist; a PATH lookup that misses is a clear
/// error telling the user how to point at the binary.
fn resolve_gpu_bin(cfg: &Config) -> Result<PathBuf> {
    if let Some(p) = &cfg.gpu_bin {
        let pb = PathBuf::from(p);
        if !pb.exists() {
            bail!(
                "configured [target] gpu_bin '{p}' does not exist \
                 (build it via ../vfu_host/build_gpu.sh)"
            );
        }
        return Ok(pb);
    }
    find_in_path("nkv_vfu_gpu").ok_or_else(|| {
        anyhow!(
            "nkv_vfu_gpu not found on PATH; set [target] gpu_bin in \
             ~/.rados-nkv.conf or add it to PATH (build it via \
             ../vfu_host/build_gpu.sh)"
        )
    })
}

/// Minimal PATH search for an executable by name (no external crate).
fn find_in_path(name: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    for dir in std::env::split_paths(&path) {
        let cand = dir.join(name);
        if cand.is_file() {
            return Some(cand);
        }
    }
    None
}

/// `--gpu store ns/key` (value already read by the caller). Delegates to
/// `nkv_vfu_gpu <traddr> store-file <key> <valuefile>` (bead spdk-jhk.13): the
/// value is written to a temp file and the GPU binary stores it over a
/// region-bounded SGL, so values >4 KiB (and binary/NUL values) no longer fail
/// with KV Store status 0x6 nor hit the argv-token ceiling. The namespace is
/// selected via the `NKVX_NSID` env var the GPU binary reads. Bounded by the
/// controller max_io_size (64 MiB); a larger value is rejected up front.
pub fn store(cfg: &Config, nsid: u32, key: &str, val: &[u8]) -> Result<()> {
    const MAX_VALUE: usize = 64 * 1024 * 1024; // controller max_io_size
    if val.len() > MAX_VALUE {
        bail!(
            "--gpu store value is {} bytes; exceeds the controller max_io_size \
             ({MAX_VALUE} bytes). Use the CPU datapath (drop --gpu) only if it \
             supports a larger value",
            val.len()
        );
    }
    let bin = resolve_gpu_bin(cfg)?;

    // Pass the value via a temp file (not argv), so it can be >ARG_MAX, contain
    // NUL bytes, and ride the SGL store path. A small RAII guard removes it.
    let valfile = TempKeyfile::create()?;
    {
        let mut f = std::fs::File::create(valfile.path())
            .with_context(|| format!("creating temp value file {}", valfile.path().display()))?;
        f.write_all(val).context("writing value to temp file")?;
        f.flush().context("flushing temp value file")?;
    }

    let status = Command::new(&bin)
        .env("NKVX_NSID", nsid.to_string())
        .arg(cfg.traddr())
        .arg("store-file")
        .arg(key)
        .arg(valfile.path())
        .status()
        .with_context(|| format!("spawning {}", bin.display()))?;
    if !status.success() {
        bail!("nkv_vfu_gpu store-file failed ({status})");
    }
    Ok(())
}

/// A self-removing temp file path for the `--gpu get` keyfile. Avoids pulling in
/// the `tempfile` crate; the name embeds pid + a process-local counter to stay
/// unique across concurrent invocations.
struct TempKeyfile {
    path: PathBuf,
}

impl TempKeyfile {
    fn create() -> Result<Self> {
        use std::sync::atomic::{AtomicU64, Ordering};
        static SEQ: AtomicU64 = AtomicU64::new(0);
        let seq = SEQ.fetch_add(1, Ordering::Relaxed);
        let name = format!("rados-nkv-gpu-keys.{}.{seq}.txt", std::process::id());
        let path = std::env::temp_dir().join(name);
        Ok(TempKeyfile { path })
    }
    fn path(&self) -> &std::path::Path {
        &self.path
    }
}

impl Drop for TempKeyfile {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
}

/// `--gpu get` for one-OR-MORE keys that resolve to the SAME nsid (bead
/// spdk-jhk.11). Writes the keys one-per-line to a temp file, spawns
/// `nkv_vfu_gpu <traddr> retrieve-batch <keyfile>` (which issues all N Retrieves
/// across one wavefront + ONE doorbell per chunk), then parses the length-framed
/// stdout stream into one `Vec<u8>` per key IN INPUT ORDER. A not-found key
/// yields `None`. The single-key case is just N == 1 (no special path).
///
/// The single wavefront maps to one controller IO queue, hence one nsid; the
/// caller guarantees all keys share `nsid` (multi-nsid get is rejected upstream).
pub fn retrieve_batch(cfg: &Config, nsid: u32, keys: &[String]) -> Result<Vec<Option<Vec<u8>>>> {
    if keys.is_empty() {
        return Ok(Vec::new());
    }
    let bin = resolve_gpu_bin(cfg)?;

    // Keys go via a temp file (not argv) so the batch size is unbounded by
    // ARG_MAX and the GPU client reads them one-per-line. A small RAII guard
    // removes the file on drop (no external tempfile crate dependency).
    let keyfile = TempKeyfile::create()?;
    {
        let mut f = std::fs::File::create(keyfile.path())
            .with_context(|| format!("creating temp keyfile {}", keyfile.path().display()))?;
        for k in keys {
            if k.as_bytes().contains(&b'\n') || k.as_bytes().contains(&b'\r') {
                bail!("--gpu get key '{k}' contains a newline; line-framed keyfile cannot carry it");
            }
            writeln!(f, "{k}").context("writing key to temp keyfile")?;
        }
        f.flush().context("flushing temp keyfile")?;
    }

    let out = Command::new(&bin)
        .env("NKVX_NSID", nsid.to_string())
        .arg(cfg.traddr())
        .arg("retrieve-batch")
        .arg(keyfile.path())
        .output()
        .with_context(|| format!("spawning {}", bin.display()))?;
    if !out.status.success() {
        bail!(
            "nkv_vfu_gpu retrieve-batch failed ({}): {}",
            out.status,
            String::from_utf8_lossy(&out.stderr).trim()
        );
    }
    parse_framed(&out.stdout, keys.len()).with_context(|| {
        format!(
            "parsing nkv_vfu_gpu retrieve-batch stream for {} key(s)",
            keys.len()
        )
    })
}

/// Parse the length-framed retrieve-batch stream: for each of `expect` keys, a
/// 4-byte LE length then that many value bytes; the sentinel 0xFFFFFFFF marks a
/// not-found key (no value bytes follow). Returns one entry per key in order.
fn parse_framed(stdout: &[u8], expect: usize) -> Result<Vec<Option<Vec<u8>>>> {
    let mut vals = Vec::with_capacity(expect);
    let mut pos = 0usize;
    for i in 0..expect {
        if pos + 4 > stdout.len() {
            bail!(
                "truncated frame header for key {i} (offset {pos}, stream {} bytes)",
                stdout.len()
            );
        }
        let len = u32::from_le_bytes([
            stdout[pos],
            stdout[pos + 1],
            stdout[pos + 2],
            stdout[pos + 3],
        ]);
        pos += 4;
        if len == NKVX_NOTFOUND_LEN {
            vals.push(None);
            continue;
        }
        let len = len as usize;
        let end = pos
            .checked_add(len)
            .filter(|&e| e <= stdout.len())
            .ok_or_else(|| {
                anyhow!(
                    "frame for key {i} claims {len} bytes but only {} remain",
                    stdout.len().saturating_sub(pos)
                )
            })?;
        vals.push(Some(stdout[pos..end].to_vec()));
        pos = end;
    }
    Ok(vals)
}

/// `--gpu exec name ns/key`. Delegates to `nkv_vfu_gpu <traddr> exec <key>
/// <op_id>`. The GPU binary takes no exec input payload, so a provided `-i` is
/// rejected. Returns the binary's own stdout (it pretty-prints bytecount/identity
/// itself) for the caller to relay.
pub fn exec(
    cfg: &Config,
    nsid: u32,
    key: &str,
    op_id: u32,
    input_present: bool,
) -> Result<Vec<u8>> {
    if input_present {
        bail!(
            "--gpu exec does not support an input payload (-i); nkv_vfu_gpu \
             passes no exec input. Use the CPU datapath (drop --gpu)"
        );
    }
    let bin = resolve_gpu_bin(cfg)?;

    let out = Command::new(&bin)
        .env("NKVX_NSID", nsid.to_string())
        .arg(cfg.traddr())
        .arg("exec")
        .arg(key)
        .arg(op_id.to_string())
        .output()
        .with_context(|| format!("spawning {}", bin.display()))?;
    if !out.status.success() {
        bail!(
            "nkv_vfu_gpu exec failed ({}): {}",
            out.status,
            String::from_utf8_lossy(&out.stderr).trim()
        );
    }
    Ok(out.stdout)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(v: &[u8]) -> Vec<u8> {
        let mut b = (v.len() as u32).to_le_bytes().to_vec();
        b.extend_from_slice(v);
        b
    }

    #[test]
    fn parse_framed_in_order() {
        let mut s = Vec::new();
        s.extend_from_slice(&frame(b"alpha"));
        s.extend_from_slice(&frame(b""));
        s.extend_from_slice(&frame(b"gamma"));
        let got = parse_framed(&s, 3).unwrap();
        assert_eq!(got[0].as_deref(), Some(&b"alpha"[..]));
        assert_eq!(got[1].as_deref(), Some(&b""[..]));
        assert_eq!(got[2].as_deref(), Some(&b"gamma"[..]));
    }

    #[test]
    fn parse_framed_notfound_sentinel() {
        let mut s = Vec::new();
        s.extend_from_slice(&NKVX_NOTFOUND_LEN.to_le_bytes());
        s.extend_from_slice(&frame(b"v"));
        let got = parse_framed(&s, 2).unwrap();
        assert_eq!(got[0], None);
        assert_eq!(got[1].as_deref(), Some(&b"v"[..]));
    }

    #[test]
    fn parse_framed_truncated_errors() {
        let s = frame(b"abc");
        // Claim 2 keys but only one frame present.
        assert!(parse_framed(&s, 2).is_err());
        // A frame whose payload is short.
        let mut bad = 5u32.to_le_bytes().to_vec();
        bad.extend_from_slice(b"ab");
        assert!(parse_framed(&bad, 1).is_err());
    }
}
