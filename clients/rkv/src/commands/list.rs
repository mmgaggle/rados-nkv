// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM Corporation. All rights reserved.
//! `list` — list configured namespaces, or enumerate the keys in a namespace
//! via the target's `nvmf_ns_kv_list` control RPC (bead spdk-jhk.7.13).
//!
//!   * `list` (bare) prints the configured `[namespaces]` from `~/.rados-nkv.conf`
//!     — name and nsid, one per line, to stdout.
//!   * `list <ns>` resolves the namespace to an nsid (a real check — an unknown
//!     name still errors with the usual "run ns create" hint), then calls
//!     `nvmf_ns_kv_list` over JSON-RPC and prints the keys to stdout, one per
//!     line, in the backend's stable order. An empty namespace prints nothing.
//!     Backends that cannot enumerate (e.g. the rados kvdev) make the target
//!     return a clear "not supported" error which is surfaced verbatim; no key
//!     list is ever fabricated.
//!
//! Keys are 1-16 binary bytes. The target returns each key hex-encoded; we
//! decode and print it as UTF-8 when it is printable ASCII, otherwise as a
//! `0x…` hex literal so binary keys are still unambiguously shown.

use anyhow::{bail, Context, Result};
use serde::Serialize;

use crate::config::Config;
use crate::rpc::RpcClient;

/// `rados-nkv list [ns]`.
pub fn run(cfg: &Config, ns: Option<&str>) -> Result<()> {
    match ns {
        None => list_namespaces(cfg),
        Some(name) => list_keys(cfg, name),
    }
}

/// Bare `list`: print configured namespaces (`name<TAB>nsid`) to stdout.
fn list_namespaces(cfg: &Config) -> Result<()> {
    if cfg.namespaces.is_empty() {
        eprintln!(
            "no namespaces configured in ~/.rados-nkv.conf [namespaces]; \
             run 'rados-nkv ns create <name> --kvdev <dev>' to add one"
        );
        return Ok(());
    }
    // BTreeMap iterates in sorted key order, giving stable output.
    for (name, nsid) in &cfg.namespaces {
        println!("{name}\t{nsid}");
    }
    Ok(())
}

/// Params for `nvmf_ns_kv_list`. `start_key` (hex) and `limit` page the result.
#[derive(Serialize)]
struct KvListParams<'a> {
    nqn: &'a str,
    nsid: u32,
    #[serde(skip_serializing_if = "Option::is_none")]
    start_key: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    limit: Option<u32>,
}

/// Result of `nvmf_ns_kv_list`: hex-encoded keys plus a truncation flag.
#[derive(serde::Deserialize)]
struct KvListResult {
    keys: Vec<String>,
    more: bool,
}

/// `list <ns>`: resolve the nsid (real check), then enumerate the namespace's
/// keys via `nvmf_ns_kv_list`, following the `more` flag to page through large
/// key sets. Prints one key per line to stdout.
fn list_keys(cfg: &Config, name: &str) -> Result<()> {
    let nqn = cfg.nqn.as_deref().context(
        "no [target] nqn in ~/.rados-nkv.conf; set it (e.g. nqn = nqn.2026-06.io.spdk:nkvx)",
    )?;
    let nsid = cfg.resolve_ns(name).with_context(|| {
        format!(
            "unknown namespace '{name}': add it to ~/.rados-nkv.conf [namespaces] \
             or run 'rados-nkv ns create {name}'"
        )
    })?;

    let client = RpcClient::new(cfg.rpc_sock());
    let mut start_key: Option<String> = None;

    loop {
        let res: KvListResult = client
            .call(
                "nvmf_ns_kv_list",
                KvListParams {
                    nqn,
                    nsid,
                    start_key: start_key.as_deref(),
                    limit: None,
                },
            )
            .with_context(|| format!("enumerating keys of '{name}' (nsid {nsid})"))?;

        for hex in &res.keys {
            println!("{}", format_key(hex)?);
        }

        if !res.more {
            break;
        }
        // Resume strictly after the last key returned in this page. The backend
        // begins at-or-after start_key, so re-using the last key as the start
        // would re-emit it; advance to its byte-successor to skip it.
        let last = res
            .keys
            .last()
            .context("target reported more keys but returned none")?;
        start_key = Some(next_hex_key(last)?);
    }

    Ok(())
}

/// Render a hex-encoded key for display: printable ASCII as-is, otherwise a
/// `0x…` hex literal so binary keys remain visible and unambiguous.
fn format_key(hex: &str) -> Result<String> {
    let bytes = decode_hex(hex)?;
    if !bytes.is_empty() && bytes.iter().all(|&b| (0x20..0x7f).contains(&b)) {
        Ok(String::from_utf8(bytes).expect("verified printable ASCII"))
    } else {
        Ok(format!("0x{hex}"))
    }
}

/// Compute the hex of the lexicographically next key after `hex`, used as the
/// resume position for pagination. Appends a 0x00 byte (the smallest possible
/// successor) when there is room, otherwise increments the last byte; this is a
/// strict successor in the backend's byte order so the prior page's last key is
/// not re-emitted.
fn next_hex_key(hex: &str) -> Result<String> {
    let mut bytes = decode_hex(hex)?;
    // A 16-byte key is at the max length: bump the last byte instead of growing.
    if bytes.len() < 16 {
        bytes.push(0x00);
    } else {
        // Increment with carry; if every byte is 0xff there is no successor and
        // enumeration is effectively complete, so reuse the key (the backend
        // will return it again, but `more` will then be false).
        let mut i = bytes.len();
        while i > 0 {
            i -= 1;
            if bytes[i] != 0xff {
                bytes[i] += 1;
                bytes.truncate(i + 1);
                break;
            }
        }
    }
    Ok(encode_hex(&bytes))
}

/// Decode a lowercase/uppercase hex string into bytes.
fn decode_hex(hex: &str) -> Result<Vec<u8>> {
    if hex.len() % 2 != 0 {
        bail!("target returned an odd-length hex key '{hex}'");
    }
    (0..hex.len())
        .step_by(2)
        .map(|i| {
            u8::from_str_radix(&hex[i..i + 2], 16)
                .with_context(|| format!("target returned a non-hex key '{hex}'"))
        })
        .collect()
}

/// Encode bytes as a lowercase hex string.
fn encode_hex(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn printable_keys_render_as_utf8() {
        // "k1" -> 0x6b31
        assert_eq!(format_key("6b31").unwrap(), "k1");
        assert_eq!(format_key("6b766b657930").unwrap(), "kvkey0");
    }

    #[test]
    fn binary_keys_render_as_hex_literal() {
        // 0x00ff is not printable ASCII.
        assert_eq!(format_key("00ff").unwrap(), "0x00ff");
        // A control byte (0x01) forces hex rendering.
        assert_eq!(format_key("0141").unwrap(), "0x0141");
    }

    #[test]
    fn next_key_appends_zero_when_room() {
        // Successor of "k1" (6b31) is "6b3100".
        assert_eq!(next_hex_key("6b31").unwrap(), "6b3100");
    }

    #[test]
    fn next_key_increments_last_byte_at_max_len() {
        // 16 bytes, last byte 0x00 -> increment to 0x01.
        let max = "0".repeat(32); // 16 zero bytes
        let next = next_hex_key(&max).unwrap();
        assert_eq!(next, format!("{}01", "0".repeat(30)));
    }

    #[test]
    fn odd_length_hex_is_rejected() {
        assert!(decode_hex("abc").is_err());
        assert!(format_key("abc").is_err());
    }
}
