//! `ns create` and `ns allowlist` — control-plane namespace management over
//! JSON-RPC (bead spdk-jhk.7.4; create-by-name reworked in spdk-jhk.8.1).
//!
//! These two commands are the only ones that touch the SPDK target's control
//! socket. `ns create <name> -o pool=POOL` creates (or idempotently resolves) a
//! rados-backed Key-Value namespace by friendly name: the target creates a
//! kvdev bound to the pool and an isolated rados namespace (== the friendly
//! name), adds it to the subsystem, and records name->{nsid, rados_namespace}
//! durably in a rados-omap registry in the pool. The CLI never touches rados
//! directly; the target owns all registry I/O via its librados cluster handle.
//! `ns allowlist` pushes a KV-Exec allowlist (from a YAML policy) onto a ns.
//!
//! RPC contract:
//!   - `nvmf_kv_ns_create_by_name(nqn, name, pool, [cluster_name], [namespace],
//!      [nsid], [uuid], [read_only], [max_value_len]) -> nsid`
//!   - `nvmf_ns_set_kv_exec_allowlist(nqn, nsid, allowlist) -> bool`

use std::path::Path;

use anyhow::{bail, Context, Result};
use serde::Serialize;

use crate::config::Config;
use crate::policy;
use crate::rpc::RpcClient;

/// Parsed `ns create -o KEY[=VAL]` options. `pool` is required; the rest refine
/// the target-side create. Mirrors the `nvmf_kv_ns_create_by_name` params.
#[derive(Debug, Default)]
struct NsCreateOptions {
    /// `-o pool=POOL` (required): rados pool holding the data + name registry.
    pool: Option<String>,
    /// `-o cluster=NAME`: registered rados cluster handle (target default ceph0).
    cluster: Option<String>,
    /// `-o namespace=NS`: override the rados namespace (default == the name).
    namespace: Option<String>,
}

impl NsCreateOptions {
    /// Parse `KEY=VAL` option strings. Only `pool`, `cluster`, `namespace` are
    /// accepted; an unknown key or a missing `=VAL` is a hard error so a typo
    /// never silently drops a setting.
    fn parse(options: &[String]) -> Result<Self> {
        let mut out = NsCreateOptions::default();
        for opt in options {
            let (key, val) = opt.split_once('=').ok_or_else(|| {
                anyhow::anyhow!("ns create option '{opt}' needs a value (use KEY=VAL, e.g. pool=kvpool)")
            })?;
            if val.is_empty() {
                bail!("ns create option '{key}' has an empty value");
            }
            match key {
                "pool" => out.pool = Some(val.to_string()),
                "cluster" => out.cluster = Some(val.to_string()),
                "namespace" => out.namespace = Some(val.to_string()),
                other => bail!(
                    "unknown ns create option '{other}' (supported: pool=, cluster=, namespace=)"
                ),
            }
        }
        Ok(out)
    }
}

/// Build the RPC client from the config's `[target] rpc_sock`, erroring clearly
/// if it is unset (control ops have no usable default beyond the documented one).
fn rpc_client(cfg: &Config) -> RpcClient {
    RpcClient::new(cfg.rpc_sock())
}

/// Require `[target] nqn`; control ops cannot proceed without a subsystem name.
fn require_nqn(cfg: &Config) -> Result<&str> {
    cfg.nqn.as_deref().context(
        "no [target] nqn in ~/.rados-nkv.conf; set it (e.g. nqn = nqn.2026-06.io.spdk:nkvx)",
    )
}

/// Params for `nvmf_kv_ns_create_by_name`. `pool` is required; the rest are
/// elided when unset so the target applies its own defaults (cluster=ceph0,
/// rados namespace == the friendly name, target-assigned nsid).
#[derive(Serialize)]
struct CreateByNameParams<'a> {
    nqn: &'a str,
    name: &'a str,
    pool: &'a str,
    #[serde(skip_serializing_if = "Option::is_none")]
    cluster_name: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    namespace: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    nsid: Option<u32>,
}

