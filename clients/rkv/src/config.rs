// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM Corporation. All rights reserved.
//! `~/.rados-nkv.conf` parsing and persistence (bead spdk-jhk.7.2).
//!
//! A small hand-rolled INI reader/writer (no external dep) covering the sections
//! the CLI needs:
//!
//! ```ini
//! [target]
//! traddr   = /tmp/nkvx/muser/0      # vfio-user cntrl dir (datapath)
//! rpc_sock = /tmp/nkvx/rpc.sock     # JSON-RPC (control)
//! nqn      = nqn.2026-06.io.spdk:nkvx
//! [namespaces]                      # friendly name -> nvme nsid
//! default = 1
//! myns    = 1
//! [exec]                            # op name -> op_id
//! bytecount = 10
//! identity  = 11
//! [opcodes]                         # informational (raw opcode -> label) for v1
//! 83 = exec
//! ```
//!
//! If the file is missing we fall back to a sane default `traddr`, but we do
//! **not** invent namespaces: an unknown `ns` must be a hard error so callers are
//! told to run `rados-nkv ns create <ns>`.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result};

/// Default vfio-user listener dir if `[target] traddr` is unset / no config file.
pub const DEFAULT_TRADDR: &str = "/tmp/nkvx/muser/0";

/// Default JSON-RPC control socket if `[target] rpc_sock` is unset.
pub const DEFAULT_RPC_SOCK: &str = "/tmp/nkvx/rpc.sock";

/// Parsed `~/.rados-nkv.conf`.
#[derive(Debug, Default, Clone)]
pub struct Config {
    /// `[target] traddr` (vfio-user cntrl dir). `None` => use [`DEFAULT_TRADDR`].
    pub traddr: Option<String>,
    /// `[target] rpc_sock` (JSON-RPC unix socket for control ops).
    pub rpc_sock: Option<String>,
    /// `[target] nqn`.
    pub nqn: Option<String>,
    /// `[target] gpu_bin` (path to the `nkv_vfu_gpu` binary for `--gpu`).
    /// `None` => resolve `nkv_vfu_gpu` from `PATH`.
    pub gpu_bin: Option<String>,
    /// `[target] daemon_sock` (unix socket for the persistent session daemon).
    /// `None` => use `$HOME/.rados-nkv.sock` (bead spdk-jhk.7.11).
    pub daemon_sock: Option<String>,
    /// `[namespaces]` friendly-name -> nvme nsid.
    pub namespaces: BTreeMap<String, u32>,
    /// `[exec]` op-name -> op_id.
    pub exec: BTreeMap<String, u32>,
    /// `[opcodes]` raw opcode -> label (informational; preserved on save).
    pub opcodes: BTreeMap<String, String>,
}

impl Config {
    /// Path to the per-user config file (`$HOME/.rados-nkv.conf`).
    pub fn default_path() -> Result<PathBuf> {
        let home = std::env::var_os("HOME").context("HOME is not set")?;
        Ok(Path::new(&home).join(".rados-nkv.conf"))
    }

    /// Load from the default path. A missing file yields defaults (empty maps).
    pub fn load() -> Result<Self> {
        let path = Self::default_path()?;
        Self::load_from(&path)
    }

