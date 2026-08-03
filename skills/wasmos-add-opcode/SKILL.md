---
name: wasmos-add-opcode
description: Add or change a WASMOS IPC opcode (a positive ipc_message_t.type value) via the single-source IDL abi/opcodes.yaml + scripts/gen_abi_opcodes.py. Covers the per-subsystem model, the REQ/RESP/ERROR convention, endpoint-scoped value collisions, what is generated (C enums in wasmos_driver_abi.h, the opcode->name diagnostic table, the doc tables, and the Rust/Go/Zig/AS constant files) vs hand-written, and how to validate. Use whenever adding/retiring an IPC opcode.
---

# WASMOS: Add an IPC Opcode

## Overview

An "opcode" is a **positive** `ipc_message_t.type` value a driver/service uses to
name a request/reply/notify, owned per subsystem. Historically each new opcode
meant editing the enum in `src/drivers/include/wasmos_driver_abi.h` plus every
language that hardcoded a copy.

**The opcode space is now generated from one file, `abi/opcodes.yaml`.** You edit
the IDL and regenerate; `wasmos_driver_abi.h` `#include`s the generated enums. See
`docs/architecture/34-abi-idl-and-error-model.md`.

Generated from the IDL (do **not** hand-edit — `GENERATED` banner):

| Generated file | Contents |
|---|---|
| `abi/generated/c/wasmos_opcodes.h` | per-subsystem C enums + best-effort `wasmos_opcode_name()` |
| `abi/generated/docs/opcodes.md` | per-subsystem reference tables |
| `abi/generated/rust/wasmos_opcodes.rs` | `pub const <SYM>: i32` |
| `abi/generated/go/wasmos_opcodes.go` | `const <SYM> int32` (`package wasmos`) |
| `abi/generated/zig/wasmos_opcodes.zig` | `pub const <SYM>: i32` |
| `abi/generated/assemblyscript/wasmos_opcodes.ts` | `export const <SYM>: i32` |

`wasmos_driver_abi.h` consumes the C header via a relative `#include`; the
negative `*_ERR_*` codes (→ `abi/errors.yaml`), flags, and descriptor structs
stay hand-written there.

## Workflow

1. Add the entry to the right subsystem in `abi/opcodes.yaml`.
2. Regenerate and verify.
3. Handle the message (send/reply/dispatch) in the driver/service.
4. Build and boot both runtimes.

## Step 1: Add the entry to `abi/opcodes.yaml`

Find the owning `subsystems:` block and add an opcode with a **free value inside
that subsystem's range**. The convention is `REQ = base`, `RESP = base + 0x80`,
`ERROR = base + 0xFF` (several subsystems deviate — match the one you are in):

```yaml
  - name: fs
    id: 5
    range: [0x400, 0x4FF]
    opcodes:
      # … existing …
      - symbol: FS_IPC_FSYNC_REQ      # full C symbol name, verbatim across languages
        value: 0x415                  # next free value in the fs range
        kind: req                     # req | resp | error | notify (diagnostics/doc)
        doc: |                        # optional; emitted into every generated stub
          Flush buffered writes for the open handle in arg0; RESP on success.
```

- A **new subsystem** gets a new `name`, the next free `id` (dense `0..N-1`), a
  `range`, and its opcodes.
- Opcode **values are endpoint-scoped**: the same value may already exist in a
  *different* subsystem (e.g. `0x223`), which is fine — but must be unique
  *within* its own subsystem (the generator enforces this). Avoid colliding
  inside a range you own.
- Retiring an opcode: delete the entry (there is no dense-id constraint on opcode
  values, unlike host-call ids). Leave a comment if the value must stay reserved.

## Step 2: Regenerate and verify

```sh
python3 scripts/gen_abi_opcodes.py                    # regenerate all outputs
python3 scripts/gen_abi_opcodes.py --check            # generated files match the IDL
python3 scripts/gen_abi_opcodes.py --verify-source    # parity vs the live header
```

Requires PyYAML. `--verify-source` proves the IDL reproduces the header's opcode
enums while any stay hand-written and **self-skips once the header is swapped**
(it is). Both `--check` and `--verify-source` are wired into the `quality` lint
target. Commit the regenerated `abi/generated/**` alongside the IDL.

## Step 3: Handle the message

Opcodes are pure constants — there is no generated dispatch. Send with the
libc/libsys IPC wrappers (`wasmos_ipc_send`/`wasmos_ipc_call`, see
`skills/wasmos-add-hostcall` and `wasmos/ipc.h`) and add the `case` in the
service's message loop. Reply with `RESP`/`ERROR`.

## Step 4: Build and boot both runtimes

Opcodes drive all IPC, so a boot smoke exercises them. Use the per-runtime build
dirs (see `skills/wasmos-build-and-run`):

```sh
cmake -S . -B build-wasm3 -DWASMOS_DOTCONFIG=configs/wasm3_smp_defconfig && \
  cmake --build build-wasm3 --target run-qemu-test
cmake -S . -B build-warp  -DWASMOS_DOTCONFIG=configs/warp_smp_defconfig  && \
  cmake --build build-warp  --target run-qemu-test
```

## Guardrails

- Negative operation-error codes are NOT opcodes — they live in `abi/errors.yaml`
  (`skills/wasmos-add-error`). Opcodes are the positive `type` values only.
- Some opcode families are still hand-written OUTSIDE the IDL (Phase 3b): the
  scattered `rtc_ipc.h` copies, `font_ipc.h`, `gfx_ipc.h`, and `serial.c`. If you
  touch those, they are not generated yet — prefer folding them into
  `abi/opcodes.yaml` over hand-editing.
- Never hand-edit `abi/generated/**` — regenerate from `abi/opcodes.yaml`.
- Flags, sizes, and descriptor structs stay hand-written in `wasmos_driver_abi.h`.
