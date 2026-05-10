<p align="center">
  <picture>
    <source srcset="wasmos_wordmark-light.webp" type="image/webp" media="(prefers-color-scheme: light)">
    <source srcset="wasmos_wordmark-dark.webp" type="image/webp" media="(prefers-color-scheme: dark)">
    <img src="wasmos_wordmark-light.svg" alt="WASMOS wordmark" width="520">
  </picture>
</p>

<p align="center">
  <picture>
    <source srcset="wasmo.webp" type="image/webp">
    <img src="wasmo.svg" alt="Wasmo the WASMOS mascot" width="180">
  </picture>
</p>

<p align="center"><strong>Small boot path. Small kernel. Large mascot and agent energy.</strong></p>

WASMOS is a minimal x86_64 UEFI OS playground with a small microkernel core
and a WASM-first user-space stack (`wasm3`), plus optional native drivers for
hardware paths that benefit from native execution.

It is designed for experimentation, not production use.

For contributors and coding agents: read `AGENTS.md` before making changes.
It defines repository workflow and documentation/update conventions.

## Current Highlights
- Deterministic UEFI boot handoff (`BOOTX64.EFI` -> `kernel.elf` + `initfs.img`)
- Microkernel baseline: paging, scheduling, IPC, process lifecycle, exceptions
- WASM-first runtime model with optional native driver payloads
- Storage-first startup chain (`hw-discovery` -> `fs-fat` -> `sysinit`)
- Usable VT/CLI stack with multi-TTY switching
- Ring-3 hardening enabled by default in normal test boots
- Ring-3 smoke includes process-local `#PF`, `#UD`, `#GP`, `#DE`, `#DB`, `#OF`, `#NM`, `#SS`, and `#AC` fault-policy checks
- Ring-3 smoke includes shared-memory owner/grant/revoke isolation checks (kernel and user-space app-pair paths)
- Shared-memory app-pair smoke now also checks forged-ID deny, map-argument policy deny, and post-revoke stale-ID deny
- Strict ring3 boot smoke now includes a kernel-level shared-memory misuse matrix marker (`[test] ring3 shmem misuse matrix ok`)
- Kernel threading Phase B now includes schedulable internal worker threads with per-thread kernel stacks (`[test] threading internal worker ok`)
- Kernel threading join-order smoke now validates in-process thread-join wake ordering in a dedicated probe path (`[test] threading join wake order ok`)
- Kernel threading Phase B now includes a targeted multi-thread IPC stress marker (`[test] threading ipc stress ok`)
- Threading Phase C syscall baseline now includes native ring3 `gettid` and `thread_yield` coverage (`[test] ring3 native gettid ok`, `[test] ring3 thread yield syscall ok`)
- Threading Phase C syscall baseline now includes native ring3 `thread_exit` coverage (`[test] ring3 thread exit syscall ok`)
- Threading Phase C now includes native ring3 `thread_create` coverage with per-thread user context setup (`[test] ring3 thread create syscall ok`)
- Threading Phase C syscall baseline now includes native ring3 `thread_join` entry and self-join deny coverage (`[test] ring3 thread join syscall ok`, `[test] ring3 thread join self deny ok`)
- Threading Phase C syscall baseline now includes native ring3 `thread_detach` entry plus invalid-argument and detach-then-join deny coverage (`[test] ring3 thread detach syscall ok`, `[test] ring3 thread detach invalid deny ok`, `[test] ring3 thread detach join deny ok`)
- Threading Phase C now includes a user-facing continuation-style native thread API wrapper (`wasmos/thread_x86_64.h`) for native ring3 callers
- Threading lifecycle smoke now also validates kill-while-blocked wait wakeup behavior (`[test] threading wait kill wake ok`)
- Threading Phase D hardening markers now include join-after-kill ordering and kill-during-join waiter wakeup checks (`[test] threading join after kill order ok`, `[test] threading join kill wake ok`)

## Quick Start

### Requirements
- `clang` + `lld`
- `llvm-objcopy`
- `cmake` 3.20+
- `qemu-system-x86_64`

macOS note:
- use Homebrew LLVM (`appleclang` is not sufficient for the UEFI target)
- install with: `brew install llvm lld qemu`

### Configure
```sh
cmake -S . -B build
```

If tool autodiscovery fails:
```sh
cmake -S . -B build \
  -DCLANG=/path/to/llvm/bin/clang \
  -DLLD_LINK=/path/to/lld-link
```

If OVMF autodiscovery fails:
```sh
cmake -S . -B build -DOVMF_CODE=/path/to/OVMF_CODE.fd
```

Optional vars image:
```sh
cmake -S . -B build \
  -DOVMF_CODE=/path/to/OVMF_CODE.fd \
  -DOVMF_VARS=/path/to/OVMF_VARS.fd
```

### Build
```sh
cmake --build build --target bootloader
cmake --build build --target kernel
cmake --build build --target make_wasmos_app
```

### Run
```sh
cmake --build build --target run-qemu
cmake --build build --target run-qemu-debug
cmake --build build --target run-qemu-ui-test
```

### Test
```sh
cmake --build build --target run-qemu-test
cmake --build build --target run-qemu-cli-test
cmake --build build --target run-qemu-ring3-test
cmake --build build --target run-qemu-ring3-threading-test
```

Target summary:
- `run-qemu`: normal boot in QEMU
- `run-qemu-debug`: paused boot for GDB attach
- `run-qemu-test`: compile + boot + halt smoke
- `run-qemu-cli-test`: CLI integration suite
- `run-qemu-ring3-test`: strict ring-3 smoke path
- `run-qemu-ring3-threading-test`: opt-in strict ring-3 threading smoke (ring3-threading spawn + ring3 thread `create`/`join`/`detach` syscall markers including detach-then-join deny + wait/kill wake marker)

## Startup Model
Boot sequence (high level):
1. `BOOTX64.EFI` loads `kernel.elf` and `initfs.img`
2. Kernel boots, initializes core subsystems, starts `init`
3. `init` starts `hw-discovery`
4. `hw-discovery` starts storage drivers/services (`ata`, `fs-fat`)
5. `init` requests `sysinit` load from FAT via process manager
6. `sysinit` starts configured services/apps from boot config

## Repository Layout
- `src/boot/`: UEFI bootloader
- `src/kernel/`: kernel core
- `src/drivers/`: drivers (WASM and native)
- `src/services/`: services
- `lib/libc/`: shared user-space libc + shims
- `examples/`: sample/smoke apps
- `tests/`: QEMU-driven tests
- `scripts/`: build/test helpers
- `docs/`: architecture/design docs

## Documentation Index
- `docs/ARCHITECTURE.md`: architecture index
- `docs/architecture/`: feature-level architecture docs
- `docs/architecture/14-ring3-isolation-and-separation.md`: ring-3 isolation and kernel/user-space separation design
- `docs/THREADING.md`: threading design and rollout
- `docs/TASKS.md`: active and planned work
- `AGENTS.md`: contributor/agent workflow and repository rules

## Runtime Model (Brief)
- Runtime host: `wasm3`
- Process manager loads WASMOS-APP payloads
- Payloads can be WASM apps/services or native driver payloads

For the complete ABI/runtime contract and subsystem details, use the
architecture docs under `docs/architecture/`.
