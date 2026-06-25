// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM Corporation. All rights reserved.
//! rados-nkv: ergonomic Rust CLI for the NVMe-KV (rados-nkvx) datapath.
//!
//! Foundation (bead spdk-jhk.7.1): FFI to the proven raw vfio-user driver via the
//! C shim, plus a HIDDEN `selftest` subcommand.
//!
//! This bead (spdk-jhk.7.2) adds the ergonomic surface: `~/.rados-nkv.conf`
//! parsing (config), `ns/key` path parsing (path), a safe RAII datapath session
//! (datapath), and the `store` / `get` commands. ns->nsid is resolved from the
//! config's `[namespaces]` table; an unknown ns is a hard error.

mod commands;
mod config;
mod daemon;
mod datapath;
mod ffi;
#[cfg(not(feature = "gpu-native"))]
mod gpu;
#[cfg(feature = "gpu-native")]
mod gpu_native;
mod path;
mod policy;
mod proto;
mod rpc;

// `--gpu` routing: the native in-process HIP path when built with
// `--features gpu-native` (bead spdk-jhk.7.15), else the nkv_vfu_gpu subprocess
// delegation (bead spdk-jhk.7.7). Both expose the same store/retrieve/exec
// surface, so the call sites below are identical regardless of feature.
#[cfg(feature = "gpu-native")]
use gpu_native as gpu_route;
#[cfg(not(feature = "gpu-native"))]
use gpu as gpu_route;

use std::fs;
use std::io::{self, IsTerminal, Read, Write};
use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand};

use commands::options::{GetOptions, StoreOptions};
use config::Config;
use datapath::Session;
use path::KvPath;
use proto::{Request, Response};

#[derive(Parser)]
#[command(name = "rkv", about = "Ergonomic CLI for the NVMe-KV datapath")]
struct Cli {
    /// Route store/get/exec through the GPU-initiated datapath (nkv_vfu_gpu
    /// subprocess) instead of the in-process CPU datapath. `get` issues N
    /// Retrieves across one wavefront + ONE doorbell per chunk (all keys must
    /// share one nsid). store/exec support the subset nkv_vfu_gpu offers (small
    /// UTF-8 values, no exec input).
    #[arg(long, global = true)]
    gpu: bool,

    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Store a value under ns/key (value from -i FILE, else piped stdin).
    Store {
        /// Path 'ns/key' (ns resolved to nsid via ~/.rados-nkv.conf).
        path: String,
        /// Read the value from FILE instead of stdin.
        #[arg(short, long, value_name = "FILE")]
        input: Option<String>,
        /// Object option KEY[=VAL] (ephemeral, ttl=DURATION, touch, prefetch);
        /// validated but a no-op in v1 (no server semantics). Repeatable.
        #[arg(short = 'o', long = "option", value_name = "KEY[=VAL]")]
        options: Vec<String>,
    },

    /// Retrieve one or more ns/key values; raw bytes to stdout.
    #[command(visible_aliases = ["receive", "recieve"])]
    Get {
        /// One or more 'ns/key' paths.
        #[arg(required = true)]
        paths: Vec<String>,
        /// Object option KEY[=VAL] (prefetch); validated but a no-op in v1.
        /// Repeatable.
        #[arg(short = 'o', long = "option", value_name = "KEY[=VAL]")]
        options: Vec<String>,
    },

    /// Probe whether ns/key is present; prints present+length (exit 0) or
    /// absent (non-zero exit). Carries no value body.
    Exist {
        /// Path 'ns/key' (ns resolved to nsid via ~/.rados-nkv.conf).
        path: String,
    },

    /// List configured namespaces, or report that per-ns key listing is
    /// unsupported by the target.
    #[command(visible_alias = "ls")]
    List {
        /// Namespace name; omit to list configured namespaces from the config.
        ns: Option<String>,
    },

