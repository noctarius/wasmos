# ABI IDL, Code Generation, and Error Model

> **Documentation status: Design proposal.** No IDL, generator, or unified
> status type is implemented yet. This document specifies a single-source-of-truth
> interface description (IDL) that generates the WASM host-call, IPC opcode, and
> error-code surfaces across every runtime and language variant, and the layered
> error model those surfaces carry.

## Motivation

Three recurring costs slow feature work, all rooted in hand-maintained mirrors of
one logical contract:

- **N-site host-call edits.** A new `wasmos.*` host call must be added, in sync,
  to wasm3 (`src/kernel/wasm3/link.c`), the WARP JIT link table
  (`src/kernel/warp/link.cpp`, ~54 `LINK("wasmos", …)` wrappers today), the WARP
  ring-3 numbered dispatch (`warp_ring3_dispatch`, `RAX = 0x100 + id`), the WARP
  AOT symbol table (`src/tools/warp_aot`), and the per-language stubs in
  `src/libc` / `src/libsys` (C, Rust, Go, Zig, AssemblyScript). Five to eight
  edit sites, plus an ABI-version bump kept consistent by hand.
- **ABI/opcode drift.** IPC opcodes and message shapes live in
  `src/drivers/include/wasmos_driver_abi.h` and are mirrored by prose tables in
  the architecture docs; the tables drift out of date because nothing binds them
  to the header.
- **Ad-hoc error handling.** Bare `-1` returns remain common despite the
  named-error rule, and there is no consistent way to carry a lower layer's
  failure (for example a driver error) up through intermediate services to the
  client.

The remedy is one machine-readable contract per surface, with all variant sites
and documentation tables generated from it, plus compile-time assertions that the
variants agree.

## The IDL

A small set of checked-in YAML files under `abi/` (exact path TBD) is the sole
source of truth. Generated outputs carry a `GENERATED — do not edit` banner and a
`quality` sub-check re-runs the generator and fails if the working tree differs,
so the checked-in generated files can never silently diverge from the IDL.

Three sections, one generator family:

### `abi/hostcalls.yaml`

Each entry declares `{ name, id, params, returns }`. Generation is
unconditional — every host call is emitted for all backends and every language
stub, so backend/language parity is enforced by construction rather than opted
into per entry. Generates:

- the wasm3 registration + dispatch block,
- the WARP JIT `LINK("wasmos", …)` table entries,
- the WARP ring-3 numbered dispatch case,
- the WARP AOT symbol-table entry,
- the guest import stubs for all five languages —
  `abi/generated/{c/wasmos_imports.h,rust,go,zig,assemblyscript}/wasmos_imports.*`
  (every `wasmos`-module call, incl. aliases, with its signature + `doc:` as a
  comment),
- the host-call reference table in `architecture/13-runtime-and-packaging.md`.

The C stub keeps ergonomic typed signatures (`const char*`, `uint64_t*`, struct
pointers) via a per-param `c_type:` override — default `int32_t`, since a wasm32
pointer crosses as an i32 offset. `src/libc/include/wasmos/api.h` reduces to its
struct typedefs, `#define`s, the two native-only `mutex_*` decls (a driver_api
vtable entry, not a WASM host call), and a `#include` of the generated header.
Anything still hand-declared there is guarded by `gen_abi_hostcalls.py
--verify-source` (every `WASMOS_WASM_IMPORT("wasmos", …)` must name a real host
call with a matching arity), so it can never silently drift. Per-call
documentation lives in the IDL's `doc:` field — one source, emitted into every
language stub. The wasi/env-module calls are toolchain-provided and declared by
neither side.

#### Parameter model

A name and id are not enough to drive the generator: it must know the full
**parameter set**, because the marshalling — not the call itself — is what differs
per backend and is where hand-written code goes wrong. Every parameter carries a
semantic **kind** as well as a wire type; the kind selects the marshalling wrapper
the generator emits for each backend.

Wire types are the WASM value types (`i32`, `i64`, `f32`, `f64`); a wasm32 pointer
crosses the boundary as an `i32` offset. Parameter kinds:

- `scalar` — a plain value (`i32`/`i64`/`f32`/`f64`, with signedness recorded so
  the language stubs pick `int32_t`/`uint32_t`/etc.). Passed through unmodified.
- `handle` — an opaque id (`endpoint`, `buffer_id`, `pid`); wire `i32`. Language
  stubs may use a distinct newtype; the kernel side validates it as an id.
