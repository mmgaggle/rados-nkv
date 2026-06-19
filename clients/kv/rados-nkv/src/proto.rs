//! Length-prefixed wire framing for the persistent session daemon
//! (bead spdk-jhk.7.11).
//!
//! The daemon (`rados-nkv daemon start`) holds ONE open vfio-user [`Session`] and
//! serves datapath ops (store/get/exec) for short-lived CLI processes over a unix
//! socket, so a cross-process pipe chain pays the EAL/attach cost once instead of
//! per invocation. This module is the transport-agnostic codec for that link: it
//! only encodes/decodes frames, so it is fully unit-testable without a socket or a
//! live target.
//!
//! ## Frame layout (all integers little-endian)
//!
//! Every message is a single length-prefixed frame:
//!
//! ```text
//! u32 body_len           # number of bytes that follow this prefix
//! u8  tag                # message kind (request opcode or response status class)
//! ... body              # tag-specific, body_len-1 bytes
//! ```
//!
//! ### Request bodies
//!
//! ```text
//! Store    : u32 nsid | u32 key_len | key | u32 val_len | val
//! Get      : u32 nsid | u32 key_len | key
//! Exec     : u32 nsid | u32 op_id  | u32 key_len | key | u32 in_len | input
//! Ping     : (empty)
//! Shutdown : (empty)
//! ```
//!
//! ### Response bodies
//!
//! ```text
//! Ok       : ... payload (raw bytes; empty for store/shutdown/ping)
//! Err      : utf-8 error message
//! ```
//!
//! The body length prefix is the sole framing authority, so a reader knows exactly
//! how many bytes to consume before the next frame and never relies on the socket
//! delivering a message in one `read()`.

use std::io::{self, Read, Write};

/// A request from a short-lived CLI to the daemon.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Request {
    /// KV Store `val` under `key` in `nsid`.
    Store { nsid: u32, key: String, val: Vec<u8> },
    /// KV Retrieve `key` from `nsid`.
    Get { nsid: u32, key: String },
    /// KV Exec `op_id` against `key` in `nsid` with optional `input`.
    Exec {
        nsid: u32,
        op_id: u32,
        key: String,
        input: Vec<u8>,
    },
    /// Liveness probe (the CLI uses this to detect a live daemon before forwarding).
    Ping,
    /// Ask the daemon to release its session and exit cleanly.
    Shutdown,
}

/// A response from the daemon to a CLI.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Response {
    /// Success; `payload` carries the op result bytes (empty for store/ping/shutdown).
    Ok(Vec<u8>),
    /// Failure; carries a human-readable error message.
    Err(String),
}

// Request tags.
const TAG_STORE: u8 = 1;
const TAG_GET: u8 = 2;
const TAG_EXEC: u8 = 3;
const TAG_PING: u8 = 4;
const TAG_SHUTDOWN: u8 = 5;

// Response tags.
const TAG_OK: u8 = 1;
const TAG_ERR: u8 = 2;

/// Cap on a single frame body to bound a malformed/length-lying peer's allocation.
/// 64 MiB matches the datapath's single-value ceiling plus key/header slack.
const MAX_FRAME: u32 = 80 * 1024 * 1024;

impl Request {
    /// Encode this request into a length-prefixed frame.
    pub fn encode(&self) -> Vec<u8> {
        let mut body = Vec::new();
        match self {
            Request::Store { nsid, key, val } => {
                body.push(TAG_STORE);
                put_u32(&mut body, *nsid);
                put_bytes(&mut body, key.as_bytes());
                put_bytes(&mut body, val);
            }
            Request::Get { nsid, key } => {
                body.push(TAG_GET);
                put_u32(&mut body, *nsid);
                put_bytes(&mut body, key.as_bytes());
            }
            Request::Exec {
                nsid,
                op_id,
                key,
                input,
            } => {
                body.push(TAG_EXEC);
                put_u32(&mut body, *nsid);
                put_u32(&mut body, *op_id);
                put_bytes(&mut body, key.as_bytes());
                put_bytes(&mut body, input);
            }
            Request::Ping => body.push(TAG_PING),
            Request::Shutdown => body.push(TAG_SHUTDOWN),
        }
        frame(body)
    }

    /// Write this request as a frame to `w`.
    pub fn write_to(&self, w: &mut impl Write) -> io::Result<()> {
        w.write_all(&self.encode())
    }

    /// Read one request frame from `r`.
    pub fn read_from(r: &mut impl Read) -> io::Result<Request> {
        let body = read_frame(r)?;
        Self::decode(&body)
    }