    /// Run a named KV Exec op against ns/key (op-name resolved via [exec]).
    Exec {
        /// Exec op name (resolved to op_id via ~/.rados-nkv.conf [exec]).
        name: String,
        /// Path 'ns/key' (ns resolved to nsid via ~/.rados-nkv.conf).
        path: String,
        /// Read the exec input payload from FILE (optional; default empty).
        #[arg(short, long, value_name = "FILE")]
        input: Option<String>,
    },

    /// Namespace management (control-plane, over JSON-RPC).
    Ns {
        #[command(subcommand)]
        command: NsCommand,
    },

    /// Persistent session daemon (opt-in; amortizes EAL/attach across processes).
    Daemon {
        #[command(subcommand)]
        command: DaemonCommand,
    },

    /// Hidden: the detached daemon worker (started by `daemon start`).
    #[command(hide = true, name = "daemon-run")]
    DaemonRun {
        /// Unix socket to bind (overrides the configured daemon_sock).
        #[arg(long)]
        sock: Option<PathBuf>,
    },

    /// Datapath smoke test: open, store (nsid=1), retrieve, byte-compare.
    #[command(hide = true)]
    Selftest {
        /// vfio-user listener dir containing 'cntrl' (e.g. /tmp/nkvx/muser/0)
        traddr: String,
        /// key to store/retrieve under
        key: String,
        /// value to round-trip
        value: String,
    },
}

#[derive(Subcommand)]
enum NsCommand {
    /// Create (or idempotently resolve) a rados-backed Key-Value namespace by
    /// name on the target, durably recording it in the pool's omap registry.
    Create {
        /// Friendly namespace name: the registry key and (by default) the rados
        /// namespace that isolates its keys within the pool.
        name: String,
        /// Request a specific nsid for a freshly created namespace (otherwise the
        /// target assigns one). Ignored on an idempotent hit.
        #[arg(long)]
        nsid: Option<u32>,
        /// Create option KEY[=VAL]; `pool=POOL` is required, `cluster=NAME` and
        /// `namespace=NS` are optional overrides. Repeatable.
        #[arg(short = 'o', long = "option", value_name = "KEY=VAL")]
        options: Vec<String>,
    },

    /// Attach (bind) an EXISTING namespace by name from the pool's durable
    /// registry, reproducing its recorded nsid + data namespace (restart-replay).
    Attach {
        /// Friendly namespace name; must already exist in the registry (defined
        /// by an earlier `ns create`).
        name: String,
        /// Attach option KEY[=VAL]; `pool=POOL` is required, `cluster=NAME` is an
        /// optional override. The nsid and rados namespace are replayed from the
        /// registry, so they are NOT settable here. Repeatable.
        #[arg(short = 'o', long = "option", value_name = "KEY=VAL")]
        options: Vec<String>,
    },

    /// Set a namespace's KV-Exec allowlist from a YAML policy file.
    Allowlist {
        /// Namespace name (resolved to nsid via ~/.rados-nkv.conf).
        name: String,
        /// Policy YAML file describing the allowlist entries.
        #[arg(short, long, value_name = "FILE")]
        input: PathBuf,
    },
}

#[derive(Subcommand)]
enum DaemonCommand {
    /// Start the persistent session daemon (does the vfio-user attach once).
    Start,
    /// Stop the daemon, releasing its vfio-user session cleanly.
    Stop,
    /// Report whether a daemon is answering on the configured socket.
    Status,
}

/// Resolve a namespace name to its nsid, with a helpful error on miss.
///
/// Resolution order (spdk-jhk.7.12):
///   1. The server-side name registry: ask the target (`nvmf_get_subsystems`)
///      for the configured nqn's KV namespace `kv_name`->nsid map. This makes
///      the target the source of truth so a name registered with
///      `rados-nkv ns create <name>` resolves even with no local `[namespaces]`
///      entry.
///   2. The client-side `[namespaces]` cache in `~/.rados-nkv.conf` — used as a
///      fallback for offline/compat (no rpc_sock reachable, or a target/build
///      that predates server-side names).
///
/// The server lookup is best-effort: any RPC failure (target down, old build,
/// no nqn configured) silently falls through to the config cache, so nsid-only
/// and existing client-side-map flows keep working unchanged.
fn resolve_ns(cfg: &Config, ns: &str) -> Result<u32> {
    if let Some(nsid) = resolve_ns_via_server(cfg, ns) {
        return Ok(nsid);
    }
    cfg.resolve_ns(ns).with_context(|| {
        format!(
            "unknown namespace '{ns}': add it to ~/.rados-nkv.conf [namespaces], \
             run 'rados-nkv ns create {ns}', or ensure the target is reachable \
             so the server-side name can be resolved"
        )
    })
}

