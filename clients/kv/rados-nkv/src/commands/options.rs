// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM Corporation. All rights reserved.
//! `-o KEY[=VAL]` option parsing for `store` / `get` (bead spdk-jhk.7.5,
//! server semantics added in spdk-jhk.7.14).
//!
//! The gist sketches namespace/object options:
//!
//! ```text
//! rados-nkv store myns/key -o ttl=24h -o ephemeral
//! rados-nkv get   myns/key -o prefetch
//! ```
//!
//! As of spdk-jhk.7.14 the in-memory target HONOURS `ttl`, `ephemeral`, and
//! `touch`: they are encoded into the KV Store command (CDW11 Store Option bits +
//! CDW12 TTL seconds) and acted on server-side (lazy TTL expiry; ephemeral
//! marker; touch refreshes TTL/access without changing the value). `prefetch`
//! remains a read hint with no in-memory meaning, so it is still a DOCUMENTED
//! no-op (the CLI says so explicitly rather than claiming a semantic it lacks).
//! Every option is still parsed and validated (unknown keys, malformed `ttl`
//! durations, and stray values on flag-style options are hard errors).
//!
//! Supported store options: `ephemeral`, `ttl=DURATION`, `touch`, `prefetch`.
//! Supported get options:   `prefetch`.
//!
//! `DURATION` is `<n><unit>` with unit `s`/`m`/`h`/`d` (e.g. `30s`, `15m`, `24h`,
//! `7d`). A bare integer is rejected (units are mandatory) so an ambiguous `ttl=1`
//! never silently means "1 second".

use std::time::Duration;

use anyhow::{bail, Result};

// KV Store CDW11 "Store Option" bits, mirroring enum spdk_nvme_kv_store_option in
// include/spdk/nvme_spec.h. Only the vendor TTL/ephemeral/touch bits are set by
// this CLI; SIKE/SINKE (bits 0/1) are not exposed here.
/// TTL Valid: CDW12 carries a TTL in seconds.
pub const KV_STORE_OPT_TTL_VALID: u8 = 1 << 3;
/// Ephemeral: mark the entry non-durable.
pub const KV_STORE_OPT_EPHEMERAL: u8 = 1 << 4;
/// Touch: refresh an existing key's TTL/access without changing its value.
pub const KV_STORE_OPT_TOUCH: u8 = 1 << 5;

/// Parsed `store -o` options. `ttl`/`ephemeral`/`touch` are honoured server-side
/// (spdk-jhk.7.14); `prefetch` remains a documented no-op hint. See module docs.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct StoreOptions {
    /// `-o ephemeral`: mark the object non-durable (honoured server-side).
    pub ephemeral: bool,
    /// `-o ttl=DURATION`: requested time-to-live (honoured server-side).
    pub ttl: Option<Duration>,
    /// `-o touch`: refresh TTL/access without changing the value (honoured).
    pub touch: bool,
    /// `-o prefetch`: hint to warm caches for a subsequent read (no-op hint).
    pub prefetch: bool,
}

/// Parsed `get -o` options.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct GetOptions {
    /// `-o prefetch`: hint to warm caches.
    pub prefetch: bool,
}

/// Split one raw `-o` token into `(key, Option<value>)`, lowercasing the key.
/// `ttl=24h` -> (`"ttl"`, Some("24h")); `ephemeral` -> (`"ephemeral"`, None).
fn split_kv(raw: &str) -> Result<(String, Option<&str>)> {
    match raw.split_once('=') {
        Some((k, v)) => {
            let k = k.trim();
            if k.is_empty() {
                bail!("malformed -o option '{raw}': empty key");
            }
            Ok((k.to_ascii_lowercase(), Some(v.trim())))
        }
        None => {
            let k = raw.trim();
            if k.is_empty() {
                bail!("malformed -o option '{raw}': empty key");
            }
            Ok((k.to_ascii_lowercase(), None))
        }
    }
}

/// Reject a value on a flag-style option (`-o ephemeral=1` is a user error).
fn no_value(key: &str, val: Option<&str>) -> Result<()> {
    if val.is_some() {
        bail!("-o {key} takes no value (got '{key}={}')", val.unwrap());
    }
    Ok(())
}