- `ptr(len = <param>)` — a **linear-memory pointer**: wire `i32` offset, bounded
  by a companion length parameter (or a fixed size). The generator emits the
  per-backend resolve-and-bounds-check — `warp_mem(ctx, off, len)` (higher-half
  alias) for WARP, the per-process memory base for wasm3 — and hands the
  implementation a validated host pointer. This is the single point that
  currently must be re-derived by hand in every host call and is the origin of
  the WARP ring-3 pointer-coherence bug class.
- `buf` — a `(ptr, len)` pair that travels together; shorthand for a `ptr` plus
  its bounding `len`, so the grouping (and therefore the bounds check) is explicit
  in one place.
- `out(T)` / `outbuf` — caller-provided output pointer(s), resolved and
  bounds-checked like `ptr`/`buf` but written by the implementation.

`returns` is normally a `scalar` — by convention a `wasmos_status_t` (see the
error model) or a single value; larger results are returned through `out`/`outbuf`
parameters rather than a struct-by-value across the boundary.

Illustrative entry:

```yaml
- name: xfer_buffer_read
  id: 0x0210
  params:
    - { name: endpoint,  kind: handle }
    - { name: buffer_id, kind: handle }
    - { name: dst,       kind: outbuf, len: len }   # linmem out pointer + bound
    - { name: offset,    kind: scalar, type: u32 }
    - { name: len,       kind: scalar, type: u32 }
  returns: { kind: scalar, type: status }           # bytes read, or negative status
```

### `abi/opcodes.yaml`

Declares the IPC opcodes (positive `ipc_message_t.type` values) grouped by
subsystem, each with an optional `kind` (req/resp/error/notify) and `doc:`.
`scripts/gen_abi_opcodes.py` generates the per-subsystem C enums + subsystem-id
constants + a `wasmos_opcode_name(subsystem_id, type)` diagnostic lookup
(`abi/generated/c/wasmos_opcodes.h`), the per-language constants
(`abi/generated/{rust,go,zig,assemblyscript}/wasmos_opcodes.*`), and the opcode
reference tables (`abi/generated/docs/opcodes.md`). Every consumer — the core
`wasmos_driver_abi.h` and the per-service headers `rtc_ipc.h`/`font_ipc.h`/
`gfx_ipc.h` plus `serial.c` — `#include`s the generated enums; `--verify-source`
guarded symbol/value parity against the hand-written header before the swap and
self-skips now.

Opcodes are **endpoint-scoped**, not a global namespace: distinct services reuse
the same value ranges on their own endpoints (`gfx` and `proc_manager` both at
`0x200`; `font` and `netdrv` both at `0xA00`; `0x223` is both a proc-broker and a
service-registry request). Each service is its own subsystem, so the per-subsystem
enums preserve the reuse and the name lookup is subsystem-scoped (exact, not
best-effort). Negative `*_ERR_*` codes belong to `abi/errors.yaml`, not here;
flags and descriptor structs stay hand-written in the per-service headers.

Landed as Phase 3a (the `wasmos_driver_abi.h` core) + 3b (the per-service
subsystems `rtc`/`font`/`gfx`/`serial` folded in, and per-language constants).
Optional (3c): typed future-returning request/reply stubs that emit the
transfer-buffer borrow/release ownership contract in one place.

### `abi/errors.yaml`

Declares the error vocabulary (see the error model below). Generates the C
`enum`s, the domain registry, per-language constants, a `wasmos_strerror`-style
decoder, and the error tables in the capability/diagnostics docs.

Generated layout: each language's value ABI is emitted to
`abi/generated/<lang>/wasmos_status.{h,rs,go,zig,ts}` — kept out of the `src/`
format/lint scope so per-language formatters cannot fight the byte-exact re-gen
guard, and un-duplicated across the wasm/native split. C carries the full
reference (constants + the fixed `wasmos_error_t` + chain helpers); the other
languages get constants + packed accessors + decode lookups, with the fixed
error object and `wrap`/`is`/`as` chain helpers emitted alongside the runtime
wrappers. The runtime/IPC wrappers that consume these codes live in
`src/libsys/{wasm,native}`, not in libc — errors are a service-runtime concern.

## ABI version

The active ABI version is derived from the IDL (an explicit field or a content
hash), not tracked by hand. The generator emits a `static_assert` of that version
into every variant's translation unit, so a stale mirror fails the build instead
of drifting. This mechanically enforces the libc/libsys parity rule that is
currently a manual convention (`AGENTS.md`).

## Error Model

The model is layered, following the Zircon transport/domain split with a
Mach-style packed, self-describing code so provenance survives propagation.