/// Best-effort server-side name resolution: returns `Some(nsid)` only when the
/// target is reachable, a nqn is configured, and that subsystem reports a KV
/// namespace whose `kv_name` matches `ns`. Any failure returns `None` so the
/// caller falls back to the client-side `[namespaces]` cache.
fn resolve_ns_via_server(cfg: &Config, ns: &str) -> Option<u32> {
    let nqn = cfg.nqn.as_deref()?;
    let client = rpc::RpcClient::new(cfg.rpc_sock());
    client.kv_name_map(nqn).ok()?.get(ns).copied()
}

/// Read the store value: from `-i FILE`, else piped stdin, else error.
fn read_store_value(input: Option<&str>) -> Result<Vec<u8>> {
    match input {
        Some(file) => fs::read(file).with_context(|| format!("reading input file '{file}'")),
        None => {
            let stdin = io::stdin();
            if stdin.is_terminal() {
                bail!(
                    "no value: pass -i FILE or pipe data on stdin \
                     (e.g. `printf data | rados-nkv store ns/key`)"
                );
            }
            let mut buf = Vec::new();
            stdin
                .lock()
                .read_to_end(&mut buf)
                .context("reading value from stdin")?;
            Ok(buf)
        }
    }
}

fn cmd_store(cfg: &Config, path: &str, input: Option<&str>, options: &[String], use_gpu: bool) -> Result<()> {
    let (ns, key) = KvPath::parse_with_key(path)?;
    let nsid = resolve_ns(cfg, &ns)?;
    // Parse/validate -o options before touching the device so a bad option
    // (unknown key, malformed ttl) fails fast and never opens a session.
    let opts = StoreOptions::parse(options)?;
    let val = read_store_value(input)?;

    let (store_opt, ttl_secs) = opts.wire();

    if use_gpu {
        // The GPU subprocess/native path does not carry Store Option bits; the
        // honoured options need the CDW11/CDW12 the CPU datapath sets. Reject
        // rather than silently dropping the requested semantics.
        if opts.has_honored() {
            bail!(
                "--gpu store does not support store option(s) [{}] \
                 (the GPU path cannot set the KV Store Option/TTL fields); \
                 drop --gpu to use ttl/ephemeral/touch",
                opts.honored_summary()
            );
        }
        gpu_route::store(cfg, nsid, &key, &val)?;
    } else if opts.has_honored() {
        // Honoured options ride the KV Store CDW11/CDW12; the daemon wire format
        // does not carry them, so bypass the daemon fast-path and store directly.
        let sess = Session::open(cfg.traddr())?;
        sess.store_opts(nsid, &key, &val, store_opt, ttl_secs)?;
    } else {
        // Fast path: forward to a live session daemon if one is running; else
        // fall back to the existing in-process datapath (opt-in, unchanged).
        let req = Request::Store {
            nsid,
            key: key.clone(),
            val: val.clone(),
        };
        match daemon::try_forward(cfg, &req)? {
            Some(Response::Ok(_)) => {}
            Some(Response::Err(msg)) => bail!("daemon: {msg}"),
            None => {
                let sess = Session::open(cfg.traddr())?;
                sess.store(nsid, &key, &val)?;
            }
        }
    }
    eprintln!(
        "stored {ns}/{key} (nsid={nsid}, {} bytes{})",
        val.len(),
        if use_gpu { ", gpu" } else { "" }
    );
    // ttl/ephemeral/touch are honoured server-side (spdk-jhk.7.14); report them
    // as applied. prefetch has no in-memory meaning, so it stays a documented
    // hint/no-op -- never silently accepted.
    if opts.has_honored() {
        eprintln!(
            "note: applied store option(s) [{}] (server-side)",
            opts.honored_summary()
        );
    }
    if opts.prefetch {
        eprintln!("note: store option [prefetch] is a documented no-op (read hint, no in-memory effect)");
    }
    Ok(())
}

