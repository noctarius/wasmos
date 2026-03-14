# WASMOS

WASMOS is a minimal x86_64 UEFI bootloader + kernel scaffold that boots a
small WASM-based user-space stack on top of `wasm3`.

The project is intentionally narrow:
- the bootloader stays deterministic and small
- the kernel keeps only low-level mechanisms
- drivers, services, and applications are WASM programs
- integration is validated through QEMU boot and CLI tests

IMPORTANT: Keep this file and `ARCHITECTURE.md` up to date with every prompt
execution and code iteration.
IMPORTANT: Create a git commit after each prompt iteration.

## Current Highlights
- UEFI boot via `BOOTX64.EFI`
- ELF64 kernel loading with aligned/overlap-safe `PT_LOAD` handling
- versioned `boot_info_t` handoff
- bootstrap `initfs.img` packaging for early WASMOS modules and boot config
- serial-first early boot diagnostics
- physical frame allocator and per-process CR3-managed paging
- preemptive round-robin scheduler driven by PIT IRQ0
- kernel IPC transport with endpoint ownership checks
- process manager with WASMOS-APP loading
- FAT-backed loading of `sysinit`, `cli`, and user apps
- manifest-driven late startup policy consumed by `sysinit`
- growable per-process `wasm3` heaps with a 2 GiB cap
- per-process virtual memory contexts with private user mappings
- shared user-space libc surface for C, Rust, Go, Zig, and AssemblyScript
- language-native application entrypoints behind a stable `wasmos_main` ABI

## Repository Layout
- `src/boot/` UEFI bootloader
- `src/kernel/` kernel core, scheduler, memory, IPC, runtime hosting
- `src/drivers/` WASM drivers
- `src/services/` WASM services
- `lib/libc/` shared user-space libc and language shims
- `examples/` WASM applications and smoke tests
- `tests/` QEMU-driven regression tests
- `scripts/` helper scripts and the QEMU test framework
- `ARCHITECTURE.md` system design and implementation baseline
- `TASKS.md` open work and follow-up items

## Boot and Startup Model

### Bootloader
`BOOTX64.EFI` is responsible for:
- loading `kernel.elf`
- loading `initfs.img`
- collecting the UEFI memory map
- copying the initfs blob into boot handoff memory
- synthesizing early `boot_module_t` entries from bootstrap-marked initfs apps
- exiting boot services
- jumping into the kernel entry point

### Kernel
The kernel owns:
- early CPU, paging, timer, and memory initialization
- process scheduling and IPC
- the process manager
- runtime hosting for WASM modules
- the `init` bootstrap sequence

### User Space
The current startup chain is:
1. bootloader loads `initfs.img`
2. initfs provides bootstrap WASMOS apps plus a generated boot-config blob
3. kernel `init` starts `hw-discovery` from the bootstrap module set
4. `hw-discovery` starts `ata` and `fs-fat`
5. kernel `init` waits for FAT readiness
6. kernel `init` asks PM to load `sysinit` from disk
7. `sysinit` parses the generated boot-config blob and starts its configured
   late user processes

This is the current stable bootstrap baseline.

## Initfs and Boot Config
The early bootstrap payload is a single `initfs.img` built from
[`scripts/initfs.toml`](/Volumes/git/wasmos/scripts/initfs.toml).

Current behavior:
- `scripts/make_initfs.py` reads the TOML manifest during the build
- bootstrap WASMOS modules are packed into one initfs image
- a small binary boot-config blob is generated from the TOML data and embedded
  as `config/bootcfg.bin`
- the bootloader exposes bootstrap apps through the existing boot-module
  mechanism and exposes the raw config blob through `boot_info_t`
- `sysinit` validates and reads the `sysinit.spawn` list from that blob at
  runtime, failing closed if the config is malformed

Current boot-config binary format:
- magic `WCFG0001`
- header fields: version, bootstrap-module count, sysinit-spawn count, string-table size
- bootstrap and sysinit string offsets
- NUL-terminated string table

Current `sysinit.spawn` rules:
- at least one process name must be present
- names must be unique
- names must fit the current 16-byte PM by-name spawn ABI

User-space access:
- `wasmos_boot_config_size()`
- `wasmos_boot_config_copy(ptr, len, offset)`

## Toolchain
- `clang` + `lld`
- `llvm-objcopy`
- `cmake` 3.20+
- `qemu-system-x86_64`

macOS note:
- use Homebrew LLVM; AppleClang cannot build the UEFI target correctly
- install with `brew install llvm lld qemu`

If auto-discovery fails during configure, pass:
```sh
cmake -S . -B build \
  -DCLANG=/path/to/llvm/bin/clang \
  -DLLD_LINK=/path/to/lld-link
```

## OVMF
If OVMF is not auto-detected, configure with:

```sh
cmake -S . -B build -DOVMF_CODE=/path/to/OVMF_CODE.fd
```

Optional vars image:

```sh
cmake -S . -B build \
  -DOVMF_CODE=/path/to/OVMF_CODE.fd \
  -DOVMF_VARS=/path/to/OVMF_VARS.fd
```

Homebrew note:
- QEMU often ships OVMF code at
  `/opt/homebrew/share/qemu/edk2-x86_64-code.fd`
- an x86_64 vars file is not always present

## Configure and Build

