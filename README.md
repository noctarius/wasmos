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

Threading design work is tracked in `THREADING.md`, which defines the planned
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

IMPORTANT: Keep this file and `ARCHITECTURE.md` up to date with every prompt
execution and code iteration.
IMPORTANT: Create a git commit after each prompt iteration.

## Current Highlights
- UEFI boot via `BOOTX64.EFI`
- ELF64 kernel loading with aligned/overlap-safe `PT_LOAD` handling
- versioned `boot_info_t` handoff
- bootstrap `initfs.img` packaging for the storage bootstrap path and boot
  config; display/input drivers now stay on the FAT image and are started by
  `hw-discovery` after `fs-fat` is available
- serial-first early boot diagnostics
- an AssemblyScript `serial` driver loads from FAT through `hw-discovery` and calls
  `serial_register`, letting the kernel hand off console output from the COM1
  stub to the new service once it is ready
- an AssemblyScript keyboard driver is launched from FAT by `hw-discovery` and polls the
  PS/2 controller for scancodes; it stays running even if no input arrives so
  the kernel does not need any keyboard-specific logic in its microkernel core
- native ELF driver loading through the process manager for
  `WASMOS_APP_FLAG_DRIVER | WASMOS_APP_FLAG_NATIVE` payloads
- a freestanding native-driver ABI (`wasmos_driver_api_t`) shared between
  kernel and native drivers
- a native C framebuffer driver that maps the physical framebuffer into driver
  space and paints at native speed without wasm3; it now honors the
  bootloader-captured framebuffer size and ignores larger post-boot Bochs VBE
  geometry until mode-setting can update the kernel framebuffer contract
- the native framebuffer driver is now a post-FAT startup target: the kernel
  early framebuffer remains the pre-FAT display path, and `hw-discovery` starts
  the native framebuffer before `sysinit` launches VT and CLI
- the native framebuffer driver now explicitly registers its text-control IPC
  endpoint with process-manager so VT receives a valid framebuffer endpoint for
  tty clear/replay control (instead of falling back to logical-only switching)
- kernel-owned 1-page shared-memory console ring: `serial_write` appends text
  and the native framebuffer driver drains it directly (no serial→framebuffer
  text IPC forwarding)
- fatal CPU exceptions now render a kernel panic screen directly on the
  framebuffer (black background) with crash context: vector/register state,
  process identity, stack bounds, CR3, kernel text range, and framebuffer info
- shared-memory APIs for native drivers (`shmem_create/map/unmap`,
  `console_ring_id`) and WASM syscalls (`wasmos_shmem_create/map/unmap`)
- VT service now tracks per-TTY state (4 slots), including active-TTY switching
  via `VT_IPC_SWITCH_TTY` and per-TTY attributes via `VT_IPC_SET_ATTR_REQ`
  while keeping console output routed through `wasmos_console_write`
- `tty0` is the system console mirror (serial/console-ring). Switching to
  `tty1+` disables console-ring drain and replays VT-managed framebuffer cells;
  every tty switch clears the framebuffer first, then replays the selected tty
  buffer; switch-time clear/replay now uses a reliable framebuffer IPC path to
  avoid dropped redraws under transient queue pressure; VT now treats
  control-plane switch IPC failures as switch failures (instead of reporting
  false success), so `tty N` can report an error when mode/clear operations do
  not complete. Replay remains best-effort under sustained queue pressure to
  avoid abort loops; VT commits tty-generation/active-tty atomically with
  successful switch control operations and restores prior console mode on
  failure, avoiding half-switched visual states; the native framebuffer driver
  now prioritizes control IPC over ring-drain work and drains ring output in
  bounded chunks so switch commands are not delayed behind large console
  backlogs; switching back to `tty0` re-enables ring drain in live-tail mode
  (stale backlog is dropped on re-enable), avoiding multi-screen catch-up floods
  that could destabilize follow-on tty operations
- CLI now attaches to VT `tty1` by default and writes through `VT_IPC_WRITE_REQ`
  (with `tty 0..3` command for manual switching)
- CLI keeps VT as the primary interactive path on `tty1+`; VT write retries
  were raised to reduce dropped output chunks under transient queue pressure
  during larger command output bursts (for example `ls`)
- `ls`/`cat` output is now returned from `fs-fat` as requester-scoped IPC stream
  chunks and rendered by the requesting CLI tty, so filesystem listings/content
  stay on the active virtual tty instead of disappearing into `tty0`-only
  console output
- CLI now surfaces VT switch error codes (`tty switch failed: <code>`) so
  failures can be tied to the precise switch stage during diagnostics
- CLI `cd` now preserves shell-like path semantics for `.` (stay in cwd) and
  `..` (move to parent) instead of forcing root