impl StoreOptions {
    /// Parse a list of raw `-o` tokens for `store`. Unknown keys are hard errors
    /// (no silent acceptance), as are malformed durations and stray values.
    pub fn parse(raw: &[String]) -> Result<Self> {
        let mut o = StoreOptions::default();
        for token in raw {
            let (key, val) = split_kv(token)?;
            match key.as_str() {
                "ephemeral" => {
                    no_value(&key, val)?;
                    o.ephemeral = true;
                }
                "touch" => {
                    no_value(&key, val)?;
                    o.touch = true;
                }
                "prefetch" => {
                    no_value(&key, val)?;
                    o.prefetch = true;
                }
                "ttl" => {
                    let v = val.ok_or_else(|| {
                        anyhow::anyhow!("-o ttl requires a duration (e.g. -o ttl=24h)")
                    })?;
                    o.ttl = Some(parse_duration(v)?);
                }
                other => bail!(
                    "unknown store option '-o {other}' \
                     (supported: ephemeral, ttl=DURATION, touch, prefetch)"
                ),
            }
        }
        Ok(o)
    }

    /// Summary of the options that are HONOURED server-side (ttl/ephemeral/touch).
    pub fn honored_summary(&self) -> String {
        let mut parts = Vec::new();
        if let Some(ttl) = self.ttl {
            parts.push(format!("ttl={}s", ttl.as_secs()));
        }
        if self.ephemeral {
            parts.push("ephemeral".to_string());
        }
        if self.touch {
            parts.push("touch".to_string());
        }
        parts.join(", ")
    }

    /// True if any HONOURED option (ttl/ephemeral/touch) was set.
    pub fn has_honored(&self) -> bool {
        self.ttl.is_some() || self.ephemeral || self.touch
    }

    /// Encode the honoured options into the wire form: the KV Store CDW11 Store
    /// Option byte and the CDW12 TTL in seconds. A TTL is clamped to u32 seconds
    /// (the CDW12 width); the duration parser already rejects overflow into u64.
    pub fn wire(&self) -> (u8, u32) {
        let mut opt: u8 = 0;
        let mut ttl_secs: u32 = 0;
        if let Some(ttl) = self.ttl {
            opt |= KV_STORE_OPT_TTL_VALID;
            ttl_secs = ttl.as_secs().min(u32::MAX as u64) as u32;
        }
        if self.ephemeral {
            opt |= KV_STORE_OPT_EPHEMERAL;
        }
        if self.touch {
            opt |= KV_STORE_OPT_TOUCH;
        }
        (opt, ttl_secs)
    }
}

impl GetOptions {
    /// Parse a list of raw `-o` tokens for `get`. Only `prefetch` is meaningful.
    pub fn parse(raw: &[String]) -> Result<Self> {
        let mut o = GetOptions::default();
        for token in raw {
            let (key, val) = split_kv(token)?;
            match key.as_str() {
                "prefetch" => {
                    no_value(&key, val)?;
                    o.prefetch = true;
                }
                other => bail!(
                    "unknown get option '-o {other}' (supported: prefetch)"
                ),
            }
        }
        Ok(o)
    }
}

