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

WASMOS is a tiny OS playground built almost entirely through AI-assisted coding:
basically no line was typed directly by hand, but many were produced through
aggressively hands-on guidance, nudging, and occasional emotional support for
coding agents. It is intentionally basic and absolutely not tuned for speed,
especially because it uses `wasm3` (an interpreter-only WebAssembly runtime).
The point is not benchmark glory; the point is to explore ideas, run
experiments, and learn how to better steer coding agents without setting your
keyboard on fire.

Threading design work is tracked in `docs/THREADING.md`, which defines the planned
process/thread split, scheduler updates, and phased rollout for kernel threads.

## WASMOS Kernel Architecture

WASMOS is a minimal x86_64 UEFI bootloader + kernel scaffold that boots a
small WASM-first user-space stack on top of `wasm3`, with optional native ELF
drivers for hardware paths that need full native speed.

The project is intentionally narrow:
- the bootloader stays deterministic and small
- the kernel keeps only low-level mechanisms
- services and applications are WASM programs; drivers can be WASM or native
  ELF payloads inside WASMOS-APP
- integration is validated through QEMU boot and CLI tests

If the boot log feels a little too serious, Wasmo is here to remind you that
the system still boots better when the mascot is confident.

IMPORTANT: Keep this file and `docs/ARCHITECTURE.md` up to date with every prompt
execution and code iteration.
IMPORTANT: Create a git commit after each prompt iteration.

## Current Highlights
- Deterministic UEFI boot path: `BOOTX64.EFI` loads `kernel.elf` + `initfs.img`,
  builds versioned `boot_info_t`, then hands off to the kernel.
- Microkernel baseline is operational: paging, physical memory, preemptive
  scheduling, IPC, process lifecycle, and exception handling are in place.
- Runtime split is active: policy and most services/drivers run as WASM apps,
  with selected hardware-facing components running as native drivers through
  the WASMOS-APP/native ABI path.
- Storage-first startup is implemented: `hw-discovery` brings up storage,
  `fs-fat` enables FAT-backed loading, and `sysinit` executes config-driven
  post-FAT startup.
- VT/CLI stack is usable: multi-TTY operation, tty switching, per-tty CLI
  ownership, and bounded backpressure behavior are implemented.
- Ring-3 hardening is enabled by default in normal test boots, with dedicated
  ring-3 smoke coverage kept as a separate target.
- Shared-memory transport is established for high-volume console/framebuffer
  flow, reducing IPC pressure on display output.
- Capability-aware policy checks are enforced for privileged operations
  (e.g. IRQ routing and system-control paths).

For implementation-level details and subsystem status, see
`docs/ARCHITECTURE.md` and the feature docs under `docs/architecture/`.

## Repository Layout
- `src/boot/` UEFI bootloader
- `src/kernel/` kernel core, scheduler, memory, IPC, runtime hosting
- `src/drivers/` WASM and native drivers
- `src/services/` WASM services
- `lib/libc/` shared user-space libc and language shims
- `examples/` WASM applications and smoke tests
- `tests/` QEMU-driven regression tests
- `scripts/` helper scripts and the QEMU test framework
- `docs/ARCHITECTURE.md` architecture index and design entry point
- `docs/architecture/` feature-level architecture and implementation documents
- `TASKS.md` open work and follow-up items
- `docs/RING3_ISOLATION_PLAN.md` phased design/tasks for full ring-3 isolation
- `wasmo.svg` / `wasmo.webp` repo-local mascot art used by this README
- `wasmos_wordmark.svg` / `wasmos_wordmark.webp` repo-local wordmark art used
  by this README

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
`BOOTX64.EFI` also attempts to capture the GOP framebuffer. If GOP protocols are missing it now falls back to scanning VGA PCI BARs so drivers can map the QEMU framebuffer and paint a gradient. PCI fallback logs now show when handle discovery or BAR selection fails so we can tell whether no VGA device was found.

### Kernel
The kernel owns:
- early CPU, paging, timer, and memory initialization
- process scheduling and IPC
- the process manager
- runtime hosting for WASM modules and native driver entry dispatch
- the `init` bootstrap sequence