### Transport status

A single small global `wasmos_status_t` enum is the kernel/IPC-layer result
space: `WASMOS_OK = 0` and negative `WASMOS_ERR_{NOENT, DENIED, FULL, INVAL,
TIMEOUT, NOMEM, UNSUPPORTED, …}`. A service returns this when it, or the
transport itself, fails. It consolidates today's scattered `IPC_ERR_*` /
`PROC_PM_ERR_*` fragments.

### Domain errors and the domain registry

Operation-level failures are namespaced. `abi/errors.yaml` holds a **domain
registry** — each subsystem (`fs`, `net`, `proc`, `driver-class`, …) is assigned a
stable numeric domain id in the YAML and nowhere else — and, per domain, its local
codes. The generator is the single authority for both, so two subsystems cannot
collide on a domain id and every language sees identical `(domain, code)`
constants.

A domain error is carried as a packed status word plus an optional origin:

- `domain` — which subsystem produced it (from the registry),
- `code` — domain-local error number,
- `origin` — optional source pid/context, for diagnostics and the IPC trace.

The packed layout occupies the IPC reply's status field (wire layout specified
with the opcode message shapes).

### Propagation policy

- **Default: pass-through with provenance.** Intermediate services forward a
  lower layer's `(domain, code)` unchanged. The client decodes the domain and
  code; `origin` records which component actually failed.
- **Wrap only at deliberate abstraction seams.** Where a service must hide the
  lower detail, it re-maps to its own domain and stores the lower error as a
  `caused_by` field, so diagnostic precision is not lost. Wrapping is the
  explicit exception, not the silent default.

