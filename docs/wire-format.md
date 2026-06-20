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

### Length bounds

| Bound            | Value |
|------------------|-------|
| Minimum key      | 1     |
| Maximum key      | 255   |

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