fn cmd_get(cfg: &Config, paths: &[String], options: &[String], use_gpu: bool) -> Result<()> {
    // Parse/validate -o options first so a bad option fails before any I/O.
    let opts = GetOptions::parse(options)?;
    // Resolve everything first so a bad path errors before opening a session.
    let mut resolved: Vec<(u32, String, String)> = Vec::with_capacity(paths.len());
    for p in paths {
        let (ns, key) = KvPath::parse_with_key(p)?;
        let nsid = resolve_ns(cfg, &ns)?;
        resolved.push((nsid, ns, key));
    }

    if opts.prefetch {
        eprintln!("note: get option [prefetch] is a documented no-op (read hint, no in-memory effect)");
    }

    if use_gpu {
        // --gpu get drives N Retrieves through one wavefront + ONE doorbell per
        // chunk (bead spdk-jhk.11). One wavefront == one controller IO queue ==
        // one nsid, so all keys must resolve to the SAME nsid; a request spanning
        // namespaces errors clearly (per-nsid splitting is future work).
        let nsid0 = resolved[0].0;
        if let Some((nsid, ns, key)) = resolved.iter().find(|(n, _, _)| *n != nsid0) {
            let (_, ns0, key0) = &resolved[0];
            bail!(
                "--gpu get cannot span multiple namespaces in one wavefront: \
                 '{ns0}/{key0}' is nsid {nsid0} but '{ns}/{key}' is nsid {nsid}. \
                 One wavefront drives one controller IO queue (one nsid); split \
                 the request per namespace, or drop --gpu for a multi-nsid get"
            );
        }
        let keys: Vec<String> = resolved.iter().map(|(_, _, k)| k.clone()).collect();
        let vals = gpu_route::retrieve_batch(cfg, nsid0, &keys)?;

        let stdout = io::stdout();
        let mut out = stdout.lock();
        let tty = out.is_terminal();
        let multi = resolved.len() > 1;
        for (i, ((_nsid, ns, key), val)) in resolved.iter().zip(vals.iter()).enumerate() {
            let bytes = val.as_deref().ok_or_else(|| {
                anyhow::anyhow!("--gpu get: key '{ns}/{key}' not found")
            })?;
            if tty && multi {
                if i > 0 {
                    writeln!(out)?;
                }
                writeln!(out, "==> {ns}/{key} ({} bytes) <==", bytes.len())?;
            }
            out.write_all(bytes).context("writing value to stdout")?;
            if tty && multi {
                writeln!(out)?;
            }
        }
        out.flush().context("flushing stdout")?;
        return Ok(());
    }

    // Fast path: if a daemon is live, forward each get over it and skip opening a
    // local session entirely. Otherwise open one in-process session for all keys
    // (existing behavior). We probe with the first key; a None means no daemon.
    let mut daemon_live = false;
    let mut first_via_daemon: Option<Vec<u8>> = None;
    if !resolved.is_empty() {
        let (nsid, _ns, key) = &resolved[0];
        let req = Request::Get {
            nsid: *nsid,
            key: key.clone(),
        };
        match daemon::try_forward(cfg, &req)? {
            Some(Response::Ok(bytes)) => {
                daemon_live = true;
                first_via_daemon = Some(bytes);
            }
            Some(Response::Err(msg)) => bail!("daemon: {msg}"),
            None => {}
        }
    }

    let sess = if daemon_live {
        None
    } else {
        Some(Session::open(cfg.traddr())?)
    };
    let stdout = io::stdout();
    let mut out = stdout.lock();
    let tty = out.is_terminal();
    let multi = resolved.len() > 1;

    for (i, (nsid, ns, key)) in resolved.iter().enumerate() {
        let val = if daemon_live {
            if i == 0 {
                first_via_daemon.take().expect("probed first key")
            } else {
                let req = Request::Get {
                    nsid: *nsid,
                    key: key.clone(),
                };
                match daemon::try_forward(cfg, &req)? {
                    Some(Response::Ok(bytes)) => bytes,
                    Some(Response::Err(msg)) => bail!("daemon: {msg}"),
                    // Daemon vanished mid-batch: fall back for this key directly.
                    None => Session::open(cfg.traddr())?.retrieve(*nsid, key)?,
                }
            }
        } else {
            sess.as_ref().expect("direct session").retrieve(*nsid, key)?
        };
        if tty && multi {
            // Human at a terminal asking for several keys: label each clearly.
            if i > 0 {
                writeln!(out)?;
            }
            writeln!(out, "==> {ns}/{key} ({} bytes) <==", val.len())?;
        }
        out.write_all(&val).context("writing value to stdout")?;
        if tty && multi {
            writeln!(out)?;
        }
    }
    out.flush().context("flushing stdout")?;
    Ok(())
}

