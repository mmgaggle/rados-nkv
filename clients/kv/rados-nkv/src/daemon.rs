//! Persistent session daemon (bead spdk-jhk.7.11).
//!
//! OPT-IN amortization of the per-invocation EAL/attach cost. A short-lived
//! `rados-nkv store|get|exec` pays the full `spdk_env_init` + vfio-user attach on
//! every process start; a cross-process pipe chain therefore re-attaches once per
//! stage. The daemon does that work ONCE: it opens a single [`Session`] and serves
//! datapath ops over a unix socket using the [`crate::proto`] framing.
//!
//! ## Opt-in contract
//!
//! With NO daemon running, behavior is identical to before: the CLI's fast path
//! ([`try_forward`]) probes the daemon socket, finds nothing live, and the caller
//! falls back to the existing in-process [`Session`]. Starting a daemon changes
//! only *where* the attach happens, never the bytes returned.
//!
//! ## Concurrency
//!
//! The controller exposes one IO queue, so the daemon serializes requests against
//! its single session. It accepts connections on the listener thread and handles
//! each one to completion before the next: simple, correct, and matching the
//! one-op-at-a-time nature of the underlying hardware.
//!
//! ## Process model
//!
//! `daemon start` re-execs the current binary as the hidden `daemon-run`
//! foreground subcommand, detached with stdio pointed at a log file
//! (`<sock>.log`). `daemon-run` does the actual `spdk_env_init` + attach and runs
//! the accept loop, so the single "Attached to vfio-user" line lands in that log
//! and never in any client's output. `daemon stop` sends a `Shutdown` frame (and
//! falls back to SIGTERM via the recorded pid); `daemon status` pings the socket.

use std::fs;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use anyhow::{bail, Context, Result};

use crate::config::Config;
use crate::datapath::Session;
use crate::proto::{Request, Response};

/// Sidecar paths derived from the daemon socket path.
struct Paths {
    sock: PathBuf,
    log: PathBuf,
    pid: PathBuf,
}

impl Paths {
    fn from(cfg: &Config) -> Result<Self> {
        let sock = cfg.daemon_sock()?;
        let log = with_ext(&sock, "log");
        let pid = with_ext(&sock, "pid");
        Ok(Paths { sock, log, pid })
    }
}

fn with_ext(p: &Path, ext: &str) -> PathBuf {
    let mut s = p.as_os_str().to_os_string();
    s.push(".");
    s.push(ext);
    PathBuf::from(s)
}

// ---------------------------------------------------------------------------
// Client side: the fast path used by store/get/exec.
// ---------------------------------------------------------------------------

/// Try to forward `req` to a live daemon. Returns:
/// - `Ok(Some(resp))` if a daemon answered (success OR an op-level `Err`),
/// - `Ok(None)` if no daemon is live (caller must fall back to the direct path),
/// - `Err(..)` only on a genuine protocol/IO error against a daemon that *was*
///   reachable (so a real fault is not silently masked as "no daemon").
///
/// "No daemon live" is intentionally broad: a missing socket file, a refused
/// connection (stale socket), or a failed ping all mean fall back. This keeps the
/// opt-in contract: no daemon => identical behavior to today.
pub fn try_forward(cfg: &Config, req: &Request) -> Result<Option<Response>> {
    let sock = cfg.daemon_sock()?;
    if !sock.exists() {
        return Ok(None);
    }
    let mut stream = match UnixStream::connect(&sock) {
        Ok(s) => s,
        // Stale socket (daemon gone) or not yet accepting: fall back.
        Err(_) => return Ok(None),
    };
    stream
        .set_read_timeout(Some(Duration::from_secs(120)))
        .ok();

    // Liveness handshake first: a stale-but-connectable socket would otherwise
    // swallow our real op. A failed ping => treat as no daemon (fall back).
    if Request::Ping.write_to(&mut stream).is_err() {
        return Ok(None);
    }
    match Response::read_from(&mut stream) {
        Ok(Response::Ok(_)) => {}
        _ => return Ok(None),
    }

    // Daemon is live: from here a failure is a real error, not a fall-back signal.
    req.write_to(&mut stream)
        .context("forwarding request to session daemon")?;
    let resp = Response::read_from(&mut stream)
        .context("reading response from session daemon")?;
    Ok(Some(resp))
}