/// `rados-nkv ns create <name> -o pool=POOL [-o cluster=NAME] [-o namespace=NS] [--nsid N]`.
///
/// Calls `nvmf_kv_ns_create_by_name`: the target creates (or idempotently
/// resolves) a rados-backed KV namespace bound to `pool` and an isolated rados
/// namespace (default == `<name>`), durably records name->{nsid, rados_namespace}
/// in the pool's omap registry, and returns the nsid. The returned nsid is also
/// cached in `~/.rados-nkv.conf [namespaces]` for offline resolution, but the
/// target registry — surfaced via `nvmf_get_subsystems` `kv_name` — is the source
/// of truth (so resolution works even with no local config). Idempotent: a repeat
/// `ns create <name> -o pool=POOL` returns the same nsid with no error.
pub fn create(
    cfg: &mut Config,
    name: &str,
    nsid: Option<u32>,
    options: &[String],
) -> Result<()> {
    let nqn = require_nqn(cfg)?.to_string();
    let opts = NsCreateOptions::parse(options)?;
    let pool = opts.pool.as_deref().context(
        "ns create needs a pool: pass -o pool=POOL (the rados pool that backs the namespace \
         and holds its name registry)",
    )?;

    let client = rpc_client(cfg);
    let assigned: u32 = client
        .call(
            "nvmf_kv_ns_create_by_name",
            CreateByNameParams {
                nqn: &nqn,
                name,
                pool,
                cluster_name: opts.cluster.as_deref(),
                namespace: opts.namespace.as_deref(),
                nsid,
            },
        )
        .context("nvmf_kv_ns_create_by_name failed")?;

    cfg.namespaces.insert(name.to_string(), assigned);
    cfg.save()
        .context("namespace was created on the target but writing ~/.rados-nkv.conf failed")?;

    println!("{assigned}");
    eprintln!(
        "created namespace '{name}' = nsid {assigned} (pool {pool}) in the target registry \
         and cached to config"
    );
    Ok(())
}

/// Parsed `ns attach -o KEY[=VAL]` options. Only `pool` (required) and
/// `cluster` are accepted: attach binds an EXISTING registry entry, so it never
/// picks a rados namespace (the recorded one is replayed) and never picks an nsid
/// (the recorded one is replayed). Rejecting `namespace=`/`nsid` here keeps the
/// restart-replay contract honest rather than silently ignoring them.
#[derive(Debug, Default)]
struct NsAttachOptions {
    /// `-o pool=POOL` (required): rados pool that holds the name registry + data.
    pool: Option<String>,
    /// `-o cluster=NAME`: registered rados cluster handle (target default ceph0).
    cluster: Option<String>,
}

impl NsAttachOptions {
    fn parse(options: &[String]) -> Result<Self> {
        let mut out = NsAttachOptions::default();
        for opt in options {
            let (key, val) = opt.split_once('=').ok_or_else(|| {
                anyhow::anyhow!("ns attach option '{opt}' needs a value (use KEY=VAL, e.g. pool=kvpool)")
            })?;
            if val.is_empty() {
                bail!("ns attach option '{key}' has an empty value");
            }
            match key {
                "pool" => out.pool = Some(val.to_string()),
                "cluster" => out.cluster = Some(val.to_string()),
                "namespace" => bail!(
                    "ns attach does not accept 'namespace=': attach replays the recorded rados \
                     namespace from the registry (it is fixed at create time)"
                ),
                other => bail!(
                    "unknown ns attach option '{other}' (supported: pool=, cluster=)"
                ),
            }
        }
        Ok(out)
    }
}

/// Params for `nvmf_kv_ns_attach_by_name`. `pool` is required; the rest are
/// elided when unset so the target applies its own defaults (cluster=ceph0). No
/// nsid/namespace: attach reproduces the recorded binding.
#[derive(Serialize)]
struct AttachByNameParams<'a> {
    nqn: &'a str,
    name: &'a str,
    pool: &'a str,
    #[serde(skip_serializing_if = "Option::is_none")]
    cluster_name: Option<&'a str>,
}

/// `rados-nkv ns attach <name> -o pool=POOL [-o cluster=NAME]`.
///
/// Calls `nvmf_kv_ns_attach_by_name`: the target reads the pool's durable omap
/// registry for `<name>`, then binds it back to the subsystem at the RECORDED
/// nsid and RECORDED rados namespace, so the same nsid + data come back across
/// target restarts / on a fresh target (restart-replay). An unknown name is a
/// clear error ("run ns create"); a name already attached is a clean no-op. The
/// returned nsid is cached in `~/.rados-nkv.conf [namespaces]` for offline
/// resolution, but the target registry remains the source of truth.
pub fn attach(cfg: &mut Config, name: &str, options: &[String]) -> Result<()> {
    let nqn = require_nqn(cfg)?.to_string();
    let opts = NsAttachOptions::parse(options)?;
    let pool = opts.pool.as_deref().context(
        "ns attach needs a pool: pass -o pool=POOL (the rados pool that holds the namespace \
         and its name registry)",
    )?;

    let client = rpc_client(cfg);
    let nsid: u32 = client
        .call(
            "nvmf_kv_ns_attach_by_name",
            AttachByNameParams {
                nqn: &nqn,
                name,
                pool,
                cluster_name: opts.cluster.as_deref(),
            },
        )
        .context("nvmf_kv_ns_attach_by_name failed")?;

    cfg.namespaces.insert(name.to_string(), nsid);
    cfg.save()
        .context("namespace was attached on the target but writing ~/.rados-nkv.conf failed")?;

    println!("{nsid}");
    eprintln!(
        "attached namespace '{name}' = nsid {nsid} (pool {pool}) from the target registry \
         and cached to config"
    );
    Ok(())
}