- sysinit now keeps one CLI instance per VT tty (`tty1..tty3`), with process
  manager assigning each CLI a home tty; only the foreground tty's CLI consumes
  keyboard input
- VT now owns keyboard input routing and delivers per-tty raw input via
  `VT_IPC_READ_REQ`; in raw mode, extended arrows and nav/edit keys are emitted
  as ANSI escape sequences (`ESC[A/B/C/D`, `ESC[H/F`, `ESC[5~/6~`, `ESC[2~/3~`).
  Raw printable key translation now also applies Shift-modified ASCII symbols
  (for example `Shift+7` => `/` on the configured Set-1 map).
  CLI owns line editing/echo and now handles `Up/Down` (`ESC[A` / `ESC[B`) as
  shell-history navigation in raw mode, with serial input retained as fallback
  for headless/automation flows
- VT output is now source-tty scoped: each CLI endpoint writes to its assigned
  tty buffer, and background tty writes no longer paint the active framebuffer
- kernel-hosted WASM `console_write` now mirrors to the active VT tty as
  best-effort kernel-origin VT write chunks while retaining serial output, so
  app/runtime logs remain visible on framebuffer sessions without regressing
  serial-based diagnostics
- VT now decodes a core ANSI/VT100 CSI subset per tty buffer: cursor movement
  (`A/B/C/D/H/f`), save/restore cursor (`s/u`), erase display/line (`J/K`),
  cursor show/hide (`?25h/l`), and 16-color SGR (`m`)
- VT now queries framebuffer text geometry at startup and sizes tty buffers to
  the runtime grid (bounded by 160x64) instead of fixed 80x25, so scrolling
  and cursor bounds match the visible framebuffer text area. Per-tty cell
  buffers are now allocated dynamically from WASM linear memory at startup
  (growing memory on demand), with fallback to default geometry if larger-grid
  allocation fails
- VT now exposes `VT_IPC_SET_MODE_REQ` to configure per-tty input mode
  (`raw`, `canonical`, `echo`) without changing writer/read ownership rules
- VT canonical mode now handles core line-discipline controls in-service
  (`Backspace`, `Ctrl+U`, `Ctrl+C`) and per-tty history navigation via
  `Up/Down` arrows (with `Ctrl+P` / `Ctrl+N` fallback), so cooked-mode clients
  do not need to reimplement baseline editing behavior
- VT now requires explicit writer registration (`VT_IPC_REGISTER_WRITER`) and
  tags tty output with a switch-generation token so stale pre-switch writes are
  dropped instead of repainting over a freshly replayed tty
- VT replies now use bounded retry/yield behavior under IPC queue pressure, so
  transient full queues are less likely to drop switch/query responses and
  produce CLI-side timeout failures
- VT switch/write-drop diagnostics now emit compact `wasmos_debug_mark` tags
  (when `WASMOS_TRACE=1`) into the existing global kernel trace stream
- Known deferred issue: an intermittent framebuffer-only prompt duplication /
  spacing artifact during very rapid `Ctrl+Shift+F1..F4` switching was observed
  previously; it has not reproduced again recently, and trace hooks remain in
  place for a future focused repro/debug pass
- VT supports keyboard hotkey switching with `Ctrl+Shift+F1..F4` mapped to
  `tty0..tty3`; while viewing `tty0`, plain `F2/F3/F4` also switches directly
  to `tty1..tty3` as a recovery path when modifier-state tracking is out of
  sync
- entering `tty0` now renders a short read-only hint line so an otherwise blank
  live-tail screen is visibly intentional
- keyboard notify events use fire-and-forget IPC (`request_id = 0`) and VT/CLI
  output paths now use bounded queue-full retries so transient framebuffer/IPC
  backpressure does not hard-lock interactive input loops
- physical frame allocator and per-process CR3-managed paging
- preemptive round-robin scheduler driven by PIT IRQ0
- kernel IPC transport with endpoint ownership checks
- IPC endpoint table scaled to 128 entries with endpoint reclamation on process
  reap to keep repeated app exec/file-I/O flows stable
- process manager with WASMOS-APP loading
- FAT-backed loading of `sysinit`, `cli`, and user apps
- manifest-driven late startup policy consumed by `sysinit`
- multi-cluster FAT file reads for libc and app loading
- read-only FAT seek/stat support for libc and language shims
- overwrite-only FAT writes for existing files through the C libc `open/write`
  path
- `O_TRUNC` support for shrinking existing FAT files through the same low-level
  write path
- `O_APPEND` support for extending existing FAT files within their current
  cluster chain
- `O_CREAT` support for zero-length 8.3 FAT files in existing directories
- FAT12/16 cluster allocation for file growth, including writes to new files
- FAT file unlink support with cluster-chain reclamation for regular files
- FAT directory create/remove support for empty directories
- C stdio write/append modes via `fopen`/`fwrite`
- C libc `unlink` support
- C libc `mkdir`/`rmdir` support
- Rust, Zig, Go, and AssemblyScript fs shims can now create/write/append/unlink files
  and create/remove directories