/// KV Exist probe for `ns/key`. Prints a one-line human result and returns
/// `true` when the key is present (length surfaced from the completion's cdw0),
/// `false` when absent. The caller maps `false` to a non-zero exit. Exist carries
/// no value body, so it is unaffected by the controller->host DMA path and does
/// not route through the GPU datapath (a presence probe has no value to scatter).
fn cmd_exist(cfg: &Config, path: &str) -> Result<bool> {
    let (ns, key) = KvPath::parse_with_key(path)?;
    let nsid = resolve_ns(cfg, &ns)?;
    let sess = Session::open(cfg.traddr())?;
    match sess.exist(nsid, &key)? {
        Some(len) => {
            println!("present {ns}/{key} (nsid={nsid}, {len} bytes)");
            Ok(true)
        }
        None => {
            println!("absent {ns}/{key} (nsid={nsid})");
            Ok(false)
        }
    }
}

/// Resolve an exec op-name to its op_id, with a helpful error listing the known
/// names on miss. Never hardcodes an op_id — the mapping is config-driven.
fn resolve_exec(cfg: &Config, name: &str) -> Result<u32> {
    cfg.resolve_exec(name).with_context(|| {
        let mut known: Vec<&str> = cfg.exec.keys().map(String::as_str).collect();
        known.sort_unstable();
        let known = if known.is_empty() {
            "(none configured)".to_string()
        } else {
            known.join(", ")
        };
        format!(
            "unknown exec op '{name}': add it to ~/.rados-nkv.conf [exec] \
             (known ops: {known})"
        )
    })
}

fn cmd_exec(cfg: &Config, name: &str, path: &str, input: Option<&str>, use_gpu: bool) -> Result<()> {
    let (ns, key) = KvPath::parse_with_key(path)?;
    let nsid = resolve_ns(cfg, &ns)?;
    let op_id = resolve_exec(cfg, name)?;

    if use_gpu {
        // Both routes return display-ready bytes for bytecount/identity; relay.
        let raw = gpu_route::exec(cfg, nsid, &key, op_id, input.is_some())?;
        let stdout = io::stdout();
        let mut out = stdout.lock();
        out.write_all(&raw).context("writing exec result to stdout")?;
        out.flush().context("flushing stdout")?;
        return Ok(());
    }

    // Exec input is optional (most built-ins ignore it); default to empty.
    let payload: Vec<u8> = match input {
        Some(file) => fs::read(file).with_context(|| format!("reading input file '{file}'"))?,
        None => Vec::new(),
    };

    // Fast path: forward to a live daemon; else fall back to a local session.
    let result = {
        let req = Request::Exec {
            nsid,
            op_id,
            key: key.clone(),
            input: payload.clone(),
        };
        match daemon::try_forward(cfg, &req)? {
            Some(Response::Ok(bytes)) => bytes,
            Some(Response::Err(msg)) => bail!("daemon: {msg}"),
            None => {
                let sess = Session::open(cfg.traddr())?;
                sess.exec(nsid, &key, op_id, &payload)?
            }
        }
    };

    // bytecount returns the object length as a little-endian u64; print it as an
    // integer. All other ops (identity, ...) echo raw result bytes to stdout.
    if name == "bytecount" {
        if result.len() != 8 {
            bail!(
                "exec '{name}' (op_id={op_id}) returned {} bytes, expected 8 (LE u64)",
                result.len()
            );
        }
        let mut le = [0u8; 8];
        le.copy_from_slice(&result);
        println!("{}", u64::from_le_bytes(le));
    } else {
        let stdout = io::stdout();
        let mut out = stdout.lock();
        out.write_all(&result).context("writing exec result to stdout")?;
        out.flush().context("flushing stdout")?;
    }
    Ok(())
}

