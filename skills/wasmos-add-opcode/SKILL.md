---
name: wasmos-add-opcode
description: Add or change a WASMOS IPC opcode (a positive ipc_message_t.type value) via the single-source IDL abi/opcodes.yaml + scripts/gen_abi_opcodes.py. Covers FIRST how to shape the message - a request descriptor in a transfer buffer is the default, four bare argument words are the exception, and packing two values into one word or adding a sibling opcode that differs only in how a parameter is expressed both mean the message outgrew its arguments - then the per-subsystem model, the REQ/RESP/ERROR convention, endpoint-scoped value collisions, what is generated (C enums in wasmos_driver_abi.h, the opcode->name diagnostic table, the doc tables, and the Rust/Go/Zig/AS constant files) vs hand-written, and how to validate. Use whenever adding or retiring an IPC opcode, and whenever deciding what a message should carry.
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
| `abi/generated/c/wasmos_opcodes.h` | per-subsystem C enums + subsystem-id constants + `wasmos_opcode_name(subsystem_id, type)` |
| `abi/generated/docs/opcodes.md` | per-subsystem reference tables |
| `abi/generated/rust/wasmos_opcodes.rs` | `pub const <SYM>: i32` |
| `abi/generated/go/wasmos_opcodes.go` | `const <SYM> int32` (`package wasmos`) |
| `abi/generated/zig/wasmos_opcodes.zig` | `pub const <SYM>: i32` |
| `abi/generated/assemblyscript/wasmos_opcodes.ts` | `export const <SYM>: i32` |

`wasmos_driver_abi.h` and the per-service headers (`rtc_ipc.h`, `font_ipc.h`,
`gfx_ipc.h`) plus `serial.c` all consume the generated C header via a relative
`#include`; the negative `*_ERR_*` codes (→ `abi/errors.yaml`), flags, and
descriptor structs stay hand-written in those headers.

Opcode values are **endpoint-scoped**: distinct services reuse the same ranges
on their own endpoints (e.g. `gfx` and `proc_manager` both use `0x200`; `font`
and `netdrv` both use `0xA00`). Each is its own subsystem in the IDL; the name
lookup is subsystem-scoped so it stays exact.

## Workflow

0. **Decide the message shape: four argument words, or a descriptor.**
1. Add the entry to the right subsystem in `abi/opcodes.yaml`.
2. Regenerate and verify.
3. Handle the message (send/reply/dispatch) in the driver/service.
4. Build and boot both runtimes.

## Step 0: Decide the message shape (do this first)

`ipc_message_t` is 32 bytes: four header words and **exactly four** opcode
words (`src/kernel/include/ipc.h`). That ceiling is not a budget to spend
cleverly. Every attempt to fit "just one more thing" into it has had to be
undone later, and the same mistake keeps being made because packing a value
*looks* like it worked.

**Default: a request DESCRIPTOR in a transfer buffer.** A struct the caller
fills, carried as `arg0 = buffer_id, arg1 = byte_offset, arg2 = size`, with the
CLIENT owning the buffer (`skills/wasmos-shared-primitives`,
`docs/architecture/12-dma-transfers.md`). Precedents: `DEVMGR_PUBLISH_DEVICE_DESC`
(which superseded `DEVMGR_PUBLISH_DEVICE` for exactly this reason — four words
could not describe six BARs) and `PROC_BROKER_IPC_SPAWN_PLAN_REQ`.

**Use bare arguments only** for a fixed, small set that will not grow — a
status, a handle, a count, a flag. "Will not grow" must be an argument you can
make, not a hope.

### Bright lines: use a descriptor if ANY of these is true

- More than about two independent values beyond an id or status.
- **You are writing a shift or a mask into an argument.** If you type
  `(x << 12) | y`, you have already lost. Two identities in one word is the
  clearest possible signal the message outgrew its arguments.
- A value that can grow: an LBA, a size, a byte count, an address, a string, a
  GUID, a list. A 32-bit LBA is a 2 TiB ceiling; a 32-bit sector count is the
  same ceiling on a different axis.
- You want a `reserved` argument, or a wildcard sentinel like `0xFF = any`.
- Identity and parameters would share the four words — hoist the identity into
  the descriptor rather than inferring it from the sender's endpoint.
- **You are about to add a sibling opcode that differs only in how a parameter
  is expressed.** `BLOCK_IPC_READ_REQ` and `BLOCK_IPC_READ_ZC_REQ` exist as two
  protocols solely because the destination could not be described in the words
  left over; as a descriptor field it is one opcode with a destination kind.

### The objection that is always wrong

*"This is the hot path, a transfer buffer is too expensive."*

It is not, and this has been measured in this tree. A client acquires and grants
**once per operation** and reuses both for every request in it — see the chunk
loop in `src/libc/zig/wasmos.zig` ("Own one buffer and grant the FS manager
once; reuse both across the whole call") and `fat_block_setup`, which acquires
once per driver. The per-request cost is one `xfer_buffer_write` of a few dozen
bytes into an already-mapped, already-granted buffer, next to a send host call,
a device transfer, an interrupt and a scheduler round trip.

What a descriptor does NOT absorb is bulk payload. Read data still lands in a
physical address or a borrowed buffer named BY the descriptor — that is what
makes the transfer zero-copy, and routing payload through the request would
undo it.

### Concurrency

Several in-flight requests need several regions, and a region cannot be recycled
until its reply lands. Give each in-flight slot its own offset — the same
slot-per-item discipline `pci_bus` uses when publishing devices — and bound the
slots the way `fs_fat` already bounds `FAT_MAX_INFLIGHT`.

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
- All opcode families are now in the IDL (`wasmos_driver_abi.h` core + the
  per-service `rtc`/`font`/`gfx`/`serial` subsystems). Add a new service's
  opcodes as a new subsystem here — don't hand-write a fresh `*_ipc.h` enum.
- Never hand-edit `abi/generated/**` — regenerate from `abi/opcodes.yaml`.
- Flags, sizes, and descriptor structs stay hand-written in `wasmos_driver_abi.h`.