/// Params for `nvmf_ns_set_kv_exec_allowlist`.
#[derive(Serialize)]
struct SetAllowlistParams<'a> {
    nqn: &'a str,
    nsid: u32,
    allowlist: &'a [policy::AllowEntry],
}

/// `rados-nkv ns allowlist <name> -i policy.yaml`.
///
/// Parses the YAML policy into allowlist entries and calls
/// `nvmf_ns_set_kv_exec_allowlist` for the namespace's nsid.
pub fn allowlist(cfg: &Config, name: &str, input: &Path) -> Result<()> {
    let nqn = require_nqn(cfg)?.to_string();
    let nsid = cfg.resolve_ns(name).with_context(|| {
        format!(
            "unknown namespace '{name}': add it to [namespaces] or run 'rados-nkv ns create {name}'"
        )
    })?;

    let entries = policy::load(input)
        .with_context(|| format!("loading allowlist policy from {}", input.display()))?;
    if entries.is_empty() {
        bail!("policy {} declares no allowlist entries", input.display());
    }

    let client = rpc_client(cfg);
    let _ok: bool = client
        .call(
            "nvmf_ns_set_kv_exec_allowlist",
            SetAllowlistParams {
                nqn: &nqn,
                nsid,
                allowlist: &entries,
            },
        )
        .context("nvmf_ns_set_kv_exec_allowlist failed")?;

    eprintln!(
        "set {} allowlist entr{} on '{name}' (nsid {nsid})",
        entries.len(),
        if entries.len() == 1 { "y" } else { "ies" }
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{NsAttachOptions, NsCreateOptions};

    fn opts(args: &[&str]) -> Vec<String> {
        args.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn parses_pool_cluster_and_namespace() {
        let o = NsCreateOptions::parse(&opts(&[
            "pool=kvpool",
            "cluster=ceph1",
            "namespace=demo-ns",
        ]))
        .unwrap();
        assert_eq!(o.pool.as_deref(), Some("kvpool"));
        assert_eq!(o.cluster.as_deref(), Some("ceph1"));
        assert_eq!(o.namespace.as_deref(), Some("demo-ns"));
    }

    #[test]
    fn pool_is_the_only_required_option_others_default_to_none() {
        let o = NsCreateOptions::parse(&opts(&["pool=kvpool"])).unwrap();
        assert_eq!(o.pool.as_deref(), Some("kvpool"));
        assert!(o.cluster.is_none());
        assert!(o.namespace.is_none());
    }

    #[test]
    fn unknown_option_is_rejected() {
        let err = NsCreateOptions::parse(&opts(&["pool=kvpool", "kvdev=KvRados0"]))
            .unwrap_err()
            .to_string();
        assert!(err.contains("unknown ns create option 'kvdev'"), "{err}");
    }

    #[test]
    fn flag_form_without_value_is_rejected() {
        // `-o pool` (no `=VAL`) must error rather than silently dropping the pool.
        let err = NsCreateOptions::parse(&opts(&["pool"]))
            .unwrap_err()
            .to_string();
        assert!(err.contains("needs a value"), "{err}");
    }

    #[test]
    fn empty_value_is_rejected() {
        let err = NsCreateOptions::parse(&opts(&["pool="]))
            .unwrap_err()
            .to_string();
        assert!(err.contains("empty value"), "{err}");
    }

    #[test]
    fn attach_parses_pool_and_cluster() {
        let o = NsAttachOptions::parse(&opts(&["pool=kvpool", "cluster=ceph1"])).unwrap();
        assert_eq!(o.pool.as_deref(), Some("kvpool"));
        assert_eq!(o.cluster.as_deref(), Some("ceph1"));
    }

    #[test]
    fn attach_requires_only_pool() {
        let o = NsAttachOptions::parse(&opts(&["pool=kvpool"])).unwrap();
        assert_eq!(o.pool.as_deref(), Some("kvpool"));
        assert!(o.cluster.is_none());
    }

    #[test]
    fn attach_rejects_namespace_override() {
        // attach replays the registry-recorded rados namespace; accepting an
        // override would silently break restart-replay.
        let err = NsAttachOptions::parse(&opts(&["pool=kvpool", "namespace=other"]))
            .unwrap_err()
            .to_string();
        assert!(err.contains("does not accept 'namespace='"), "{err}");
    }

    #[test]
    fn attach_rejects_unknown_option() {
        let err = NsAttachOptions::parse(&opts(&["pool=kvpool", "nsid=7"]))
            .unwrap_err()
            .to_string();
        assert!(err.contains("unknown ns attach option 'nsid'"), "{err}");
    }

    #[test]
    fn attach_rejects_empty_value() {
        let err = NsAttachOptions::parse(&opts(&["pool="]))
            .unwrap_err()
            .to_string();
        assert!(err.contains("empty value"), "{err}");
    }
}