    /// Decode a request from a frame body (the bytes after the length prefix).
    pub fn decode(body: &[u8]) -> io::Result<Request> {
        let mut c = Cursor::new(body);
        let tag = c.u8()?;
        let req = match tag {
            TAG_STORE => Request::Store {
                nsid: c.u32()?,
                key: c.string()?,
                val: c.bytes()?,
            },
            TAG_GET => Request::Get {
                nsid: c.u32()?,
                key: c.string()?,
            },
            TAG_EXEC => Request::Exec {
                nsid: c.u32()?,
                op_id: c.u32()?,
                key: c.string()?,
                input: c.bytes()?,
            },
            TAG_PING => Request::Ping,
            TAG_SHUTDOWN => Request::Shutdown,
            other => return Err(bad(format!("unknown request tag {other}"))),
        };
        c.expect_end()?;
        Ok(req)
    }
}

impl Response {
    /// Encode this response into a length-prefixed frame.
    pub fn encode(&self) -> Vec<u8> {
        let mut body = Vec::new();
        match self {
            Response::Ok(payload) => {
                body.push(TAG_OK);
                body.extend_from_slice(payload);
            }
            Response::Err(msg) => {
                body.push(TAG_ERR);
                body.extend_from_slice(msg.as_bytes());
            }
        }
        frame(body)
    }

    /// Write this response as a frame to `w`.
    pub fn write_to(&self, w: &mut impl Write) -> io::Result<()> {
        w.write_all(&self.encode())
    }

    /// Read one response frame from `r`.
    pub fn read_from(r: &mut impl Read) -> io::Result<Response> {
        let body = read_frame(r)?;
        Self::decode(&body)
    }

    /// Decode a response from a frame body.
    pub fn decode(body: &[u8]) -> io::Result<Response> {
        let (tag, rest) = body
            .split_first()
            .ok_or_else(|| bad("empty response frame".to_string()))?;
        match *tag {
            TAG_OK => Ok(Response::Ok(rest.to_vec())),
            TAG_ERR => {
                let msg = String::from_utf8(rest.to_vec())
                    .map_err(|_| bad("response error message is not utf-8".to_string()))?;
                Ok(Response::Err(msg))
            }
            other => Err(bad(format!("unknown response tag {other}"))),
        }
    }
}

/// Wrap `body` in its u32 little-endian length prefix.
fn frame(body: Vec<u8>) -> Vec<u8> {
    let mut out = Vec::with_capacity(4 + body.len());
    put_u32(&mut out, body.len() as u32);
    out.extend_from_slice(&body);
    out
}

/// Read one length-prefixed frame body from `r`.
fn read_frame(r: &mut impl Read) -> io::Result<Vec<u8>> {
    let mut lenbuf = [0u8; 4];
    r.read_exact(&mut lenbuf)?;
    let len = u32::from_le_bytes(lenbuf);
    if len == 0 {
        return Err(bad("zero-length frame".to_string()));
    }
    if len > MAX_FRAME {
        return Err(bad(format!(
            "frame body {len} exceeds {MAX_FRAME}-byte cap"
        )));
    }
    let mut body = vec![0u8; len as usize];
    r.read_exact(&mut body)?;
    Ok(body)
}

fn put_u32(buf: &mut Vec<u8>, v: u32) {
    buf.extend_from_slice(&v.to_le_bytes());
}

fn put_bytes(buf: &mut Vec<u8>, b: &[u8]) {
    put_u32(buf, b.len() as u32);
    buf.extend_from_slice(b);
}

fn bad(msg: String) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg)
}

