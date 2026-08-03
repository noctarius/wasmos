---
name: wasmos-add-hostcall
description: Add a new WASM host call (wasmos.* / wasi / env import) to WASMOS and regenerate the ABI. Covers the single-source-of-truth IDL (abi/hostcalls.yaml), the generator (scripts/gen_abi_hostcalls.py), the parts that are generated vs. still hand-written (the two kernel wrapper bodies + language client stubs), and how to validate on both runtimes. Use whenever adding, changing, or retiring a wasmos host call.
---

# WASMOS: Add a Host Call

## Overview

A "host call" is a function the kernel exports to WASM guests (module `wasmos`,
plus a couple of `wasi_snapshot_preview1` / `env` imports). Historically each new
host call meant editing 5–8 places in lock-step (the `HC_*` enum, the WARP
`WASMOS_SYMBOLS` table, the WARP ring-3 dispatch, the WARP AOT symbol table, the
wasm3 link table + m3 signature string, and per-language client stubs).

**Those surfaces are now generated from one file, `abi/hostcalls.yaml`.** You edit
the IDL, regenerate, and write only the two kernel wrapper *bodies* (WARP + wasm3)
and the user-space client binding. See
`docs/architecture/34-abi-idl-and-error-model.md` for the design.

Generated from the IDL (do **not** hand-edit — they carry a `GENERATED` banner):

| Generated file (`abi/generated/c/`) | Consumed by |
|---|---|
| `wasmos_hostcall_ids.h` (`HC_*` enum) | `src/kernel/include/warp_ring3.h` |
| `wasmos_symbols_warp.inc` (`WASMOS_SYMBOLS`) | `src/kernel/warp/link.cpp` |
| `wasmos_link_wasm3.inc` (`WASMOS_WASM3_LINKS`) | `src/kernel/wasm3/link.c` |
| `wasmos_ring3_dispatch.inc` (`warp_ring3_dispatch_table`) | `src/kernel/warp/link.cpp` |
| `wasmos_symbols_aot.inc` (`WASMOS_AOT_SYMBOLS`) | `src/tools/warp_aot/warp_aot.cpp` |

Still hand-written (the generator emits references/decls; you supply the logic):
- the WARP wrapper body `warp_<name>(...)` in `src/kernel/warp/link.cpp`;
- the wasm3 wrapper body `wasmos_<name>(...)` in `src/kernel/wasm3/link.c`;
- the user-space client binding (libc/libsys + per-language import).

## Workflow

1. Add the entry to `abi/hostcalls.yaml`.
2. Regenerate and verify.
3. Write the two kernel wrapper bodies (WARP + wasm3).
4. Add the user-space client binding.
5. Build and boot both runtimes.

## Step 1: Add the entry to `abi/hostcalls.yaml`

Append the entry with the **next free id** — the id space is append-only and must
stay dense (`0..N-1`). It is the ring-3 syscall number (`RAX = 0x100 + id`) **and**
the AOT rebind index (position == id in both WARP tables), so you must never
reorder or renumber existing ids or leave a gap. To retire a host call, replace
its entry with `{ id: N, reserved: true }` (keeps the slot; never delete it).

```yaml
  - name: my_call
    id: 117            # HC_COUNT was 117 → next free id
    returns: status    # status (0/named-negative rc) | value (datum, -1 err) | void
    params:
      - { name: endpoint, kind: handle }        # an id (endpoint/buffer/pid/...)
      - { name: buf,      kind: out, len: len }  # linmem the call WRITES; len bounds it
      - { name: len,      kind: scalar }         # a plain value
```

Param `kind`:
- `scalar` — a plain value.
- `handle` — an id (endpoint, buffer_id, pid, …).
- `ptr` — a linear-memory offset used as a mapping window; `len` names its bound.
- `buf` — a `(ptr, len)` the call READS; `len` names the length param.
- `out` — a `(ptr, len)` the call WRITES; `len` names the bound.