fn selftest(traddr: &str, key: &str, value: &str) -> Result<bool> {
    let sess = Session::open(traddr)?;
    let val = value.as_bytes();
    sess.store(1, key, val)?;
    let got = sess.retrieve(1, key)?;
    Ok(got == val)
}

fn run() -> Result<ExitCode> {
    let cli = Cli::parse();
    let use_gpu = cli.gpu;
    // `exist` is the one command whose exit code encodes a query result (present
    // vs absent), not just success vs error, so it returns its own ExitCode. Every
    // other command maps Ok -> SUCCESS; a returned Err becomes FAILURE in main.
    if let Command::Exist { path } = &cli.command {
        let cfg = Config::load()?;
        return Ok(match cmd_exist(&cfg, path)? {
            true => ExitCode::SUCCESS,
            false => ExitCode::FAILURE,
        });
    }
    let res: Result<()> = match cli.command {
        Command::Store { path, input, options } => {
            let cfg = Config::load()?;
            cmd_store(&cfg, &path, input.as_deref(), &options, use_gpu)
        }
        Command::Get { paths, options } => {
            let cfg = Config::load()?;
            cmd_get(&cfg, &paths, &options, use_gpu)
        }
        Command::Exist { .. } => unreachable!("handled above"),
        Command::List { ns } => {
            let cfg = Config::load()?;
            commands::list::run(&cfg, ns.as_deref())
        }
        Command::Exec { name, path, input } => {
            let cfg = Config::load()?;
            cmd_exec(&cfg, &name, &path, input.as_deref(), use_gpu)
        }
        Command::Ns { command } => {
            let mut cfg = Config::load()?;
            match command {
                NsCommand::Create { name, nsid, options } => {
                    commands::ns::create(&mut cfg, &name, nsid, &options)
                }
                NsCommand::Attach { name, options } => {
                    commands::ns::attach(&mut cfg, &name, &options)
                }
                NsCommand::Allowlist { name, input } => {
                    commands::ns::allowlist(&cfg, &name, &input)
                }
            }
        }
        Command::Daemon { command } => {
            let cfg = Config::load()?;
            match command {
                DaemonCommand::Start => daemon::start(&cfg),
                DaemonCommand::Stop => daemon::stop(&cfg),
                DaemonCommand::Status => daemon::status(&cfg),
            }
        }
        Command::DaemonRun { sock } => {
            let cfg = Config::load()?;
            daemon::run(&cfg, sock.as_deref())
        }
        Command::Selftest { traddr, key, value } => match selftest(&traddr, &key, &value)? {
            true => {
                println!("OK");
                Ok(())
            }
            false => bail!("selftest mismatch (retrieved bytes != stored)"),
        },
    };
    res.map(|()| ExitCode::SUCCESS)
}

fn main() -> ExitCode {
    match run() {
        Ok(code) => code,
        Err(e) => {
            eprintln!("error: {e:#}");
            ExitCode::FAILURE
        }
    }
}