/// `daemon status`: report whether a daemon is answering on the socket.
pub fn status(cfg: &Config) -> Result<()> {
    let paths = Paths::from(cfg)?;
    match ping(&paths.sock) {
        Ok(true) => {
            let pid = fs::read_to_string(&paths.pid).ok();
            let pid = pid.as_deref().map(str::trim).unwrap_or("?");
            println!(
                "daemon: running (socket {}, pid {}, log {})",
                paths.sock.display(),
                pid,
                paths.log.display()
            );
            Ok(())
        }
        _ => {
            println!("daemon: not running (no live socket at {})", paths.sock.display());
            Ok(())
        }
    }
}

/// `daemon stop`: ask the daemon to release its session and exit. Prefers a clean
/// `Shutdown` frame; falls back to SIGTERM on the recorded pid; then cleans up the
/// socket/pid files if the process is gone.
pub fn stop(cfg: &Config) -> Result<()> {
    let paths = Paths::from(cfg)?;
    let was_live = ping(&paths.sock).unwrap_or(false);

    if was_live {
        if let Ok(mut s) = UnixStream::connect(&paths.sock) {
            s.set_read_timeout(Some(Duration::from_secs(10))).ok();
            // Best effort: send Shutdown and read the ack.
            let _ = Request::Shutdown.write_to(&mut s);
            let _ = Response::read_from(&mut s);
        }
    }

    // Give the daemon a moment to release the session and unlink its socket.
    let pid = read_pid(&paths.pid);
    let deadline = Instant::now() + Duration::from_secs(10);
    while Instant::now() < deadline {
        if !ping(&paths.sock).unwrap_or(false) && !pid_alive(pid) {
            break;
        }
        std::thread::sleep(Duration::from_millis(100));
    }

    // Escalate to SIGTERM if it is still alive.
    if pid_alive(pid) {
        if let Some(pid) = pid {
            // SAFETY: kill(2) with a benign signal; pid validity is best-effort.
            unsafe { libc_kill(pid, 15) };
        }
        let deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < deadline && pid_alive(pid) {
            std::thread::sleep(Duration::from_millis(100));
        }
    }

    // Clean up leftover files.
    let _ = fs::remove_file(&paths.sock);
    let _ = fs::remove_file(&paths.pid);

    if was_live {
        println!("daemon: stopped");
    } else {
        println!("daemon: was not running (cleaned up {})", paths.sock.display());
    }
    Ok(())
}

/// `daemon start`: launch the detached `daemon-run` child, then wait until it is
/// answering pings. Refuses to start a second daemon on a live socket.
pub fn start(cfg: &Config) -> Result<()> {
    let paths = Paths::from(cfg)?;

    if ping(&paths.sock).unwrap_or(false) {
        bail!(
            "daemon already running on {} (use 'rados-nkv daemon stop' first)",
            paths.sock.display()
        );
    }
    // Stale socket from a crashed daemon: remove so bind() can succeed.
    if paths.sock.exists() {
        let _ = fs::remove_file(&paths.sock);
    }

    let exe = std::env::current_exe().context("locating own executable")?;
    let log = fs::OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open(&paths.log)
        .with_context(|| format!("opening daemon log {}", paths.log.display()))?;
    let log_err = log.try_clone().context("duplicating daemon log handle")?;

    // Re-exec as the hidden foreground runner, detached, with the attach chatter
    // landing in the log file (never in a client's stdout).
    let child = Command::new(&exe)
        .arg("daemon-run")
        .arg("--sock")
        .arg(&paths.sock)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::from(log))
        .stderr(std::process::Stdio::from(log_err))
        .spawn()
        .context("spawning detached daemon process")?;

    // Record the child pid for stop()'s SIGTERM fallback.
    fs::write(&paths.pid, format!("{}\n", child.id()))
        .with_context(|| format!("writing pid file {}", paths.pid.display()))?;

    // Wait for the daemon to finish attaching and start answering.
    let deadline = Instant::now() + Duration::from_secs(30);
    while Instant::now() < deadline {
        if ping(&paths.sock).unwrap_or(false) {
            println!(
                "daemon: started (socket {}, pid {}, log {})",
                paths.sock.display(),
                child.id(),
                paths.log.display()
            );
            return Ok(());
        }
        std::thread::sleep(Duration::from_millis(150));
    }
    bail!(
        "daemon did not come up within 30s; see log {} (target down or attach failed)",
        paths.log.display()
    );
}