/// A bounds-checked reader over a frame body.
struct Cursor<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Cursor<'a> {
    fn new(buf: &'a [u8]) -> Self {
        Cursor { buf, pos: 0 }
    }

    fn take(&mut self, n: usize) -> io::Result<&'a [u8]> {
        let end = self
            .pos
            .checked_add(n)
            .ok_or_else(|| bad("length overflow".to_string()))?;
        if end > self.buf.len() {
            return Err(bad(format!(
                "frame truncated: need {n} bytes at offset {}, have {}",
                self.pos,
                self.buf.len() - self.pos
            )));
        }
        let s = &self.buf[self.pos..end];
        self.pos = end;
        Ok(s)
    }

    fn u8(&mut self) -> io::Result<u8> {
        Ok(self.take(1)?[0])
    }

    fn u32(&mut self) -> io::Result<u32> {
        let s = self.take(4)?;
        Ok(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
    }

    fn bytes(&mut self) -> io::Result<Vec<u8>> {
        let n = self.u32()? as usize;
        Ok(self.take(n)?.to_vec())
    }

    fn string(&mut self) -> io::Result<String> {
        let b = self.bytes()?;
        String::from_utf8(b).map_err(|_| bad("non-utf8 string field".to_string()))
    }

    fn expect_end(&self) -> io::Result<()> {
        if self.pos != self.buf.len() {
            return Err(bad(format!(
                "trailing {} bytes after request body",
                self.buf.len() - self.pos
            )));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor as IoCursor;

    fn roundtrip_req(req: Request) {
        let bytes = req.encode();
        // Body-length prefix must match the trailing bytes.
        let len = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]) as usize;
        assert_eq!(len, bytes.len() - 4, "prefix length must cover the body");
        let mut r = IoCursor::new(bytes);
        let decoded = Request::read_from(&mut r).expect("decode");
        assert_eq!(decoded, req);
    }

    fn roundtrip_resp(resp: Response) {
        let bytes = resp.encode();
        let mut r = IoCursor::new(bytes);
        let decoded = Response::read_from(&mut r).expect("decode");
        assert_eq!(decoded, resp);
    }

    #[test]
    fn store_roundtrip() {
        roundtrip_req(Request::Store {
            nsid: 7,
            key: "myns/k1".to_string(),
            val: b"future of storage".to_vec(),
        });
    }

    #[test]
    fn store_empty_value() {
        roundtrip_req(Request::Store {
            nsid: 1,
            key: "k".to_string(),
            val: Vec::new(),
        });
    }

    #[test]
    fn get_roundtrip() {
        roundtrip_req(Request::Get {
            nsid: 2,
            key: "a/b/c".to_string(),
        });
    }

    #[test]
    fn exec_roundtrip() {
        roundtrip_req(Request::Exec {
            nsid: 3,
            op_id: 10,
            key: "key".to_string(),
            input: b"payload".to_vec(),
        });
    }

    #[test]
    fn exec_empty_input() {
        roundtrip_req(Request::Exec {
            nsid: 1,
            op_id: 11,
            key: "k".to_string(),
            input: Vec::new(),
        });
    }

    #[test]
    fn ping_shutdown_roundtrip() {
        roundtrip_req(Request::Ping);
        roundtrip_req(Request::Shutdown);
    }

    #[test]
    fn ok_roundtrip() {
        roundtrip_resp(Response::Ok(b"\x15\x00\x00\x00\x00\x00\x00\x00".to_vec()));
        roundtrip_resp(Response::Ok(Vec::new()));
    }

    #[test]
    fn err_roundtrip() {
        roundtrip_resp(Response::Err("nkvx_store failed: rc=-5".to_string()));
    }

    #[test]
    fn binary_value_with_nul_and_high_bytes() {
        // Values are opaque bytes; framing must not choke on NULs or non-utf8.
        let val: Vec<u8> = (0u16..=511).map(|b| b as u8).collect();
        roundtrip_req(Request::Store {
            nsid: 1,
            key: "bin".to_string(),
            val: val.clone(),
        });
        roundtrip_resp(Response::Ok(val));
    }

    #[test]
    fn two_frames_back_to_back() {
        // The length prefix must let a reader split a stream of concatenated frames.
        let mut stream = Vec::new();
        stream.extend_from_slice(
            &Request::Get {
                nsid: 1,
                key: "first".to_string(),
            }
            .encode(),
        );
        stream.extend_from_slice(&Request::Ping.encode());
        let mut r = IoCursor::new(stream);
        let a = Request::read_from(&mut r).unwrap();
        let b = Request::read_from(&mut r).unwrap();
        assert_eq!(
            a,
            Request::Get {
                nsid: 1,
                key: "first".to_string()
            }
        );
        assert_eq!(b, Request::Ping);
    }

    #[test]
    fn truncated_body_errors() {
        // A Store claiming a 100-byte value but with fewer bytes must error, not panic.
        let mut body = vec![TAG_STORE];
        put_u32(&mut body, 1); // nsid
        put_bytes(&mut body, b"k"); // key
        put_u32(&mut body, 100); // val_len lies
        body.extend_from_slice(b"short");
        let err = Request::decode(&body).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidData);
    }

    #[test]
    fn trailing_bytes_error() {
        let mut body = Request::Ping.encode();
        // Strip the 4-byte length prefix to get the body, then append junk.
        let mut tampered = body.split_off(4);
        tampered.push(0xff);
        let err = Request::decode(&tampered).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidData);
    }

    #[test]
    fn unknown_tag_errors() {
        assert!(Request::decode(&[0xee]).is_err());
        assert!(Response::decode(&[0xee]).is_err());
    }

    #[test]
    fn zero_length_frame_errors() {
        let bytes = [0u8, 0, 0, 0]; // len prefix == 0
        let mut r = IoCursor::new(bytes);
        assert!(Request::read_from(&mut r).is_err());
    }

    #[test]
    fn oversize_frame_rejected() {
        let mut bytes = Vec::new();
        put_u32(&mut bytes, MAX_FRAME + 1);
        let mut r = IoCursor::new(bytes);
        let err = Request::read_from(&mut r).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidData);
    }
}