Notes:
- `module` defaults to `wasmos`; set `env` or `wasi_snapshot_preview1` for those.
- `runtimes` defaults to `[warp, wasm3]`; set `[warp]` for a WARP-only call.
- Alias of an existing call: `{ name: my_alias, id: 118, alias_of: my_call }`.
- Rare: if the wasm3 wrapper takes a pointer param as a raw `i32` offset
  (resolved by hand) instead of an m3-translated `*`, mark that param
  `wasm3: i32` so the generated m3 signature matches.

## Step 2: Regenerate and verify

```sh
python3 scripts/gen_abi_hostcalls.py            # regenerates all abi/generated/c/*.{h,inc}
python3 scripts/gen_abi_hostcalls.py --check    # confirm generated files match the IDL
python3 scripts/gen_abi_hostcalls.py --verify-source  # cross-checks any still-hand-written table
```

Requires PyYAML (`pip install pyyaml`). The generator validates ids (unique,
dense, ordered) and refuses gaps/duplicates. `--check` is also wired into the
`quality` lint target, so a stale checked-in generated file fails CI. Commit the
regenerated `abi/generated/c/` files alongside the IDL.

## Step 3: Write the two kernel wrapper bodies

The generated tables reference `warp_<name>` and `wasmos_<name>`; if either body
is missing or has the wrong arity, the kernel build fails (that mismatch is the
drift guard).

WARP — `src/kernel/warp/link.cpp`. Signature is `N` `uint32_t` params + a trailing
`void* ctx_`. Resolve any linmem pointer/`buf`/`out` param with
`warp_mem(ctx_, offset, len)` (do the bounds check); return `uint32_t`.

```c
static uint32_t warp_my_call(uint32_t endpoint, uint32_t buf, uint32_t len, void* ctx_) {
    uint8_t* p = static_cast<uint8_t*>(warp_mem(ctx_, buf, len));
    if (!p) return (uint32_t)WASMOS_ERR_INVAL;   // see abi/generated/c/wasmos_status.h
    /* ... */
    return 0;
}
```

wasm3 — `src/kernel/wasm3/link.c`. Use the m3 raw-function convention; m3 translates
`*` pointer args to host pointers for you.

```c
m3ApiRawFunction(wasmos_my_call) {
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, endpoint);
    m3ApiGetArgMem(uint8_t*, buf);
    m3ApiGetArg(uint32_t, len);
    /* ... */
    m3ApiReturn(0);
}
```

Do NOT touch the link tables/dispatch by hand — they are regenerated. The
`ctx`/register placement in the ring-3 dispatch is computed by the generator.

## Step 4: Add the user-space client binding

Declare the import in libc/libsys and the language binding so guests can call it
(this layer is not generated yet). For C, add the `wasmos.*` import declaration in
the libc header; for AssemblyScript use `@external("wasmos", "my_call")`; Rust/Zig
use their `extern`/import forms; Go uses `//go:wasmimport` / `//go:linkname`.
Keep libc and its per-runtime wrappers in sync (repo rule).

## Step 5: Build and boot both runtimes

```sh
cmake -S . -B build && cmake --build build --target run-qemu-test          # wasm3 (default)
cmake -S . -B build -DWASMOS_WASM_RUNTIME_WARP=ON && \
  cmake --build build --target run-qemu-test                               # WARP
cmake -S . -B build   # leave the tree in the default (wasm3) config
```

A boot-to-`halt` on both backends exercises the generated tables and dispatch.
Also run `cmake --build build --target run-kernel-unit-tests`. Remove any debug
markers and commit the IDL + regenerated files + wrapper bodies together.

## Guardrails

- Never hand-edit `abi/generated/c/*` — regenerate from `abi/hostcalls.yaml`.
- Never reorder/renumber/gap the id space; append only, `reserved: true` to retire.
- Never modify `libs/warp` or `libs/wasm3`.
- The error/status vocabulary has its own IDL (`abi/errors.yaml`,
  `scripts/gen_abi_errors.py`); use `wasmos_status_t` / packed domain codes for
  return values rather than bare `-1`.