// ---------------------------------------------------------------------------
// Server side: the detached `daemon-run` process.
// ---------------------------------------------------------------------------

/// The hidden `daemon-run` entrypoint: open ONE session, bind the socket, serve
/// requests serially until a `Shutdown` request or SIGTERM, then release.
pub fn run(cfg: &Config, sock_override: Option<&Path>) -> Result<()> {
    let sock = match sock_override {
        Some(p) => p.to_path_buf(),
        None => cfg.daemon_sock()?,
    };

    // Open the single long-lived session up front. This is the one place the
    // expensive "Attached to vfio-user" attach happens for all future clients.
    let session = Session::open(cfg.traddr())
        .with_context(|| format!("daemon: opening session on {}", cfg.traddr()))?;
    eprintln!("daemon: session open on {}", cfg.traddr());

    // Bind after a successful attach so a probing client never sees a live socket
    // backed by a half-initialized daemon.
    let _ = fs::remove_file(&sock);
    let listener = UnixListener::bind(&sock)
        .with_context(|| format!("daemon: binding {}", sock.display()))?;
    eprintln!("daemon: listening on {}", sock.display());

    // SIGTERM -> graceful shutdown of the accept loop.
    let stopping = Arc::new(AtomicBool::new(false));
    install_sigterm(stopping.clone());

    for conn in incoming(&listener, &stopping) {
        if stopping.load(Ordering::SeqCst) {
            break;
        }
        match conn {
            Some(stream) => {
                if handle_conn(stream, &session) {
                    // A Shutdown request was served: exit the loop.
                    break;
                }
            }
            None => {} // accept timed out; loop to re-check `stopping`.
        }
    }

    // Drop releases the session; remove the socket so the next probe falls back.
    drop(session);
    let _ = fs::remove_file(&sock);
    eprintln!("daemon: stopped, session released");
    Ok(())
}

/// Handle one client connection to completion. Returns true if the client asked
/// the daemon to shut down.
fn handle_conn(mut stream: UnixStream, session: &Session) -> bool {
    stream.set_read_timeout(Some(Duration::from_secs(120))).ok();
    // One connection may carry several requests (ping handshake then the op).
    loop {
        let req = match Request::read_from(&mut stream) {
            Ok(r) => r,
            // Client hung up or framing error: end this connection.
            Err(_) => return false,
        };
        match req {
            Request::Ping => {
                if Response::Ok(Vec::new()).write_to(&mut stream).is_err() {
                    return false;
                }
            }
            Request::Shutdown => {
                let _ = Response::Ok(Vec::new()).write_to(&mut stream);
                return true;
            }
            op => {
                let resp = serve_op(session, op);
                if resp.write_to(&mut stream).is_err() {
                    return false;
                }
            }
        }
    }
}

/// Execute one datapath op against the shared session, mapping the result into a
/// wire [`Response`]. Errors become `Response::Err` (the op failed) rather than
/// tearing down the daemon.
fn serve_op(session: &Session, op: Request) -> Response {
    let r: Result<Vec<u8>> = match op {
        Request::Store { nsid, key, val } => session.store(nsid, &key, &val).map(|()| Vec::new()),
        Request::Get { nsid, key } => session.retrieve(nsid, &key),
        Request::Exec {
            nsid,
            op_id,
            key,
            input,
        } => session.exec(nsid, &key, op_id, &input),
        Request::Ping | Request::Shutdown => unreachable!("handled by caller"),
    };
    match r {
        Ok(bytes) => Response::Ok(bytes),
        Err(e) => Response::Err(format!("{e:#}")),
    }
}

