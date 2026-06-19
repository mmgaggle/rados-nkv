//! Minimal JSON-RPC 2.0 client over the target's unix socket (bead spdk-jhk.7.4).
//!
//! Control-plane ops (`ns create`, `ns allowlist`) talk JSON-RPC to the SPDK
//! target at `[target] rpc_sock` (default `/tmp/nkvx/rpc.sock`). We speak the
//! same framing SPDK's own `scripts/rpc.py` uses: a single JSON request object
//! is written to the socket, and the response is read back and decoded as one
//! JSON value. SPDK does not require newline termination, but it tolerates it
//! and `rpc.py` appends none; we write the compact object followed by `\n` and
//! then read until the socket yields a complete JSON value (raw streaming
//! decode), so we neither depend on a trailing newline nor on the response
//! arriving in a single `read()`.
//!
//! This is deliberately tiny: no batching, no notifications, one request per
//! call, blocking I/O. That matches the CLI's one-shot invocation model.

use std::collections::BTreeMap;
use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::time::Duration;

use anyhow::{anyhow, bail, Context, Result};
use serde::de::DeserializeOwned;
use serde::Serialize;
use serde_json::{json, Value};

/// A JSON-RPC 2.0 client bound to one unix-socket endpoint.
pub struct RpcClient {
    sock_path: String,
}

/// Wire shape of a JSON-RPC 2.0 request.
#[derive(Serialize)]
struct Request<'a, P: Serialize> {
    jsonrpc: &'static str,
    id: u64,
    method: &'a str,
    params: P,
}

impl RpcClient {
    /// Bind to the unix socket at `path` (connection happens per call).
    pub fn new(sock_path: impl Into<String>) -> Self {
        Self {
            sock_path: sock_path.into(),
        }
    }

    /// Call `method` with `params`, returning the decoded `result`.
    ///
    /// Errors carry the JSON-RPC `error.message` verbatim when the target
    /// rejects the request, so callers surface the server's own diagnostics.
    pub fn call<P, R>(&self, method: &str, params: P) -> Result<R>
    where
        P: Serialize,
        R: DeserializeOwned,
    {
        let req = Request {
            jsonrpc: "2.0",
            id: 1,
            method,
            params,
        };
        let body = serde_json::to_vec(&req).context("serializing JSON-RPC request")?;

        let mut stream = UnixStream::connect(&self.sock_path).with_context(|| {
            format!(
                "connecting to JSON-RPC socket {} (is the target up?)",
                self.sock_path
            )
        })?;
        // A generous read timeout: a paused/resumed subsystem op can take a
        // moment, but we should never hang the CLI forever on a dead target.
        stream
            .set_read_timeout(Some(Duration::from_secs(60)))
            .context("setting rpc socket read timeout")?;

        stream
            .write_all(&body)
            .and_then(|()| stream.write_all(b"\n"))
            .with_context(|| format!("writing JSON-RPC request for '{method}'"))?;

        let value = read_one_json(&mut stream)
            .with_context(|| format!("reading JSON-RPC response for '{method}'"))?;

        // JSON-RPC 2.0: exactly one of result / error is present.
        if let Some(err) = value.get("error") {
            let msg = err
                .get("message")
                .and_then(Value::as_str)
                .unwrap_or("unknown error");
            let code = err.get("code").and_then(Value::as_i64).unwrap_or(0);
            bail!("target rejected '{method}': {msg} (code {code})");
        }
        let result = value
            .get("result")
            .ok_or_else(|| anyhow!("JSON-RPC response for '{method}' has neither result nor error"))?;
        serde_json::from_value(result.clone())
            .with_context(|| format!("decoding result of '{method}'"))
    }

    /// Query the target for the server-side KV namespace name->nsid map of one
    /// subsystem (spdk-jhk.7.12).
    ///
    /// Calls `nvmf_get_subsystems` and, for the subsystem whose `nqn` matches,
    /// collects every namespace that carries a `kv_name` field into a
    /// name->nsid map. Namespaces with no server-side name are simply absent
    /// (the canonical nsid is unaffected). This lets the CLI resolve a friendly
    /// name from the target itself, with the client-side `[namespaces]` cache
    /// only used as a fallback for offline/compat.
    pub fn kv_name_map(&self, nqn: &str) -> Result<BTreeMap<String, u32>> {
        // nvmf_get_subsystems accepts an optional `nqn` filter; pass it so the
        // target returns just the one subsystem we care about.
        let subsystems: Vec<Value> = self
            .call("nvmf_get_subsystems", json!({ "nqn": nqn }))
            .context("nvmf_get_subsystems failed")?;
        Ok(kv_name_map_from_subsystems(&subsystems, nqn))
    }
}

