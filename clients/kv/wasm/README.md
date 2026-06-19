# rados-nkvx wasm Exec modules (TB-WIMP / ADR-0013)

Precompiled `.wasm` modules for the real-wasm KV Exec path. A KV Exec whose
binding is `nkvx:wasm:<name>` runs `<name>.wasm` from this directory in the
dlopen-backed wasmtime runtime, OFF the SPDK reactor, against a plain copy of the
cold-filled object bytes in wasm linear memory.

## Module ABI

A module exports a linear memory named `memory` and a function named `<name>`:

```
<name>(i32 obj_off, i32 obj_len) -> i32 result_len
```

The host copies the object bytes into linear memory at `obj_off` (256), the
module writes its result at offset 8 and returns the result length, and the host
reads `result_len` bytes back from offset 8. These offsets are fixed in
`module/kvdev/rados/kvdev_rados_nkvx_wasm.c` (`WASM_OBJ_OFF`, `WASM_RES_OFF`).

## bytecount.wasm

Returns the object length as a little-endian `u64` (8 bytes), mirroring the
`bytecount` C built-in. Source: `bytecount.c`.

Rebuild (requires clang with the wasm32 target):

```sh
clang --target=wasm32 -nostdlib -O2 \
  -Wl,--no-entry -Wl,--export=bytecount \
  -Wl,--initial-memory=131072 -Wl,--export-memory \
  -o bytecount.wasm bytecount.c
```

## checksum.wasm

Folds the object bytes into a little-endian `u64` (position-weighted byte sum,
mixed with the length). Source: `checksum.c`. Unlike `bytecount` (which only
echoes the `obj_len` argument), `checksum` READS the object bytes out of linear
memory, so its result is wrong unless the bytes are actually present there — this
makes it the TB4 (spdk-ii0) zero-copy proof: a correct answer requires the
custom-`MemoryCreator` backing to alias the cached object buffer.

Rebuild (single-page initial memory so small objects fit one page of the
content-addressed backing):

```sh
clang --target=wasm32 -nostdlib -O2 \
  -Wl,--no-entry -Wl,--export=checksum \
  -Wl,--initial-memory=65536 -Wl,--export-memory \
  -o checksum.wasm checksum.c
```

## TB4 cached / zero-copy path (spdk-ii0)

When an Exec carries an object key (the oid), the executor routes the wasm run
through `kvdev_rados_nkvx_wasm_run_cached`: the object is cold-filled ONCE into an
executor-owned, content-addressed buffer; subsequent Execs of the same object are
served locally (no librados refetch), the wasm linear memory is backed zero-copy
by that buffer via the custom `MemoryCreator` (on-demand strategy, ADR-0013), and
the instantiated instance is reused (warm-instance cache keyed by `(module,
object)`). See `module/kvdev/rados/kvdev_rados_nkvx_wasm.c`.

## oob.wasm (sandbox-escape regression, spdk-ii0 B1)

Adversarial module: declares a single 64 KiB page and writes one byte at offset
`65536` — exactly one byte PAST its own linear memory. A sound sandbox MUST trap
this; the dispatch is then contained (FAILED/ABORTED), never a host-memory clobber.
Before the B1 fix the zero-copy host memory had no guard region and used static
bounds-check elision, so this write silently succeeded (escape). The fix forces
dynamic bounds checks (`memory_reservation=0`, `memory_guard_size=0`) so the access
is caught. Exercised by `test_nkvx_tb4_oob_traps` in `kvdev_rados_nkvx_ut`. Source:
`oob.c`.

Rebuild (requires clang with the wasm32 target):

```sh
clang --target=wasm32 -nostdlib -O2 \
  -Wl,--no-entry -Wl,--export=oob \
  -Wl,--initial-memory=65536 -Wl,--export-memory \
  -o oob.wasm oob.c
```

## slackwrite.wasm / pageprobe.wasm (sandbox-semantics regression, spdk-ii0 D1)

A module that declares fewer pages than the (possibly larger) object backing must
not be able to reach the slack between its declared size and the backing. Before
the D1 fix the custom zero-copy linear memory reported the FULL object backing as
its size, so a small module run against a big object could read/write that slack
without trapping (contained to the object buffer, but a violation of normal
linear-memory semantics). The fix caps the REPORTED size to the module's declared,
page-rounded minimum while keeping the full backing as the allocation (so the
alias stays zero-copy and `memory.grow` can still extend up to the backing).