/// Probe the socket with a `Ping`; true iff a daemon answered `Ok`.
fn ping(sock: &Path) -> Result<bool> {
    if !sock.exists() {
        return Ok(false);
    }
    let mut s = match UnixStream::connect(sock) {
        Ok(s) => s,
        Err(_) => return Ok(false),
    };
    s.set_read_timeout(Some(Duration::from_secs(5))).ok();
    s.set_write_timeout(Some(Duration::from_secs(5))).ok();
    if Request::Ping.write_to(&mut s).is_err() {
        return Ok(false);
    }
    Ok(matches!(Response::read_from(&mut s), Ok(Response::Ok(_))))
}

fn read_pid(pid_path: &Path) -> Option<i32> {
    fs::read_to_string(pid_path)
        .ok()
        .and_then(|s| s.trim().parse::<i32>().ok())
}

fn pid_alive(pid: Option<i32>) -> bool {
    match pid {
        // kill(pid, 0) probes existence without sending a signal.
        Some(pid) => (unsafe { libc_kill(pid, 0) }) == 0,
        None => false,
    }
}

// ---------------------------------------------------------------------------
// Minimal libc bindings (no `libc` crate dependency, matching ffi.rs style).
// ---------------------------------------------------------------------------

extern "C" {
    #[link_name = "kill"]
    fn c_kill(pid: i32, sig: i32) -> i32;
    #[link_name = "signal"]
    fn c_signal(signum: i32, handler: usize) -> usize;
}

unsafe fn libc_kill(pid: i32, sig: i32) -> i32 {
    c_kill(pid, sig)
}

const SIGTERM: i32 = 15;
const SIGINT: i32 = 2;

// A process-global flag the signal handler flips. accept() is interrupted by the
// signal (EINTR) so the loop re-checks this promptly.
static SIG_FLAG: AtomicBool = AtomicBool::new(false);

extern "C" fn on_term(_sig: i32) {
    SIG_FLAG.store(true, Ordering::SeqCst);
}

fn install_sigterm(stopping: Arc<AtomicBool>) {
    // Mirror SIG_FLAG into the caller's flag from the accept loop. We install a
    // plain handler (no SA_RESTART) so a blocked accept() returns EINTR on signal.
    unsafe {
        c_signal(SIGTERM, on_term as *const () as usize);
        c_signal(SIGINT, on_term as *const () as usize);
    }
    // Spawn a tiny watcher that propagates the C flag into the Arc the loop reads.
    std::thread::spawn(move || loop {
        if SIG_FLAG.load(Ordering::SeqCst) {
            stopping.store(true, Ordering::SeqCst);
            return;
        }
        std::thread::sleep(Duration::from_millis(100));
    });
}

/// Yield accepted connections, returning `None` on a timeout/EINTR so the caller
/// can re-check the stop flag. Uses a short accept timeout via nonblocking + poll.
fn incoming<'a>(
    listener: &'a UnixListener,
    stopping: &'a Arc<AtomicBool>,
) -> impl Iterator<Item = Option<UnixStream>> + 'a {
    listener.set_nonblocking(true).ok();
    std::iter::from_fn(move || {
        loop {
            if stopping.load(Ordering::SeqCst) || SIG_FLAG.load(Ordering::SeqCst) {
                return Some(None);
            }
            match listener.accept() {
                Ok((stream, _addr)) => {
                    stream.set_nonblocking(false).ok();
                    return Some(Some(stream));
                }
                Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    std::thread::sleep(Duration::from_millis(100));
                }
                Err(_) => return Some(None),
            }
        }
    })
}