    /// Load from a specific path. A missing file yields a default config.
    pub fn load_from(path: &Path) -> Result<Self> {
        match fs::read_to_string(path) {
            Ok(text) => Self::parse(&text)
                .with_context(|| format!("parsing config {}", path.display())),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Config::default()),
            Err(e) => Err(e).with_context(|| format!("reading config {}", path.display())),
        }
    }

    /// Parse INI text. Lines: `[section]`, `key = value`, `# comment`/`; comment`,
    /// blank. Inline `#`/`;` after a value is stripped. Unknown sections are kept
    /// only if recognized; others are ignored.
    pub fn parse(text: &str) -> Result<Self> {
        let mut cfg = Config::default();
        let mut section = String::new();

        for (lineno, raw) in text.lines().enumerate() {
            let line = strip_inline_comment(raw).trim();
            if line.is_empty() {
                continue;
            }
            if let Some(inner) = line.strip_prefix('[').and_then(|s| s.strip_suffix(']')) {
                section = inner.trim().to_ascii_lowercase();
                continue;
            }
            let (key, val) = line
                .split_once('=')
                .with_context(|| format!("config line {}: expected 'key = value'", lineno + 1))?;
            let key = key.trim();
            let val = val.trim();
            if key.is_empty() {
                anyhow::bail!("config line {}: empty key", lineno + 1);
            }

            match section.as_str() {
                "target" => match key {
                    "traddr" => cfg.traddr = Some(val.to_string()),
                    "rpc_sock" => cfg.rpc_sock = Some(val.to_string()),
                    "nqn" => cfg.nqn = Some(val.to_string()),
                    "gpu_bin" => cfg.gpu_bin = Some(val.to_string()),
                    "daemon_sock" => cfg.daemon_sock = Some(val.to_string()),
                    _ => {} // ignore unknown target keys
                },
                "namespaces" => {
                    let nsid: u32 = val.parse().with_context(|| {
                        format!("config line {}: namespace '{key}' nsid must be an integer", lineno + 1)
                    })?;
                    cfg.namespaces.insert(key.to_string(), nsid);
                }
                "exec" => {
                    let op: u32 = val.parse().with_context(|| {
                        format!("config line {}: exec '{key}' op_id must be an integer", lineno + 1)
                    })?;
                    cfg.exec.insert(key.to_string(), op);
                }
                "opcodes" => {
                    cfg.opcodes.insert(key.to_string(), val.to_string());
                }
                _ => {} // ignore unknown sections
            }
        }
        Ok(cfg)
    }

    /// Serialize back to INI text (stable section order).
    pub fn to_ini(&self) -> String {
        let mut out = String::new();
        out.push_str("[target]\n");
        if let Some(v) = &self.traddr {
            out.push_str(&format!("traddr = {v}\n"));
        }
        if let Some(v) = &self.rpc_sock {
            out.push_str(&format!("rpc_sock = {v}\n"));
        }
        if let Some(v) = &self.nqn {
            out.push_str(&format!("nqn = {v}\n"));
        }
        if let Some(v) = &self.gpu_bin {
            out.push_str(&format!("gpu_bin = {v}\n"));
        }
        if let Some(v) = &self.daemon_sock {
            out.push_str(&format!("daemon_sock = {v}\n"));
        }
        out.push_str("\n[namespaces]\n");
        for (k, v) in &self.namespaces {
            out.push_str(&format!("{k} = {v}\n"));
        }
        out.push_str("\n[exec]\n");
        for (k, v) in &self.exec {
            out.push_str(&format!("{k} = {v}\n"));
        }
        if !self.opcodes.is_empty() {
            out.push_str("\n[opcodes]\n");
            for (k, v) in &self.opcodes {
                out.push_str(&format!("{k} = {v}\n"));
            }
        }
        out
    }

    /// Persist to the default path.
    pub fn save(&self) -> Result<()> {
        let path = Self::default_path()?;
        self.save_to(&path)
    }

    /// Persist to a specific path.
    pub fn save_to(&self, path: &Path) -> Result<()> {
        fs::write(path, self.to_ini())
            .with_context(|| format!("writing config {}", path.display()))
    }

    /// Resolve a friendly namespace name to its nvme nsid.
    pub fn resolve_ns(&self, name: &str) -> Option<u32> {
        self.namespaces.get(name).copied()
    }

    /// Resolve an exec op-name to its op_id.
    pub fn resolve_exec(&self, name: &str) -> Option<u32> {
        self.exec.get(name).copied()
    }

    /// The effective vfio-user traddr (config value or [`DEFAULT_TRADDR`]).
    pub fn traddr(&self) -> &str {
        self.traddr.as_deref().unwrap_or(DEFAULT_TRADDR)
    }

    /// The effective JSON-RPC control socket (config value or [`DEFAULT_RPC_SOCK`]).
    pub fn rpc_sock(&self) -> &str {
        self.rpc_sock.as_deref().unwrap_or(DEFAULT_RPC_SOCK)
    }

    /// The effective persistent-session-daemon socket path: the `[target]
    /// daemon_sock` value, else `$HOME/.rados-nkv.sock` (bead spdk-jhk.7.11).
    pub fn daemon_sock(&self) -> Result<PathBuf> {
        if let Some(v) = &self.daemon_sock {
            return Ok(PathBuf::from(v));
        }
        let home = std::env::var_os("HOME").context("HOME is not set")?;
        Ok(Path::new(&home).join(".rados-nkv.sock"))
    }
}

/// Strip a trailing `#`/`;` comment that is not inside the value's first token.
/// We keep it simple: any `#`/`;` preceded by whitespace (or at line start)
/// begins a comment. This avoids clobbering values that legitimately contain `#`
/// mid-token (e.g. nqn fragments), while still honoring trailing comments.
fn strip_inline_comment(line: &str) -> &str {
    let bytes = line.as_bytes();
    let mut prev_ws = true; // treat start-of-line as preceded by whitespace
    for (i, &b) in bytes.iter().enumerate() {
        if (b == b'#' || b == b';') && prev_ws {
            return &line[..i];
        }
        prev_ws = b == b' ' || b == b'\t';
    }
    line
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_full() {
        let cfg = Config::parse(
            "[target]\n\
             traddr = /tmp/nkvx/muser/0\n\
             rpc_sock = /tmp/nkvx/rpc.sock  # control\n\
             nqn = nqn.2026-06.io.spdk:nkvx\n\
             [namespaces]\n\
             default = 1\n\
             myns = 2\n\
             [exec]\n\
             bytecount = 10\n\
             [opcodes]\n\
             83 = exec\n",
        )
        .unwrap();
        assert_eq!(cfg.traddr(), "/tmp/nkvx/muser/0");
        assert_eq!(cfg.rpc_sock.as_deref(), Some("/tmp/nkvx/rpc.sock"));
        assert_eq!(cfg.nqn.as_deref(), Some("nqn.2026-06.io.spdk:nkvx"));
        assert_eq!(cfg.resolve_ns("default"), Some(1));
        assert_eq!(cfg.resolve_ns("myns"), Some(2));
        assert_eq!(cfg.resolve_ns("nope"), None);
        assert_eq!(cfg.resolve_exec("bytecount"), Some(10));
        assert_eq!(cfg.opcodes.get("83").map(String::as_str), Some("exec"));
    }

    #[test]
    fn missing_file_defaults() {
        let cfg = Config::load_from(Path::new("/nonexistent/rados-nkv.conf")).unwrap();
        assert_eq!(cfg.traddr(), DEFAULT_TRADDR);
        assert!(cfg.namespaces.is_empty());
    }

    #[test]
    fn roundtrip_ini() {
        let cfg = Config::parse("[target]\ntraddr = x\n[namespaces]\na = 3\n[exec]\nb = 7\n").unwrap();
        let text = cfg.to_ini();
        let cfg2 = Config::parse(&text).unwrap();
        assert_eq!(cfg2.resolve_ns("a"), Some(3));
        assert_eq!(cfg2.resolve_exec("b"), Some(7));
        assert_eq!(cfg2.traddr(), "x");
    }
}