/// Build the `kv_name`->nsid map from a decoded `nvmf_get_subsystems` result,
/// restricted to the subsystem whose `nqn` matches (spdk-jhk.7.12).
///
/// Only KV namespaces that carry a `kv_name` field contribute an entry;
/// namespaces with no server-side name (or non-KV namespaces) are skipped, so
/// the canonical nsid is never inferred from a missing name. Pure and
/// transport-free for unit testing.
fn kv_name_map_from_subsystems(subsystems: &[Value], nqn: &str) -> BTreeMap<String, u32> {
    let mut map = BTreeMap::new();
    for sub in subsystems {
        if sub.get("nqn").and_then(Value::as_str) != Some(nqn) {
            continue;
        }
        let Some(namespaces) = sub.get("namespaces").and_then(Value::as_array) else {
            continue;
        };
        for ns in namespaces {
            let (Some(name), Some(nsid)) = (
                ns.get("kv_name").and_then(Value::as_str),
                ns.get("nsid").and_then(Value::as_u64),
            ) else {
                continue;
            };
            map.insert(name.to_string(), nsid as u32);
        }
    }
    map
}

/// Read bytes from `stream` until they parse as exactly one JSON value.
///
/// SPDK's JSON-RPC server replies with a single object; it may arrive across
/// several reads. We accumulate and attempt a streaming parse after each chunk,
/// stopping at the first complete value (raw-decode framing, no reliance on a
/// trailing newline).
fn read_one_json(stream: &mut UnixStream) -> Result<Value> {
    let mut buf: Vec<u8> = Vec::with_capacity(4096);
    let mut chunk = [0u8; 4096];
    loop {
        // Try to parse what we have so far; a single complete value ends it.
        if !buf.is_empty() {
            let mut de = serde_json::Deserializer::from_slice(&buf).into_iter::<Value>();
            if let Some(item) = de.next() {
                match item {
                    Ok(v) => return Ok(v),
                    Err(e) if e.is_eof() => { /* need more bytes */ }
                    Err(e) => return Err(anyhow!("malformed JSON-RPC response: {e}")),
                }
            }
        }
        let n = stream.read(&mut chunk).context("reading from rpc socket")?;
        if n == 0 {
            if buf.is_empty() {
                bail!("rpc socket closed before any response was received");
            }
            // EOF with a partial buffer: one last parse attempt for a clean error.
            return serde_json::from_slice(&buf)
                .map_err(|e| anyhow!("incomplete JSON-RPC response ({e})"));
        }
        buf.extend_from_slice(&chunk[..n]);
    }
}

#[cfg(test)]
mod tests {
    use super::kv_name_map_from_subsystems;
    use serde_json::json;

    const NQN: &str = "nqn.2026-06.io.spdk:t";

    #[test]
    fn maps_named_kv_namespaces_for_the_matching_nqn() {
        let subsystems = json!([{
            "nqn": NQN,
            "namespaces": [
                { "nsid": 1, "kvdev_name": "KvMem0", "kv_name": "alpha" },
                { "nsid": 2, "kvdev_name": "KvMem1", "kv_name": "beta" },
                // KV namespace with no server-side name: contributes nothing.
                { "nsid": 3, "kvdev_name": "KvMem2" },
                // Non-KV namespace: contributes nothing.
                { "nsid": 4, "bdev_name": "Malloc0" }
            ]
        }]);
        let subsystems = subsystems.as_array().unwrap();

        let map = kv_name_map_from_subsystems(subsystems, NQN);
        assert_eq!(map.get("alpha"), Some(&1));
        assert_eq!(map.get("beta"), Some(&2));
        assert_eq!(map.len(), 2, "unnamed and non-KV namespaces must be skipped");
    }

    #[test]
    fn ignores_other_subsystems() {
        let subsystems = json!([
            {
                "nqn": "nqn.2026-06.io.spdk:other",
                "namespaces": [{ "nsid": 1, "kv_name": "alpha" }]
            },
            {
                "nqn": NQN,
                "namespaces": [{ "nsid": 7, "kv_name": "mine" }]
            }
        ]);
        let subsystems = subsystems.as_array().unwrap();

        let map = kv_name_map_from_subsystems(subsystems, NQN);
        assert_eq!(map.get("mine"), Some(&7));
        assert_eq!(map.get("alpha"), None, "must not leak names from other subsystems");
        assert_eq!(map.len(), 1);
    }

    #[test]
    fn empty_when_no_subsystem_matches() {
        let subsystems = json!([{ "nqn": "nqn.other", "namespaces": [] }]);
        let subsystems = subsystems.as_array().unwrap();
        assert!(kv_name_map_from_subsystems(subsystems, NQN).is_empty());
    }
}
