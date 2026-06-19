//! KV-Exec allowlist policy file (`policy.yaml`) parsing (bead spdk-jhk.7.4).
//!
//! The policy file mirrors the JSON-RPC `nvmf_ns_set_kv_exec_allowlist`
//! `allowlist` array, one YAML mapping per entry. Each entry pins an exec
//! `op_id` to a binding. Two binding encodings are accepted, matching the
//! target's decoder (`lib/nvmf/nvmf_rpc.c`, `rpc_nvmf_kv_exec_allow`):
//!
//! Structured (preferred):
//! ```yaml
//! allowlist:
//!   - op_id: 10
//!     runtime: cls            # "cls" or "wasm"
//!     module_namespace: nkvx  # e.g. RADOS object-class name
//!     module_key: bytecount   # e.g. class method / wasm export
//!     sha256: <64 hex chars>  # optional integrity pin
//!     caps: 0                 # optional capability tier (uint64)
//! ```
//!
//! Legacy `class:method` (deprecated; maps to runtime=cls):
//! ```yaml
//! allowlist:
//!   - op_id: 11
//!     binding: "nkvx:identity"
//! ```
//!
//! A bare top-level list (no `allowlist:` key) is also accepted for brevity.
//! Fields left unset are omitted from the RPC, exactly as the C decoder treats
//! its optional members; only `op_id` is mandatory. Mutual-exclusion of the
//! legacy `binding` string and the structured fields is enforced server-side,
//! but we reject the obvious local mistake early to give a better message.

use std::fs;
use std::path::Path;

use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};

/// One allowlist entry, serialized straight into the RPC `allowlist` array.
///
/// `#[serde(default)]` + `skip_serializing_if` make every field but `op_id`
/// optional on both the YAML-in and JSON-out sides, matching the decoder.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct AllowEntry {
    pub op_id: u32,

    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub binding: Option<String>,

    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub runtime: Option<String>,

    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub module_namespace: Option<String>,

    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub module_key: Option<String>,

    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sha256: Option<String>,

    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub caps: Option<u64>,
}

/// Top-level policy document: either `{ allowlist: [...] }` or a bare `[...]`.
#[derive(Debug, Deserialize)]
#[serde(untagged)]
enum PolicyDoc {
    Wrapped { allowlist: Vec<AllowEntry> },
    Bare(Vec<AllowEntry>),
}

/// Parse a policy YAML string into validated allowlist entries.
pub fn parse(text: &str) -> Result<Vec<AllowEntry>> {
    let doc: PolicyDoc = serde_yaml::from_str(text).context("parsing policy YAML")?;
    let entries = match doc {
        PolicyDoc::Wrapped { allowlist } => allowlist,
        PolicyDoc::Bare(v) => v,
    };
    for (i, e) in entries.iter().enumerate() {
        validate(e).with_context(|| format!("allowlist entry #{}", i + 1))?;
    }
    Ok(entries)
}

/// Load and parse a policy file.
pub fn load(path: &Path) -> Result<Vec<AllowEntry>> {
    let text = fs::read_to_string(path)
        .with_context(|| format!("reading policy file {}", path.display()))?;
    parse(&text)
}

/// Reject locally-detectable mistakes; the target enforces the rest.
fn validate(e: &AllowEntry) -> Result<()> {
    let has_structured = e.runtime.is_some()
        || e.module_namespace.is_some()
        || e.module_key.is_some()
        || e.sha256.is_some()
        || e.caps.is_some();

    if e.binding.is_some() && has_structured {
        bail!("legacy 'binding' and structured fields (runtime/module_*) are mutually exclusive");
    }
    if e.binding.is_none() && e.runtime.is_none() {
        bail!(
            "needs either a legacy 'binding: class:method' or a structured 'runtime' \
             (cls|wasm) with module_namespace/module_key"
        );
    }
    if let Some(b) = &e.binding {
        match b.split_once(':') {
            Some((cls, method)) if !cls.is_empty() && !method.is_empty() => {}
            _ => bail!("legacy binding '{b}' is not 'class:method'"),
        }
    }
    if let Some(sha) = &e.sha256 {
        if sha.len() != 64 || !sha.bytes().all(|c| c.is_ascii_hexdigit()) {
            bail!("sha256 '{sha}' must be 64 hex characters");
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn structured_wrapped() {
        let entries = parse(
            "allowlist:\n  - op_id: 10\n    runtime: cls\n    module_namespace: nkvx\n    module_key: bytecount\n",
        )
        .unwrap();
        assert_eq!(entries.len(), 1);
        let e = &entries[0];
        assert_eq!(e.op_id, 10);
        assert_eq!(e.runtime.as_deref(), Some("cls"));
        assert_eq!(e.module_namespace.as_deref(), Some("nkvx"));
        assert_eq!(e.module_key.as_deref(), Some("bytecount"));
        assert!(e.binding.is_none());
    }

    #[test]
    fn legacy_bare_list() {
        let entries = parse("- op_id: 11\n  binding: nkvx:identity\n").unwrap();
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].binding.as_deref(), Some("nkvx:identity"));
    }

    #[test]
    fn serializes_only_set_fields() {
        let e = AllowEntry {
            op_id: 10,
            binding: None,
            runtime: Some("cls".into()),
            module_namespace: Some("nkvx".into()),
            module_key: Some("bytecount".into()),
            sha256: None,
            caps: None,
        };
        let j = serde_json::to_value(&e).unwrap();
        assert_eq!(j["op_id"], 10);
        assert_eq!(j["runtime"], "cls");
        assert!(j.get("binding").is_none());
        assert!(j.get("sha256").is_none());
        assert!(j.get("caps").is_none());
    }

    #[test]
    fn rejects_mixed_encodings() {
        let err = parse("- op_id: 1\n  binding: a:b\n  runtime: cls\n").unwrap_err();
        assert!(format!("{err:#}").contains("mutually exclusive"));
    }

    #[test]
    fn rejects_bad_legacy_binding() {
        assert!(parse("- op_id: 1\n  binding: noseparator\n").is_err());
    }

    #[test]
    fn rejects_missing_binding_and_runtime() {
        assert!(parse("- op_id: 1\n  module_key: x\n").is_err());
    }

    #[test]
    fn rejects_bad_sha256() {
        assert!(parse("- op_id: 1\n  runtime: wasm\n  sha256: abc\n").is_err());
    }
}
