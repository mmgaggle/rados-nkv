# RADOS-NKV wire format

The on-the-wire contract between a RADOS-NKV host (initiator) and the controller:
the NVMe Key-Value command set as carried over the SPDK vfio-user transport. A
Key-Value namespace is identified by its Command Set Identifier (`CSI = 0x1`,
Key Value); the controller decodes each command, applies the per-namespace gates,
and dispatches to the backing key-value device (`mem` or `rados`).

This document is the authoritative description of the command encoding. It covers
the standard command set, the key and value encodings, the computational-storage
extension, and how keys and namespaces map onto RADOS objects.

## Command set

| Command  | Opcode | Direction        | Purpose                                   |
|----------|--------|------------------|-------------------------------------------|
| Store    | `0x01` | host → device    | Write the value for a key                 |
| Retrieve | `0x02` | device → host    | Read the value for a key                  |
| List     | `0x06` | device → host    | Enumerate keys from a start position       |
| Delete   | `0x10` | host → device    | Remove a key and its value                |
| Exist    | `0x14` | —                | Test whether a key is present             |
| Exec     | `0x83` | host ↔ device    | Run an allowlisted module against a value |

Store, Retrieve, List, Delete, and Exist are the standard commands. Exec is a
vendor command (see [Execution](#execution)).

## Keys

A key is a byte string of **1 to 255 bytes**. Keys are carried two ways depending
on length; the controller selects the path by the key length.

### Keys of 16 bytes or fewer — inline

The key travels inside the command itself, in the standard NVMe-KV inline slots:

| Bytes   | Location         |
|---------|------------------|
| 0–7     | CDW2, CDW3       |
| 8–15    | CDW14, CDW15     |

The key length (1–16) is given in the **Key Length field, CDW11 bits 7:0**. The
bytes are a flat little-endian image; unused high bytes are zero. This is the
standard NVMe-KV encoding, so an initiator that only ever uses keys of 16 bytes
or fewer interoperates with no extension.

### Keys of 17 to 255 bytes — in the payload

A key longer than 16 bytes does not fit the inline slots. It is instead carried
**length-prefixed at the head of the DPTR/SGL payload**:

```
[ u16 key_len ][ key_len key bytes ][ value or remaining payload … ]
```

`key_len` is the true key length (17–255). The inline key slots (CDW2/3/14/15)
are unused for this case. This is the **same key encoding the Exec command uses**,
so a single decode path serves long keys across the whole command set.

For Store, the value follows the key in the same host→device buffer. For
Retrieve, the data buffer is described as an SGL: the leading segment carries the
`[u16 key_len][key]` (host → device) and the value is returned into the following
segment(s) (device → host).

**Why in-payload.** The NVMe-KV standard carries the key *inline in the command*
and therefore caps it at 16 bytes: a command with Key Length > 16 is aborted with
*Invalid Field in Command*. The standard does **not** define how to transport a
longer key — it explicitly leaves that to a future "alternative mechanism." This
length-prefixed payload encoding is RADOS-NKV's choice of that mechanism, and it
is no less standard-conformant than any other vendor's long-key extension (e.g.
Samsung's separate host key buffer) — there is simply no standard above 16 bytes
to conform to. RADOS-NKV unifies on the in-payload form so a single decode path
serves both Exec and long base-op keys; where a key does exceed 16 bytes,
in-payload also avoids the extra key-buffer DMA a separate-pointer scheme would
add. A cache keyed by a ≤16-byte hash stays inline and needs neither — the
long-key path is for wider identifiers (see
[Content-addressed keys and collisions](#content-addressed-keys-and-collisions)).

### Length bounds

| Bound            | Value |
|------------------|-------|
| Minimum key      | 1     |
| Maximum key      | 255   |

The 255-byte maximum is the **NVMe-KV standard's architectural Key Length limit**
(the Key Length field is 8 bits wide), not a RADOS-NKV- or Samsung-specific
number. Any conforming implementation that supports long keys tops out at the same
value.

### Content-addressed keys and collisions

When the key is a content hash, the key width sets the collision resistance, and a
collision aliases two distinct values onto one key — a correctness failure, not a
performance one. The right width depends on the key's role:

- **Routing/index hints can be narrow.** Where a collision only costs a cache or
  routing miss, 64 bits is enough. llm-d's KV-cache indexer keys blocks with an
  **FNV-64a (64-bit)** chained hash over `[parent_hash, token_chunk, extra]`
  (16-token blocks) — it fits the inline slots with room to spare.
- **An authoritative store wants the wider hash.** When a collision would serve the
  *wrong* value, the ecosystem uses 256 bits: vLLM's block "engine key" defaults to
  **SHA-256 (256-bit / 32 bytes)** specifically to address collision risk.
  RADOS-NKV backing the actual KV blocks plays this store role, so its keys are
  32-byte content hashes — which is why a GPU-initiated hot path uses the long-key
  path, not the inline slots.

The birthday math underneath: a 128-bit key collides at ~2^64 entries (a store of
10^12 distinct entries has collision probability ~10^-15), so 16-byte inline keys
are themselves collision-safe for a cache. The reason to go to 32 bytes is
**interop with the 256-bit engine key (e.g. SHA-256), not collision necessity.**
Identifiers that are intrinsically wider than 16 bytes must use the long-key path
regardless — e.g. git object IDs (20-byte SHA-1, 32-byte SHA-256), which *are* the
object's identity and cannot be truncated without breaking addressing-by-OID.

## Values

The value is carried in the data buffer addressed by the command's data pointer
(DPTR — PRP1/PRP2 or SGL1):

- **Store** — the value size is in **CDW10** (`vsize`); the value is read from the
  data buffer (after the in-payload key, if any).
- **Retrieve** — `vsize` (CDW10) is the host buffer size; the device returns up to
  `vsize` bytes and reports the **true stored length in completion DW0**. A value
  larger than the host buffer is truncated, not failed: the host reads DW0,
  resizes, and re-issues.

The device advertises its maximum value length in Identify Namespace. The RADOS
backend stores each value as one RADOS object and supports values up to 64 MiB.

## Per-command field reference

| Field            | Location            | Used by              |
|------------------|---------------------|----------------------|
| Opcode           | CDW0                | all                  |
| Namespace ID     | CDW1                | all                  |
| Inline key 0–7   | CDW2, CDW3          | all (keys ≤ 16 B)    |
| Inline key 8–15  | CDW14, CDW15        | all (keys ≤ 16 B)    |
| Key length       | CDW11 bits 7:0      | all                  |
| Store options    | CDW11 (option bits) | Store                |
| Value size       | CDW10               | Store, Retrieve      |
| TTL              | CDW12               | Store                |
| Data pointer     | PRP1/PRP2 or SGL1   | all transferring data|

List takes the key as a **start position**, where length 0 means "from the
beginning"; it returns a packed table of `(u16 key_len, key bytes)` entries,
padded to a 4-byte stride.

### Store options (CDW11)

| Option                         | Effect                                            |
|--------------------------------|---------------------------------------------------|
| Store-If-Key-Exists            | Fail if the key does not already exist            |
| Store-If-No-Key-Exists         | Fail if the key already exists                    |
| TTL valid                      | Apply the CDW12 TTL (see [Write tiers](#write-tiers)) |
| Ephemeral                      | Keep the value non-durable                        |
| Touch                          | Refresh an existing key's TTL / access time       |

## Completion status

| Status | Code   | Meaning                          |
|--------|--------|----------------------------------|
| Invalid value size | `0x85` | `vsize` out of range  |
| Invalid key size   | `0x86` | key length out of range |
| Key does not exist | `0x87` | reported, not fatal — e.g. Exist / Retrieve on an absent key |

## Write tiers

A Store with **no TTL** is durable: write-through to RADOS with full replication.
A Store **with a TTL** is ephemeral: it lives in the owning host's resident store
and ages out at the TTL — never written to RADOS, no durability or migration. The
TTL is the storage-tier selector.

## Execution

The Exec command (`0x83`) runs an allowlisted module against a stored value where
it rests, and returns the result. The reference runtime is WebAssembly.

### Request / response

Exec uses a **single bidirectional data buffer**: the request travels in on the
way down, and the response overwrites it on the way back (one DMA). The request
payload is laid out exactly like a long key followed by the call arguments:

```
[ u16 key_len ][ key_len key bytes ][ input bytes … ]
```

- **Key length** is 1–255 (the data-object key the module reads).
- **Input length** is in CDW10, validated against the data-buffer transfer length.
- **Output cap** (`osize`) is in CDW12: the device writes at most `osize` bytes
  back and always reports the **true output length in completion DW0**. A result
  larger than the caller's buffer is truncated, not failed.
- **Op-ID** is in CDW13: it selects which module runs, resolved through the
  per-namespace allowlist.

### Modules and the allowlist

A module is a read-only WebAssembly unit identified by the **sha256 of its `.wasm`
binary**. Compiled wasm is stored through the ordinary key-value interface; to
become eligible for execution it must be **allowlisted by digest, per namespace**.
The allowlist binds `(subsystem, namespace, op-ID) → (module location, sha256,
per-invocation caps)`; the **sha256 is the sole authorization and integrity
anchor** — the executor runs the bytes only if they hash to the bound digest.

Exec never mutates the stored value, so it is permitted on read-only namespaces
(still gated by the allowlist). A module that could write is rejected on a
read-only namespace.

## RADOS object mapping

Each key/value pair is one RADOS object:

- **Key → object name (oid).** The key is hex-encoded into the oid. A 255-byte
  key yields a 510-character oid — well within the RADOS object-name limit of
  2048 bytes (`osd_max_object_name_len`).
- **Namespace → RADOS namespace.** The Key-Value namespace maps to a RADOS
  namespace, whose name is limited to 256 bytes (`osd_max_object_namespace_len`).
- **Pool.** The subsystem selects the RADOS pool.

## Compatibility

A standard NVMe-KV initiator that uses only inline keys (≤ 16 bytes) interoperates
unchanged: the inline encoding, the standard opcodes, and the value path are all
unmodified. Keys of 17–255 bytes are a RADOS-NKV capability — such a client is
simply limited to 16-byte keys. The only place it can observe longer keys is a
List of a namespace that contains them, since list entries carry a `u16` length
per key; it cannot create or address keys it did not write.

Because the standard defines **no transport for keys above 16 bytes** (it caps
inline keys at 16 and leaves longer keys to a vendor-defined mechanism), long-key
interoperability is inherently *per-vendor*, not a standard guarantee. Adopting
another vendor's long-key encoding (e.g. Samsung's) would buy interop with that
vendor's hardware specifically — not standard portability, which does not exist
above 16 bytes.