/// Parse a `ttl` duration string: `<n><unit>` with unit one of `s`/`m`/`h`/`d`.
///
/// Units are mandatory (a bare integer is rejected) so `ttl=1` can never silently
/// mean "1 second". The numeric part must be a non-negative integer that fits the
/// resulting seconds in a `u64`.
pub fn parse_duration(s: &str) -> Result<Duration> {
    let s = s.trim();
    if s.is_empty() {
        bail!("empty duration");
    }
    let (num_str, unit) = s.split_at(
        s.find(|c: char| !c.is_ascii_digit())
            .unwrap_or(s.len()),
    );
    if num_str.is_empty() {
        bail!("duration '{s}' has no numeric part (expected e.g. 24h)");
    }
    let n: u64 = num_str
        .parse()
        .map_err(|_| anyhow::anyhow!("duration '{s}': '{num_str}' is not a valid integer"))?;
    let mult: u64 = match unit {
        "s" => 1,
        "m" => 60,
        "h" => 60 * 60,
        "d" => 60 * 60 * 24,
        "" => bail!("duration '{s}' is missing a unit (use s/m/h/d, e.g. 24h)"),
        other => bail!("duration '{s}': unknown unit '{other}' (use s/m/h/d)"),
    };
    let secs = n
        .checked_mul(mult)
        .ok_or_else(|| anyhow::anyhow!("duration '{s}' overflows"))?;
    Ok(Duration::from_secs(secs))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v(items: &[&str]) -> Vec<String> {
        items.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn duration_units() {
        assert_eq!(parse_duration("30s").unwrap(), Duration::from_secs(30));
        assert_eq!(parse_duration("15m").unwrap(), Duration::from_secs(900));
        assert_eq!(parse_duration("24h").unwrap(), Duration::from_secs(86_400));
        assert_eq!(parse_duration("7d").unwrap(), Duration::from_secs(604_800));
    }

    #[test]
    fn duration_rejects_bad() {
        assert!(parse_duration("1").is_err()); // no unit
        assert!(parse_duration("").is_err());
        assert!(parse_duration("h").is_err()); // no number
        assert!(parse_duration("10y").is_err()); // bad unit
        assert!(parse_duration("-5s").is_err()); // '-' not a digit -> empty num
        assert!(parse_duration("99999999999999999999d").is_err()); // overflow
    }

    #[test]
    fn store_opts_full() {
        let o = StoreOptions::parse(&v(&["ephemeral", "ttl=24h", "touch", "prefetch"])).unwrap();
        assert!(o.ephemeral);
        assert_eq!(o.ttl, Some(Duration::from_secs(86_400)));
        assert!(o.touch);
        assert!(o.prefetch);
        assert!(o.has_honored());
        assert!(o.honored_summary().contains("ttl=86400s"));
    }

    #[test]
    fn store_opts_wire_encoding() {
        // ttl=2s + ephemeral + touch -> all three bits set, CDW12 = 2.
        let o = StoreOptions::parse(&v(&["ttl=2s", "ephemeral", "touch"])).unwrap();
        let (opt, ttl) = o.wire();
        assert_eq!(
            opt,
            KV_STORE_OPT_TTL_VALID | KV_STORE_OPT_EPHEMERAL | KV_STORE_OPT_TOUCH
        );
        assert_eq!(ttl, 2);
        assert!(o.has_honored());
        assert_eq!(o.honored_summary(), "ttl=2s, ephemeral, touch");

        // No options -> bytes are zero (backward compatible plain store).
        let none = StoreOptions::default();
        assert_eq!(none.wire(), (0, 0));
        assert!(!none.has_honored());

        // prefetch alone is NOT honoured (stays a hint): no wire bits.
        let pf = StoreOptions::parse(&v(&["prefetch"])).unwrap();
        assert_eq!(pf.wire(), (0, 0));
        assert!(!pf.has_honored());
        assert!(pf.prefetch);

        // ttl only -> just TTL_VALID + the seconds.
        let t = StoreOptions::parse(&v(&["ttl=1h"])).unwrap();
        assert_eq!(t.wire(), (KV_STORE_OPT_TTL_VALID, 3600));
    }

    #[test]
    fn store_opts_case_insensitive() {
        let o = StoreOptions::parse(&v(&["EPHEMERAL", "TTL=1h"])).unwrap();
        assert!(o.ephemeral);
        assert_eq!(o.ttl, Some(Duration::from_secs(3600)));
    }

    #[test]
    fn store_opts_reject_unknown() {
        assert!(StoreOptions::parse(&v(&["bogus"])).is_err());
    }

    #[test]
    fn store_opts_reject_value_on_flag() {
        assert!(StoreOptions::parse(&v(&["ephemeral=1"])).is_err());
    }

    #[test]
    fn store_opts_ttl_requires_value() {
        assert!(StoreOptions::parse(&v(&["ttl"])).is_err());
    }

    #[test]
    fn store_opts_empty() {
        let o = StoreOptions::parse(&[]).unwrap();
        assert!(!o.has_honored());
        assert!(!o.prefetch);
        assert_eq!(o.honored_summary(), "");
    }

    #[test]
    fn get_opts() {
        let o = GetOptions::parse(&v(&["prefetch"])).unwrap();
        assert!(o.prefetch);
        assert!(GetOptions::parse(&v(&["ttl=1h"])).is_err()); // not valid for get
        assert!(GetOptions::parse(&v(&["prefetch=1"])).is_err());
    }
}