### User Space
The current startup chain is:
1. bootloader loads `initfs.img`
2. initfs provides bootstrap WASMOS apps plus a generated boot-config blob
3. kernel `init` starts `hw-discovery` from the bootstrap module set
4. `hw-discovery` starts `ata` and `fs-fat`
5. kernel `init` waits for FAT readiness
6. kernel `init` asks PM to load `sysinit` from disk
7. `hw-discovery` starts post-FAT display/input drivers by name from disk
8. `sysinit` parses the generated boot-config blob and starts its configured
   services and late user processes

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
cmake --build build --target run-qemu-ring3-test
cmake --build build --target strict-ring3
cmake --build build --target run-qemu-ui-test
```

Meaning:
- `run-qemu` boots the system in QEMU
- `run-qemu-debug` starts QEMU paused for GDB
- `run-qemu-test` performs a compile + boot + halt smoke run
- `run-qemu-cli-test` runs the full CLI integration suite
- `run-qemu-ring3-test` configures a shadow `build/ring3` tree with
  `WASMOS_RING3_STRICT=ON` and `WASMOS_RING3_SMOKE=ON`, and asserts ring3
  smoke syscall markers plus
  `[mode] strict-ring3=1`, `native-call-smoke: ipc-call ok`,
  `[test] ring3 native abi ok`, and structured
  user fault telemetry (`[fault] user-pf ... reason=user_to_kernel`) before halt
- `strict-ring3` runs the phase-0 gate profile (`run-qemu-test` then
  `run-qemu-ring3-test`) in sequence
- staged-default policy: strict ring3 policy stays ON for normal boot targets,
  while ring3 smoke probes stay OFF for normal targets and are ON by default
  in the dedicated `run-qemu-ring3-test` path
- `run-qemu-ui-test` boots QEMU with a graphical display plus `mon:stdio` serial

The repository standard is:
- use `run-qemu-test` after code changes
- use `run-qemu-cli-test` before declaring work complete
- use `run-qemu-ring3-test` when validating ring3 syscall/trampoline progress
- the `kernel_ide` aggregation target also carries `${LIBC_DIR}/include` so
  CLion and other CMake IDEs can resolve `wasmos/api.h` for indexed WASM
  drivers, services, and C examples

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
- user roots keep a private low-slot PDPT snapshot for the identity/direct-map
  subset and share only the bounded higher-half kernel alias window
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
- raw payload bytes (WASM module or native ELF driver)

Applications expose `wasmos_main` through a language shim.
Drivers and services expose `initialize`.

Current driver flags:
- `WASMOS_APP_FLAG_DRIVER`: driver payload
- `WASMOS_APP_FLAG_NATIVE`: payload is native ELF (valid only with
  `WASMOS_APP_FLAG_DRIVER`)

Current heap-hint behavior:
- stack hints are applied at runtime creation
- heap hints seed the preferred initial chunk size for the runtime allocator
- the current maximum heap cap is 2 GiB per process
- `max_pages` metadata is not enforced yet

## Language Shims

### C
- user-facing entrypoint: `int main(int argc, char **argv)`
- shim-owned export: `wasmos_main`
- file I/O currently supports `open/read/close`, `stat`, `lseek`, `fseek`, and
  `ftell` on the FAT-backed read-only path

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
- `open`, `read`, `write`, `close`, `stat`, `lseek`
- `fopen`, `fread`, `fgets`, `fgetc`, `fclose`, `fseek`, `ftell`
- reads now span multi-cluster FAT files through the shared FS IPC path used by
  C, Rust, Go, Zig, and AssemblyScript shims
- seek/stat are available through libc and the language shims
- writes are currently limited to C libc `open(..., O_WRONLY)` plus `write`
  against existing files without growth, create, truncate, or append semantics

## Implemented Drivers and Services

### Drivers
- `ata`: PIO ATA block device driver
- `fs-fat`: FAT12/16/32 filesystem service on top of `ata`
- `chardev`: character-device IPC service
- `serial`: AssemblyScript-backed COM1 console driver that registers via `serial_register`
- `framebuffer`: optional native C driver packed as
  `WASMOS_APP_FLAG_DRIVER | WASMOS_APP_FLAG_NATIVE`; it probes framebuffer
  info, maps the physical framebuffer into the driver context through the
  native-driver API, and paints a gradient at native speed
- `keyboard`: AssemblyScript PS/2 driver that polls for scancodes and remains idle when no keyboard replies

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
- dedicated ring3 target regression (`tests/test_ring3_smoke_target.py`) that
  executes `cmake --build build --target run-qemu-ring3-test`

Current QEMU note:
- the `fs-write-smoke` flow keeps directory/unlink coverage for nested files and
  rmdir behavior, but avoids unlinking the grown top-level `create.txt` test
  file because some QEMU `vvfat` versions can assert in host commit paths for
  that specific sequence

## Development Rules
- prefer `rg` for searching
- keep changes small and explicit
- do not modify vendored dependencies
- keep build logic in per-component `CMakeLists.txt` files
- update `README.md` and `docs/ARCHITECTURE.md` when behavior changes
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