### Configure
```sh
cmake -S . -B build
```

### Core targets
```sh
cmake --build build --target bootloader
cmake --build build --target kernel
cmake --build build --target make_wasmos_app
```

### Run targets
```sh
cmake --build build --target run-qemu
cmake --build build --target run-qemu-debug
cmake --build build --target run-qemu-test
cmake --build build --target run-qemu-cli-test
```

Meaning:
- `run-qemu` boots the system in QEMU
- `run-qemu-debug` starts QEMU paused for GDB
- `run-qemu-test` performs a compile + boot + halt smoke run
- `run-qemu-cli-test` runs the full CLI integration suite

The repository standard is:
- use `run-qemu-test` after code changes
- use `run-qemu-cli-test` before declaring work complete

## Runtime and ABI Model

### Runtime
The supported runtime is `wasm3`.

Integration rules:
- the vendored runtime under `libs/wasm/wasm3` must not be modified
- runtime instances are process-local
- runtime mutation paths run with preemption disabled
- each process gets a growable runtime heap that can expand in chunks up to
  2 GiB without reserving one contiguous arena up front

Memory model notes:
- each process owns a separate root page table
- the scheduler switches CR3 on dispatch and restores the kernel root on return
- kernel identity/higher-half mappings stay shared across all address spaces
- process-visible WASM linear/stack/heap regions live in a private user window
- CPU privilege is still ring 0 for all tasks; this is address-space separation,
  not user-mode isolation yet

### WASMOS-APP
WASMOS-APP is the container format used by the process manager. It wraps:
- app name
- entry export name
- endpoint requirements
- capability requests
- memory hints
- raw WASM payload

Applications expose `wasmos_main` through a language shim.
Drivers and services expose `initialize`.

Current heap-hint behavior:
- stack hints are applied at runtime creation
- heap hints seed the preferred initial chunk size for the runtime allocator
- the current maximum heap cap is 2 GiB per process
- `max_pages` metadata is not enforced yet

## Language Shims

### C
- user-facing entrypoint: `int main(int argc, char **argv)`
- shim-owned export: `wasmos_main`

### Rust
- user-facing entrypoint: `fn main(args: &[&str]) -> i32`
- shim-owned export: `wasmos_main`

### Go (TinyGo)
- user-facing entrypoint: `func Main(args []string) int32`
- shim-owned export: `wasmos_main`

### AssemblyScript
- user-facing entrypoint: `main(args: Array<string>): i32`
- toolchain-owned root module exports `wasmos_main`

### Zig
- user-facing entrypoint remains Zig-native `main`
- shim-owned export: `wasmos_main`

## Shared User-Space libc
`lib/libc/` provides the shared user-space surface used by applications,
drivers, and services.

Current functionality includes:
- string and character helpers
- minimal `printf`/`snprintf`
- `putsn`
- integer parsing helpers
- IPC helper headers
- minimal read-only file API

Current file I/O scope:
- `open`, `read`, `close`
- `fopen`, `fread`, `fgets`, `fgetc`, `fclose`
- no write/seek/stat yet

## Implemented Drivers and Services

### Drivers
- `ata`: PIO ATA block device driver
- `fs-fat`: FAT12/16/32 filesystem service on top of `ata`
- `chardev`: character-device IPC service

### Services
- `process-manager`: spawn/wait/kill/status plus WASMOS-APP loading
- `hw-discovery`: ACPI-driven early storage bootstrap
- `sysinit`: late user-process startup from manifest-generated boot config
- `cli`: shell over `proc` and `fs`

## CLI
Current built-in commands:
- `help`
- `ps`
- `ls`
- `cat <path>`
- `cd <path>`
- `exec <app>`
- `halt`
- `reboot`

## Tracing
`WASMOS_TRACE` is a build-time switch and defaults to `OFF`.

With tracing disabled:
- verbose init / PM / scheduler / sysinit traces are hidden
- `debug_mark(tag)` output is hidden
- periodic `[timer] ticks` markers are hidden
- normal boot output remains visible

Enable tracing with:
```sh
cmake -S . -B build -DWASMOS_TRACE=ON
```

## Example and Smoke Apps
- `init-smoke`
- `native-call-min`
- `native-call-smoke`
- `fs-open-smoke`
- `chardev-preempt`
- per-language hello apps (`hello-c`, `hello-rust`, `hello-go`, `hello-as`, `hello-zig`)

These are not just samples; several are part of the regression strategy.

## Testing
The QEMU test framework lives in `scripts/qemu_test_framework.py`.

Coverage currently includes:
- boot smoke
- CLI interaction
- filesystem open/read path
- language hello apps
- init smoke
- IPC wakeup
- preemption smoke
- scheduler/timer forward-progress smoke
- chardev preemption exercise

## Development Rules
- prefer `rg` for searching
- keep changes small and explicit
- do not modify vendored dependencies
- keep build logic in per-component `CMakeLists.txt` files
- update `README.md` and `ARCHITECTURE.md` when behavior changes
- commit after each prompt iteration

## Open Work
Active follow-up work is tracked in `TASKS.md`.

Broad areas still open:
- ring 3 / syscall transition
- richer IPC and shared-memory paths
- driver-manager and better hardware inventory
- config-driven startup consumption in `sysinit`
- broader filesystem support
- stronger capability and privilege separation