* `slackwrite.wasm` declares one page and writes at offset `70000` — past its own
  page but inside a 2-page backing. After the fix this MUST trap.
* `pageprobe.wasm` declares one page and writes IN-BOUNDS (offset `1000`) against
  the same 2-page backing — it must still succeed and stay zero-copy.

Exercised by `test_nkvx_d1_declared_size_caps_slack`. Sources: `slackwrite.c`,
`pageprobe.c`.

```sh
clang --target=wasm32 -nostdlib -O2 \
  -Wl,--no-entry -Wl,--export=slackwrite \
  -Wl,--initial-memory=65536 -Wl,--export-memory \
  -o slackwrite.wasm slackwrite.c
clang --target=wasm32 -nostdlib -O2 \
  -Wl,--no-entry -Wl,--export=pageprobe \
  -Wl,--initial-memory=65536 -Wl,--export-memory \
  -o pageprobe.wasm pageprobe.c
```

## statefulglobal.wasm (warm-instance state isolation, spdk-yc1)

Deliberately STATEFUL: keeps a mutable counter (declared init 0) and each run
reports the value it observed BEFORE incrementing it. Run twice warm against the
same `(module, object)`, the second run MUST also observe 0 — because the executor
re-instantiates a fresh store+instance per Exec (resetting wasm globals + declared
data segments) while keeping the expensive engine + compiled module warm. Reusing
the same instance would let the second run observe 1 (state leak). Exercised by
`test_nkvx_yc1_warm_state_isolation`. (Declares 2 pages, so on a small object it
uses the private-fallback memory; the zero-copy alias is proven by other modules.)
Source: `statefulglobal.c`.

```sh
clang --target=wasm32 -nostdlib -O2 \
  -Wl,--no-entry -Wl,--export=statefulglobal \
  -Wl,--initial-memory=131072 -Wl,--export-memory \
  -o statefulglobal.wasm statefulglobal.c
```

## growcap.wasm (store-limiter cap proof, spdk-90x)

Grows a FIXED, BOUNDED number of pages (1024 = 64 MiB) then returns SUCCESS.
Unlike `overalloc` (unbounded growth), the bounded ceiling makes `growcap` a clean
fail-before/after probe for the wasmtime store memory LIMITER on the NON-custom-
memory plain `run()` path (where the limiter — not `nkvx_zc_grow` — is the real
bound): under a tight cap (e.g. 1 MiB) a `memory.grow` is refused before the target
and the module traps (contained); with the limiter disabled the bounded grows all
succeed and it returns SUCCESS without OOMing the target. Exercised by
`test_nkvx_store_limiter_bounds_plain_path`. (The warm/cached path is bounded by
`nkvx_zc_grow` instead; the store_limiter is belt-and-suspenders there — see the
mechanism note on `test_nkvx_d3_warm_memory_cap_contained`.) Source: `growcap.c`.

```sh
clang --target=wasm32 -nostdlib -O2 \
  -Wl,--no-entry -Wl,--export=growcap \
  -Wl,--initial-memory=65536 -Wl,--export-memory \
  -o growcap.wasm growcap.c
```

## libwasmtime.so (runtime dependency)

wasmtime is loaded at runtime via `dlopen` (NOT linked into SPDK). The executor
probes, in order: `libwasmtime.so` (via ldconfig / `LD_LIBRARY_PATH`),
`/usr/local/lib/libwasmtime.so`, `/usr/lib/libwasmtime.so`,
`/usr/lib64/libwasmtime.so`.

Install the v36 LTS (>= v32) C-API shared library so the target can find it:

```sh
# from wasmtime-vXX-x86_64-linux-c-api.tar.xz (lib/libwasmtime.so):
sudo cp lib/libwasmtime.so /usr/local/lib/libwasmtime.so
```

If `libwasmtime.so` is absent the wasm Exec path fails soft with a distinct
"runtime unavailable" status (NOT_SUPPORTED) — never a crash — and the C built-in
modules continue to work. A vendored copy also lives (gitignored) at
`module/kvdev/rados/wasmtime/lib/libwasmtime.so` after the artifact is unpacked.

The `kv_nkvx_exec.sh` e2e exports `SPDK_NKVX_WASM_DIR` pointing at this directory
so the target can locate `bytecount.wasm`.
