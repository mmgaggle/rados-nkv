//! `ns/key` path parsing (bead spdk-jhk.7.2).
//!
//! A path is `namespace/key`. A bare `namespace` (no slash) parses with
//! `key == None`, per the gist's "store under namespace" form. The key may itself
//! contain `/` (we split only on the first separator), so `myns/a/b/c` is
//! namespace `myns`, key `a/b/c`.

use anyhow::{bail, Result};

/// A parsed `ns/key` argument.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct KvPath {
    /// Friendly namespace name (resolved to nsid via config).
    pub ns: String,
    /// Key within the namespace, or `None` for a bare `ns`.
    pub key: Option<String>,
}

impl KvPath {
    /// Parse `"ns/key"` or `"ns"`. Errors on empty input, empty namespace, or an
    /// empty key (a trailing slash like `myns/`).
    pub fn parse(s: &str) -> Result<Self> {
        if s.is_empty() {
            bail!("empty path: expected 'ns/key' or 'ns'");
        }
        match s.split_once('/') {
            None => {
                // bare namespace
                if s.is_empty() {
                    bail!("empty namespace in path '{s}'");
                }
                Ok(KvPath {
                    ns: s.to_string(),
                    key: None,
                })
            }
            Some((ns, key)) => {
                if ns.is_empty() {
                    bail!("empty namespace in path '{s}' (expected 'ns/key')");
                }
                if key.is_empty() {
                    bail!("empty key in path '{s}' (trailing slash; expected 'ns/key')");
                }
                Ok(KvPath {
                    ns: ns.to_string(),
                    key: Some(key.to_string()),
                })
            }
        }
    }

    /// Like [`parse`], but requires a key (most commands need one).
    pub fn parse_with_key(s: &str) -> Result<(String, String)> {
        let p = Self::parse(s)?;
        match p.key {
            Some(k) => Ok((p.ns, k)),
            None => bail!("path '{s}' is missing a key (expected 'ns/key')"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ns_and_key() {
        let p = KvPath::parse("myns/k1").unwrap();
        assert_eq!(p.ns, "myns");
        assert_eq!(p.key.as_deref(), Some("k1"));
    }

    #[test]
    fn bare_ns() {
        let p = KvPath::parse("myns").unwrap();
        assert_eq!(p.ns, "myns");
        assert_eq!(p.key, None);
    }

    #[test]
    fn key_with_slashes() {
        let p = KvPath::parse("myns/a/b/c").unwrap();
        assert_eq!(p.ns, "myns");
        assert_eq!(p.key.as_deref(), Some("a/b/c"));
    }

    #[test]
    fn malformed() {
        assert!(KvPath::parse("").is_err());
        assert!(KvPath::parse("/k1").is_err());
        assert!(KvPath::parse("myns/").is_err());
        assert!(KvPath::parse_with_key("myns").is_err());
    }
}