- FAT new-file creation supports long filenames with generated short aliases
- growable per-process `wasm3` heaps with a 2 GiB cap
- x86 privilege-boundary groundwork: IDT `int 0x80` syscall gate is now
  present with a minimal syscall dispatcher (`nop`, `getpid`, `exit`,
  `yield`, `wait`, `ipc_notify`, `ipc_call`) as the initial ring3 boundary
  primitive
- scheduler/context-switch groundwork now carries per-context privilege
  metadata (`cs/ss/user_rsp`) and can restore ring3 contexts via `iretq`
  while retaining the existing ring0 fast-path (`ret`)
- scheduler now updates TSS `rsp0` per selected process so user-mode trap/syscall
  entry has a deterministic kernel stack landing point
- kernel startup now includes a ring3 smoke process (`ring3-smoke`) that
  installs a minimal user-mode code blob in the process linear region, switches
  to CPL3 via `process_set_user_entry`, validates syscall boundary flow by
  emitting `[test] ring3 syscall ok` on the first user-mode `getpid` syscall,
  now probes `ipc_notify` with both deny (`[test] ring3 ipc syscall deny ok`)
  and allow (`[test] ring3 ipc syscall ok`) paths, probes `ipc_call` with deny
  (`[test] ring3 ipc call deny ok`, plus error-path `RDX=0` contract marker
  `[test] ring3 ipc call err rdx zero ok`), permission-denied
  (`[test] ring3 ipc call perm deny ok`), and echo-allow
  (`[test] ring3 ipc call ok`) paths, issues an explicit CPL3 `yield`
  (`[test] ring3 yield syscall ok`), and runs a 4096-call CPL3
  `getpid` loop before exit; if the full loop
  completes, the kernel emits `[test] ring3 preempt stress ok` to confirm timer
  preemption/trampoline flow stayed stable under sustained user-mode syscall
  traffic; default spawn remains disabled while this path is still being
  soak-tested; ring3 smoke mode also spawns a compiled native probe process
  (`ring3-native`) built from C against `wasmos/syscall_x86_64.h`, and the
  syscall layer emits `[test] ring3 native abi ok` when that path executes
- timer preemption now uses a ring3-safe IRQ trampoline handoff: CPL3 interrupt
  frames are rewritten to return through a kernel CS trampoline before scheduler
  context switch, rather than attempting a user-mode return into kernel text
- process-owned user regions now carry an explicit user mapping flag from
  memory policy into paging (including intermediate table propagation) so
  ring3-accessible mappings are representable
- capability-based driver resource groundwork: per-context capability registry
  (`io.port`, `irq.route`, `mmio.map`, `dma.buffer`) is now wired into
  WASMOS-APP capability grants, and WASM I/O hostcalls enforce `io.port` when
  a context has explicit capability policy configured
- optional small-object slab allocator scaffold (`kalloc_small`/`kfree_small`)
  added for incremental adoption without replacing existing static tables
- per-process virtual memory contexts with private user mappings
- shared user-space libc surface for C, Rust, Go, Zig, and AssemblyScript
- language-native application entrypoints behind a stable `wasmos_main` ABI
- native x86_64 ring3 syscall helper header now available at
  `lib/libc/include/wasmos/syscall_x86_64.h` for non-WASM userland code paths
  that call `int 0x80` directly; `ipc_call` currently returns status in `RAX`
  and reply `arg0` in `RDX` (with `RDX=0` on error), and blocks by yielding
  until a matching `request_id` reply is received

## Repository Layout
- `src/boot/` UEFI bootloader
- `src/kernel/` kernel core, scheduler, memory, IPC, runtime hosting
- `src/drivers/` WASM and native drivers
- `src/services/` WASM services
- `lib/libc/` shared user-space libc and language shims
- `examples/` WASM applications and smoke tests
- `tests/` QEMU-driven regression tests
- `scripts/` helper scripts and the QEMU test framework
- `ARCHITECTURE.md` system design and implementation baseline
- `TASKS.md` open work and follow-up items
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
cmake --build build --target run-qemu-ui-test
```

Meaning:
- `run-qemu` boots the system in QEMU
- `run-qemu-debug` starts QEMU paused for GDB
- `run-qemu-test` performs a compile + boot + halt smoke run
- `run-qemu-cli-test` runs the full CLI integration suite
- `run-qemu-ring3-test` configures a shadow `build/ring3` tree with
  `WASMOS_RING3_SMOKE=ON` and asserts ring3 smoke syscall markers plus
  `native-call-smoke: ipc-call ok` and `[test] ring3 native abi ok` before halt
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