This keeps subsystems decoupled (an `fs` code never leaks into `net`'s space)
while a driver's failure remains legible all the way to the client.

### Result representation

Transport and domain failure are distinct axes and are returned as distinct
values, not collapsed into one integer (that collapse is the `-1` ambiguity this
model removes — a dead peer and a real `NOENT` must be distinguishable):

The error is a single **fixed-size** object, always present in full — never an
optional or variable-length path (optional wire paths diverge; a fixed block
either carries a frame or carries zero there):

```c
struct wasmos_frame {                 /* 8 bytes                                */
    uint16_t domain;                  /* 0 = empty slot (terminates the chain)  */
    uint16_t code;                    /* domain-local error code                */
    uint32_t origin;                  /* producing pid/context (0 if none)      */
};
struct wasmos_error {                 /* fixed 40 bytes, always carried in full */
    wasmos_status_t transport;        /* 4B  OK / TIMEOUT / DENIED / FULL / ...  */
    uint32_t        flags;            /* 4B  bit0 = chain truncated; rest rsvd   */
    struct wasmos_frame chain[4];     /* 32B head-first; domain==0 ends the chain*/
};
```

The 4-frame chain is reserved unconditionally; unused frames are zero
(`domain == 0`). On the wire a reply reserves the `{flags, chain}` block (36 B);
the caller fills `transport` from the receive primitive.

Gating semantics are strict: `transport != OK` means the operation never ran and
the chain is undefined (do not read it); `transport == OK` means read the chain —
`chain[0].domain == 0` is success, otherwise `chain[0]` is the error head. Where a
result appears:

- **IPC replies / generated RPC stubs** are the home of the full object. It
  decomposes naturally: `transport` is a property of the *receive* (the
  `wasmos_ipc_*` primitive reports whether a reply arrived at all), and the
  `{flags, chain}` block is carried *inside* the reply.
- **Host calls are degenerate.** A host call is a direct trap, so transport is
  effectively always OK (or it traps/kills the process); most host calls just
  return the scalar domain status (see the `xfer_buffer_read` example). The
  two-field struct is an IPC-path construct, not a per-host-call one.
- **Language projection** is generated: C gets the flat struct (or an out-param);
  Rust maps to a domain `Result<T, DomainError>` nested inside a transport
  `Result<_, TransportError>`. The flat struct is the canonical wire/C form and
  each language stub projects it idiomatically.

### Wrapped and caused-by errors

The propagation policy's `caused_by` is a Go-style error chain, but Go's model —
a heap pointer chain built by `fmt.Errorf("…: %w", err)` and walked by
`errors.Unwrap`/`Is`/`As` — cannot cross a process boundary: errors travel as
IPC message data between isolated ring-3 address spaces, and the WASM apps are
no-heap/freestanding. A `caused_by` *pointer* is meaningless in the receiving
process. The chain is therefore represented as bounded, serializable value data,
and code generation supplies the Go ergonomics on top.

- **Chain, not a pointer graph.** A wrapped error is the fixed four-frame array
  of the result object; each frame is the packed `(domain, code, origin)` unit
  and "caused-by" is the next frame. `chain[0]` is the head (most abstracted);
  the first `domain == 0` terminates. On overflow beyond four frames the head and
  the root cause are retained and `flags` bit0 (`truncated`) is set — the root
  cause is the most diagnostic and is never dropped.
- **Always present, fixed cost.** The chain is carried in full on every reply, in
  the reserved 32-byte block; there is no head-only fast path and no opt-in
  diagnostics path (both would diverge). Unused frames are zero.
- **Wrap = append; pass-through = forward.** To abstract a lower error a service
  pushes its own `(domain, code)` frame onto the received chain and forwards; to
  re-raise it forwards the chain unchanged (provenance already lives in each
  frame's `origin`).

Generated helpers (from the domain registry, per language) give the Go
ergonomics without heap or pointers:

```c
wasmos_error_t wasmos_error_wrap  (wasmos_error_t cause, uint16_t domain, uint16_t code);
wasmos_error_t wasmos_error_unwrap(wasmos_error_t err);            /* -> next frame (cause) */
wasmos_error_t wasmos_error_root  (wasmos_error_t err);            /* -> deepest frame      */
bool           wasmos_error_is    (wasmos_error_t err, wasmos_error_code_t sentinel);
bool           wasmos_error_as    (wasmos_error_t err, uint16_t domain, uint16_t* out_code);
int            wasmos_strerror_chain(wasmos_error_t err, char* buf, size_t n);
```

Because domains and codes come from the IDL, `wasmos_error_is`/`_as` match on
generated constants (`WASMOS_ERR_FS_NOENT`, `WASMOS_ERR_DRIVER_EIO`) — type-safe
sentinel/extraction matching — and `wasmos_strerror_chain` renders a readable
chain from the generated `(domain,code) -> name` tables
(`"fs: NOENT <- net: RESET <- driver: EIO@pid7"`), with `origin` feeding the IPC
trace.

Message policy: free-form per-wrap strings are not carried over IPC (expensive in
no-heap/fixed-payload contexts). The `(domain, code)` identity is the message:
every code has a generated name, and its `abi/errors.yaml` `description` field
(defaulting to the symbol name) is generated into the static table. Descriptions
are fixed, interned strings referenced by id — never a runtime-built string on
the wire.

### Enforcement of the no-bare-`-1` rule

A `scripts/quality.sh` lint gate flags `return -1;` in first-party
`src/services` / `src/drivers` code (allow-listing genuine POSIX-ABI
boundaries), turning the named-error convention into a checked rule.

## Rollout Plan

Phased so each step ships independent value:

1. **Error vocabulary + enforcement.** Add `abi/errors.yaml`, generate
   `wasmos_status_t`, the domain registry, per-language constants, and
   `wasmos_strerror`; land the `-1` lint gate. Standalone; no dependency on the
   host-call or opcode work.
2. **Host-call generation.** Add `abi/hostcalls.yaml` and the generator for the
   five-to-eight backend sites plus language stubs; convert existing host calls;
   add the ABI-version `static_assert`.
3. **Opcode/message generation.** Add `abi/opcodes.yaml`, generate the enum,
   the runtime name table, and the doc tables; optionally the typed request/reply
   stubs.
4. **Packed-status adoption.** Move services onto the packed `(domain, code)`
   reply status and the propagation policy, one subsystem at a time.

## Guardrails

- Never modify `libs/warp` or `libs/wasm3`; the generator targets only
  first-party emission sites.
- Ring-3 is the assumed execution model, not a configuration choice: the WARP
  emission targets the ring-3 path (numbered `warp_ring3_dispatch`) only. There
  is no non-ring3 emission path, and none should be added.
- The generator lives under `scripts/` or `src/tools/`; generated files are
  checked in with a `GENERATED` banner and verified by the `quality` target.
- Generation is a build-time/offline step, not a runtime dependency; the kernel
  and services consume only the emitted C/headers.

## Related

- [Runtime and Packaging](13-runtime-and-packaging.md) — host-call surface.
- [Drivers and Services](15-drivers-and-services.md) — IPC opcode allocation.
- [Capability and Policy](10-capability-and-policy.md) — error/status reporting.
- [Process and IPC](09-process-and-ipc.md) — reply status field and the
  futures direction the typed stubs build on.
