# Architecture Notes

This document is the repository's technical design baseline. It describes the
boot contract, the microkernel split, the current implementation state, and the
next layers that still need to be built.

IMPORTANT: Keep this file and `README.md` up to date with every prompt execution
and code iteration.
IMPORTANT: Create a git commit after each prompt iteration.

## Goals
- Boot an x86_64 kernel through UEFI with a deterministic, auditable handoff.
- Keep the kernel small: scheduling, IPC, memory, interrupts, and runtime
  hosting are kernel responsibilities; policy lives in WASM services.
- Treat services and applications as isolated WASM programs and allow selected
  drivers to run as native ELF payloads when needed, all behind explicit IPC
  contracts instead of implicit in-kernel calls.
- Preserve a stable boot and process ABI while still allowing the system to
  evolve incrementally.

## Current System Summary
The current tree already boots into a usable user-space stack:
- `BOOTX64.EFI` loads `kernel.elf` plus a single `initfs.img`, gathers the
  memory map, materializes bootstrap boot modules from the initfs, and exits
  boot services.
- The kernel initializes paging, physical memory management, exceptions, the
  timer, IPC, the scheduler, and the process manager.
- A kernel-owned `init` process starts `hw-discovery`, waits for `fs-fat` to
  become ready, and then asks the process manager to load `sysinit` from the
  FAT filesystem.
- `sysinit` is intentionally small and only starts the late user processes
  listed in the generated boot-config blob.
- The initfs also carries a generated binary boot-config blob derived from
  `scripts/initfs.toml` for config-driven startup.
- `fs-fat` currently provides read-only open/read/seek/stat primitives for the
  shared libc layer and the language-native shims.
- `fs-fat` also supports overwrite-only writes to existing files through the C
  libc `open/write` path, plus `O_TRUNC` size updates, `O_APPEND` writes for
  existing files within their current cluster chain, and `O_CREAT` for
  zero-length 8.3 files in existing directories; FAT12/16 cluster allocation
  now grows files and newly created files, long-filename creates now emit LFN
  entries plus a generated short alias, regular-file unlink now reclaims the
  cluster chain and tombstones the short+LFN dir entries, empty-directory
  create/remove now allocates a directory cluster plus `.`/`..` entries, and
  the C stdio shim now exposes `fopen`/`fwrite` write and append modes. The C
  libc now exposes `unlink`, `mkdir`, and `rmdir`, and the Rust, Zig, Go, and
  AssemblyScript shims expose matching create/write/append/unlink/mkdir/rmdir
  helpers. Update modes such as `r+`/`w+`/`a+` and non-ASCII LFN creation
  remain future work.
- The runtime host uses `wasm3`, not WAMR.
- The process manager also supports native ELF drivers wrapped in WASMOS-APP as
  `FLAG_DRIVER|FLAG_NATIVE`, loaded directly into a process context and called
  through a kernel-provided function-table ABI.
- Serial-to-framebuffer text handoff now uses a kernel-created shared-memory
  console ring (1 page). `serial_write` appends bytes into this ring, and the
  native framebuffer driver maps and drains it, removing the previous
  serial→framebuffer text IPC message path.
- Shared-memory primitives now exist for both native-driver ABI
  (`shmem_create/map/unmap`, `console_ring_id`) and WASM syscalls
  (`wasmos_shmem_create/map/unmap`) backed by the same kernel shared-memory
  registry.
- The VT WASM service now maintains explicit per-TTY state (4 tty slots),
  supports active-tty selection (`VT_IPC_SWITCH_TTY`), and stores per-tty
  attributes (`VT_IPC_SET_ATTR_REQ`) while output remains routed through
  `wasmos_console_write` into the serial/console-ring path.
- VT tty roles are now split intentionally: `tty0` reflects the system
  serial/console-ring output, while `tty1+` are VT-managed framebuffers.
  Framebuffer control IPC now includes a console-mode toggle so VT can disable
  console-ring drain when non-zero ttys are active and restore it on `tty0`.
  tty switches clear the framebuffer before replaying the selected tty buffer,
  and switch-time clear/replay now uses a higher-reliability IPC send path so
  redraw is not skipped under transient framebuffer queue backpressure; VT now
  fails switch requests when clear/replay IPC cannot be delivered so clients do
  not receive false-positive switch success. The native framebuffer driver now
  services control IPC before draining console ring backlog so switch
  clear/replay commands are not starved by log traffic.
- CLI now receives the VT endpoint from process-manager wiring, switches to
  `tty1` at startup, and sends terminal output through `VT_IPC_WRITE_REQ`
  rather than direct console writes.
- CLI `cd` path tracking now keeps standard dot-segment semantics (`.` stays in
  place, `..` resolves to the parent) instead of collapsing both to `/`.
- Process-manager now assigns a home tty to each CLI instance (`tty1..tty3`)
  and `sysinit` ensures one CLI per VT-managed tty. CLIs gate input by current
  VT foreground selection so only the active tty shell reads keystrokes.
- VT now owns keyboard input routing end-to-end and delivers per-tty raw key
  input over `VT_IPC_READ_REQ`. CLI remains the owner of line editing/echo;
  raw mode now emits extended arrows plus nav/edit keys as ANSI escape bytes
  (`ESC[A/B/C/D`, `ESC[H/F`, `ESC[5~/6~`, `ESC[2~/3~`), CLI now consumes
  `ESC[A`/`ESC[B` for shell history navigation in raw mode, raw printable keys
  now honor Shift-modified ASCII symbols (Set-1 map), and serial console
  reads are retained as fallback for headless/automated test flows.
- VT now applies a core CSI/SGR subset per tty state (`A/B/C/D/H/f`, `s/u`,
  `J`, `K`, private `?25h/l`, `m` with 16-color mapping), so replayed tty
  buffers preserve cursor/erase/color effects across switches.
- VT write routing now uses caller endpoint ownership to target the correct tty
  buffer; non-foreground tty writes are buffered without rendering over the
  active framebuffer.
- VT now accepts `VT_IPC_SET_MODE_REQ` so clients can select per-tty input
  handling (`raw`, `canonical`, `echo`) through the same owned endpoint used
  for VT writes/reads.
- VT canonical input handling now includes baseline in-service line discipline
  controls (`Backspace`, `Ctrl+U`, `Ctrl+C`) plus per-tty history navigation
  (`Up/Down` arrows with `Ctrl+P`/`Ctrl+N` fallback), so cooked-mode consumers
  can rely on VT-side editing/interrupt delivery semantics.
- VT enforces explicit writer registration (`VT_IPC_REGISTER_WRITER`) and
  switch-generation write tokens: writes tagged with older generations are
  dropped after tty switches, and switch replay runs behind a temporary render
  barrier to avoid in-flight foreground repaint races.
- VT can emit compact switch/ownership/drop telemetry through
  `wasmos_debug_mark` into the kernel's global trace stream when
  `WASMOS_TRACE=1`, so race analysis uses existing tracing infrastructure.
- Known deferred VT issue: an intermittent framebuffer-only prompt
  duplication/misalignment artifact during rapid `Ctrl+Shift+Fn` switching was
  observed earlier. It is currently not reproducible in recent runs, so the
  issue is deferred until a stable repro path is available.
- VT keyboard hotkeys support `Ctrl+Shift+F1..F4` to switch directly to
  `tty0..tty3`.
- Keyboard event delivery into VT is now explicit fire-and-forget
  (`KBD_IPC_KEY_NOTIFY` with `request_id = 0`), and VT/CLI output transport
  loops now use bounded `IPC_ERR_FULL` retries so queue backpressure degrades
  output before it can freeze the interactive path.
- Fatal CPU exceptions still log to serial and now also render an in-kernel
  framebuffer panic screen (black background) with key crash diagnostics
  including exception registers, process identity, stack bounds, CR3/kernel
  text range, and framebuffer geometry/base.
- The CMake-only `kernel_ide` aggregation target indexes kernel sources plus
  selected WASM user-space sources, so it must mirror the libc include root
  used by those components for editor diagnostics.
- The top-level documentation now uses repo-local mascot and wordmark assets in
  `README.md`; this is documentation-only branding and does not affect boot or
  runtime behavior.

## Architectural Direction

### Microkernel Split
Kernel mechanisms:
- Boot-time platform handoff.
- Physical and virtual memory management primitives.
- Preemptive scheduling and process lifecycle control.
- IPC transport, endpoint ownership, and wakeup rules.
- Interrupt handling and timer-driven preemption.
- WASM runtime hosting plus native-driver ELF loading via WASMOS-APP hooks.

User-space policy:
- Driver startup order and long-running driver logic.
- Filesystem semantics.
- Process startup policy.
- Hardware discovery policy above the raw ACPI data scan.
- Future service registry, supervision, and namespace management.

### Privilege Model
- Today all processes still execute in ring 0 with per-process kernel data
  structures and separate runtime contexts.
- Drivers are treated as privileged by policy.
- Services are intended to become least-privileged first once ring 3 support,
  page-table separation, and syscall entry are in place.
- Applications already carry the weakest semantic role in the container format,
  even if CPU privilege separation is not implemented yet.

## Boot Contract

### Bootloader Responsibilities
Intent: small, deterministic, and policy-light.

The bootloader must:
- Locate and read `kernel.elf` from the EFI System Partition.
- Locate and read `initfs.img` from the EFI System Partition.
- Validate the ELF header and load all `PT_LOAD` segments, including
  misaligned physical addresses and overlapping segment reuse.
- Collect the UEFI memory map and copy it into kernel-owned pages before
  `ExitBootServices()`.
- Fill a versioned `boot_info_t`.
- Validate the initfs header and entry table, then copy the initfs blob into
  boot handoff memory.
- Synthesize `boot_module_t` records for bootstrap-marked initfs WASMOS apps so
  the existing early-kernel bootstrap path can stay unchanged.
- Transfer control to the kernel entry point without embedding higher-level OS
  policy.

`BOOTX64.EFI` now also captures the GOP framebuffer when available. When the
GOP handles are absent, it scans VGA PCI BARs for a framebuffer, logs the
cartographic results, and pipes the discovered base, size, resolution, stride,
and `BOOT_INFO_FLAG_GOP_PRESENT` into `boot_info_t` so lower-privilege drivers
can map the framebuffer themselves.

### Kernel Entry Responsibilities
The architecture-specific entry path must:
- Establish a known stack.
- Clear `.bss`.
- Preserve the incoming `boot_info_t *` passed in `RCX` under the Microsoft x64
  UEFI calling convention.
- Call `kmain(boot_info_t *)`.

### `boot_info_t` Rules
`boot_info_t` is append-only and versioned. New fields go at the end.

Current required fields:
- `version`, `size`, `flags`
- `memory_map`, `memory_map_size`, `memory_desc_size`, `memory_desc_version`
- `modules`, `module_count`, `module_entry_size`
- `rsdp`, `rsdp_length`
- `initfs`, `initfs_size`
- `boot_config`, `boot_config_size`

Current behavior:
- The bootloader fills the structure and the kernel validates the size/version
  before using it.
- Boot modules are still used as the early bootstrap channel, but they are now
  derived from the initfs instead of being loaded one-by-one in the bootloader.

### Initfs Layout
The bootstrap image format is intentionally small and append-only enough for
boot handoff use:
- header with magic `WMINITFS`, version, table sizing, and total image size
- fixed-size entries with type, flags, payload offset/size, and logical path
- raw payload data packed directly behind the table

Current entry types:
- WASMOS app payload
- config payload
- generic data payload

Current bootstrap use:
- bootstrap-marked WASMOS app entries are exposed as `boot_module_t` records
- the first config entry is exposed through `boot_info_t.boot_config`
- the full blob is retained so later code can add a proper initfs reader

## Boot Flow

### High-Level Sequence
1. UEFI loads `BOOTX64.EFI`.
2. The bootloader loads `kernel.elf` and `initfs.img`.
3. The bootloader exits boot services and jumps to kernel entry.
4. The kernel initializes core subsystems and spawns the kernel `init` task.
5. `init` starts `hw-discovery` from the bootstrap module set exposed by initfs.
6. `hw-discovery` starts `ata` and `fs-fat`.
7. `init` waits for FAT readiness, then loads `sysinit` from disk through the
   process manager.
8. `sysinit` reads the boot config and starts the configured late user
   processes.
9. The CLI becomes the visible interactive shell.

A minimal COM1-based serial stub keeps the console alive during the steps above.
The AssemblyScript `serial` driver now loads via `hw-discovery` and invokes
`serial_register()` so console output can switch over from the stub to the new
service as soon as the driver is available.

- `hw-discovery` merely starts the keyboard WASMOS app alongside the other
  bootstrap drivers; the AssemblyScript driver now polls the PS/2 controller for
  scancodes itself so keyboard presence remains a user-space concern instead of
  spinning kernel knowledge into the microkernel core.

### Practical Boot Ownership
- Bootloader owns UEFI interaction and boot-time file I/O.
- Kernel owns core mechanisms and early bootstrap orchestration.
- `init` owns system bootstrap sequencing once the kernel is alive.
- `sysinit` owns late user process startup policy from boot config only.

This split is intentional: it keeps bootloader policy minimal and prevents
`sysinit` from becoming a second bootstrap coordinator.

## Scheduling and Preemption

### Current Design
The scheduler is fully preemptive round-robin:
- PIT IRQ0 drives time-slice accounting.
- The default PIT rate is 250 Hz.
- The ready queue is FIFO.
- Each runnable process gets a fixed quantum
  (`PROCESS_DEFAULT_SLICE_TICKS` in `src/kernel/include/process.h`).
- An explicit idle task runs `hlt` whenever no process is ready.

### Implemented Preemption Path
The preemptive scheduling design that previously lived in a separate draft is
now the baseline architecture:
- `src/kernel/timer.c` programs PIT channel 0 and tracks global ticks.
- The IRQ0 handler increments tick accounting and triggers preemption logic.
- The kernel does not perform a full scheduler run inside the ISR.
  Instead, timer preemption rewrites the interrupted RIP to
  `process_preempt_trampoline`, which returns into the normal scheduler path.
- Per-process state includes saved register context, total tick accounting, and
  remaining time slice.
- Context switching is implemented in
  `src/kernel/arch/x86_64/context_switch.S`.
- Spinlocks disable preemption while held to keep critical regions short and
  consistent.

### Preemption Safety Rules
- Never preempt while a spinlock is held.
- Never perform heavy scheduling work directly in the interrupt handler.
- IPC queue mutations must remain atomic under preemption.
- Long host calls must mark themselves as non-preemptible if interrupting them
  would break wakeup or ownership invariants.

### Current Safe Points and Special Cases
- IPC receive host calls mark the current process as inside a host call so the
  empty-to-block transition cannot race against wakeups.
- If a blocked process is woken during that transition, the scheduler preserves
  the wakeup instead of forcing the process back to `BLOCKED`.
- The CLI calls `sched_yield` while polling for user input so other processes
  continue to make progress even when the shell is idle.

### What Is Still Missing
The preemptive core is implemented, but the following are still future work:
- Priorities or budgets.
- Per-CPU scheduling.
- User-mode context switching with kernel/user stack separation.
- Richer scheduling metrics and latency instrumentation.

## Process Model

### Process Lifecycle
The implemented process states are:
- `READY`
- `RUNNING`
- `BLOCKED`
- `ZOMBIE`

Typical transitions:
- Spawn: `READY`
- Dispatch: `READY -> RUNNING`
- Time slice expiration: `RUNNING -> READY`
- IPC wait or explicit block: `RUNNING -> BLOCKED`
- Wakeup: `BLOCKED -> READY`
- Exit: `RUNNING -> ZOMBIE`
- Reap: `ZOMBIE -> UNUSED`

### Process Ownership
- The kernel-owned `init` task is the root parent for kernel-spawned processes.
- The process manager owns the `proc` IPC endpoint and mediates spawn/wait/kill/status.
- PM-created processes get their own runtime context and stack/heap sizing from
  WASMOS-APP metadata.

### Runtime Contexts
Each process is associated with a runtime context that tracks:
- linear memory
- stack
- heap
- IPC region placeholders
- device region placeholders

This is the structural precursor to full address-space separation.

## IPC Model

### Core Message Format
All IPC messages share the same fixed register-sized layout:

```c
type
source
destination
request_id
arg0
arg1
arg2
arg3
```

Small control traffic stays in-message. Bulk payloads are expected to move to
shared buffers plus synchronization messages.

### Implemented Rules
- Endpoints have an owning context.
- `ipc_send_from` requires a non-kernel sender to own its source endpoint.
- `ipc_recv_for` requires a non-kernel receiver to own the destination endpoint.
- Enqueueing a message can wake a process blocked on the destination endpoint.
- Message queues are bounded and protected by spinlocks.
- Endpoint table capacity is currently 128 and endpoints owned by a process
  context are released when that process is reaped, preventing table exhaustion
  across repeated short-lived app runs.

### Error Model
Current IPC status codes:
- `IPC_OK`
- `IPC_EMPTY`
- `IPC_ERR_INVALID`
- `IPC_ERR_PERM`
- `IPC_ERR_FULL`

### Direction of Future Growth
The current transport is intentionally small. The architecture still needs:
- notification objects distinct from synchronous request/reply IPC
- shared-memory bulk transfer paths
- service-level allowlists / badges
- async server helpers for multi-hop service stacks
- richer endpoint naming / registry rules

## Interrupts and Timer Integration
- The kernel remaps the legacy PIC and installs exception plus IRQ stubs.
- PIT IRQ0 is the active scheduler clock.
- The timer code emits a one-time visible initialization message
  (`[timer] pit init`).
- Periodic timer tick progress markers are now trace-only and hidden when
  `WASMOS_TRACE=OFF`.

The current interrupt model is still PIC-based. APIC/IOAPIC support remains open.

## Memory Management

### Current State
Implemented:
- physical frame allocator from the UEFI memory map
- freeing of physical pages
- kernel-owned x86_64 page tables
- higher-half kernel alias mapping at `0xFFFFFFFF80000000`
- root context creation
- per-process context creation
- per-process root page tables cloned from the kernel mappings
- CR3 switching on scheduler dispatch/return
- fault-driven mapping of process-owned virtual regions into a private user slot
- guard pages around process stacks
- stack canaries for overflow diagnostics

### Current Constraints
- Shared-memory primitives are still mostly architectural intent.
- Page faults are handled through a kernel-hosted memory service scaffold rather
  than a real user-space pager.
- All tasks still execute in ring 0, so address-space separation is not yet a
  security boundary.
- Process runtime stacks still rely on shared low kernel mappings rather than a
  dedicated kernel-stack virtual range per process.

The native-driver loader maps requested physical device memory (for example, the
GOP framebuffer) into the driver process context at a fixed device virtual base
for direct native access after validation.

### Direction
The desired endpoint is:
- shared kernel higher-half mappings
- per-process user mappings with ring 3 execution
- explicit shared regions for bulk IPC
- user-space memory policy
- user-mode page-fault handling

## Runtime Hosting and WASMOS-APP Format

### Runtime Choice
The supported in-tree runtime is `wasm3`.

Current wasm3 integration guarantees:
- runtime instances are process-local
- runtime allocation uses a kernel-owned per-process chunked bump allocator
- runtime heaps grow incrementally and are capped at 2 GiB per process
- runtime create/load/call/free operations execute with preemption disabled so
  timer IRQs cannot interrupt runtime mutation

Current heap behavior:
- each process starts with a preferred heap chunk size derived from the loader
  manifest, with a practical default still centered around the old 4 MiB arena
- additional chunks are allocated on demand instead of requiring a single large
  contiguous reservation
- freeing and in-place `realloc` are still optimized for tail allocations only
- WASMOS-APP heap `max_pages` metadata is parsed but not enforced yet

### Historical WAMR Note
Earlier experiments with WAMR on a preemptive branch showed the interpreter
stalling at the glue-frame/IMPDEP handoff before reaching native imports.
Those notes were useful as a debugging record, but WAMR is not the supported
runtime in this tree and the repository policy remains: do not carry routine
instrumentation inside vendored runtime code. If alternate runtime work resumes,
compare against a non-preemptive baseline and keep debug instrumentation out of
the vendored subtree when possible.

### WASMOS-APP Container
WASMOS-APP exists to make boot and PM loading deterministic:
- fixed header
- explicit app name
- explicit entry export
- endpoint requirements
- capability requests
- memory hints
- raw payload bytes (WASM module or native ELF)

Current flag roles:
- driver
- service
- normal application
- privileged request
- native payload (valid only in combination with `driver`)

Current memory-hint behavior:
- stack `min_pages` affects runtime stack sizing
- heap `min_pages` affects the preferred initial runtime heap chunk size
- heap `max_pages` is reserved metadata for future enforcement

Current entry expectations:
- applications export `wasmos_main` through a language shim
- drivers and services export `initialize`
- native drivers use ELF `e_entry` to point at `initialize(wasmos_driver_api_t *, int, int, int)`

### Language ABI Strategy
Applications no longer need to implement the raw startup ABI directly:
- the C shim exports `wasmos_main` and calls `main(int argc, char **argv)`
- the Rust shim exports `wasmos_main` and calls `main(args: &[&str])`
- the Go shim exports `wasmos_main` and calls `Main(args []string) int32`
- the AssemblyScript toolchain-owned root module exports `wasmos_main` and
  delegates to `main(args: Array<string>): i32`
- the Zig shim exports `wasmos_main` and keeps a Zig-native `main`

This keeps the external ABI stable while presenting language-native entrypoints.

## Drivers and Services

### Implemented Drivers
- `ata`
  - PIO ATA block driver
  - owns the `block` endpoint
  - supports identify and read operations
- `fs-fat`
  - FAT12/16/32 filesystem driver
  - consumes the `block` endpoint
  - owns the `fs` endpoint
  - supports root/subdirectory listing, `cat`, `cd`, PM app loading, and the
    minimal shared libc read-only file API
  - follows FAT12/16 cluster chains for multi-cluster file reads on the
    current ESP baseline
- `chardev`
  - IPC-backed console/character device service
- `framebuffer`
  - optional native C driver packed as `FLAG_DRIVER|FLAG_NATIVE`
  - probes the kernel framebuffer APIs exposed via GOP
  - validates geometry and maps framebuffer pages into a fixed driver device
    virtual region through the native-driver API
  - paints a gradient on the standard QEMU VGA framebuffer when the device is present

### Implemented Services
- `process-manager`
  - validates WASMOS-APP containers
  - creates process/runtime state
  - resolves required endpoints
  - starts entries
  - exposes `spawn`, `spawn by name`, `wait`, `kill`, and `status`
- `hw-discovery`
  - scans ACPI RSDP data
  - starts the early storage driver chain
- `sysinit`
  - intentionally narrow
  - starts late user processes from the generated boot config
- `cli`
  - interactive shell over `proc` and `fs`

### Driver and Service Startup Chain
Current startup chain:
1. bootloader loads `initfs.img`
2. initfs contributes bootstrap `boot_module_t` entries for `hw-discovery`,
   `serial`, `keyboard`, `framebuffer`, `ata`, `fs-fat`, and the current
   smoke/bootstrap apps
3. kernel `init` spawns `hw-discovery`
4. `hw-discovery` starts the driver chain: `serial`, `keyboard`, the optional
   `framebuffer`, `ata`, and `fs-fat`
5. kernel `init` waits for a successful FAT readiness probe
6. kernel `init` loads `sysinit` from disk via PM
7. `sysinit` loads the configured `sysinit.spawn` processes from disk

This is the current stable bootstrap baseline.

### Boot Config
The initial config channel is a simple binary blob generated from TOML at build
time. The current generator reads `scripts/initfs.toml` and emits both the
initfs image and a compact `bootcfg.bin` payload.

Current config format:
- magic `WCFG0001`
- version
- bootstrap-module count
- sysinit-spawn count
- string-table size
- offset arrays for each string list
- NUL-terminated ASCII string table

Current `sysinit.spawn` validation:
- at least one late-start process must be configured
- process names must be unique
- process names must fit the current 16-byte PM by-name spawn ABI

Current use:
- the blob is carried in initfs for stable packaging
- the bootloader exposes the blob through `boot_info_t`
- wasm processes can read it through `wasmos_boot_config_size()` and
  `wasmos_boot_config_copy()`
- `sysinit` validates and consumes the `sysinit.spawn` string list for its
  late-start process policy and halts that policy path if the config is
  malformed

### What Is Still Missing
- driver-manager
- full PCI enumeration
- richer ACPI-based device inventory publication
- hotplug handling
- capability-based MMIO/PIO/DMA/IRQ grants

## CLI and User-Space Baseline
The CLI intentionally stays small and testable.

Supported commands:
- `help`
- `ps`
- `ls`
- `cat <path>`
- `cd <path>`
- `exec <app>`
- `halt`
- `reboot`

The CLI is also part of the scheduler regression story because it yields while
idle instead of monopolizing CPU time in a polling loop.

## Diagnostics and Tracing

### Visible by Default
These remain visible even with tracing disabled:
- normal boot logs
- fatal diagnostics
- runtime failure diagnostics
- process listings and CLI-visible service output

### Visible Only With `WASMOS_TRACE=ON`
- init / PM / scheduler trace lines
- `debug_mark(tag)`
- periodic timer progress markers (`[timer] ticks`)
- verbose scheduling and runtime transition traces

### Existing Debug Hooks
- `g_skip_wasm_boot` can isolate runtime bring-up in the kernel init path
- GP fault reporting includes PID, name, stack bounds, RIP, CS, and RFLAGS
- CLI tests and smoke apps provide functional regression coverage for IPC,
  preemption, filesystem access, and language shims

## Current Status by Area

### Done
- boot contract versioning and validation
- ELF loading with aligned/overlap-safe segment handling
- single-image initfs bootstrap packaging
- serial-first early boot diagnostics
- physical memory allocator
- basic paging scaffold
- process contexts and stacks from physical memory
- preemptive round-robin scheduler
- timer-driven context switching
- IPC endpoint ownership enforcement
- process manager and WASMOS-APP loader
- bootstrap split between kernel `init` and user-space `sysinit`
- FAT-backed loading of `sysinit`, `cli`, and normal applications
- generated boot-config blob exposed to user space
- shared read-only file API in userland libc
- language-native application entrypoint shims

### Partially Done
- filesystem support
  - works for current boot and small-file scenarios
  - still limited in read breadth and filesystem operations
- hardware discovery
  - enough for ACPI RSDP discovery and storage bootstrap
  - not yet a general device manager
- memory service
  - kernel-hosted scaffold exists
  - real user-space pager does not

### Not Done
- ring 3 execution
- syscall ABI
- APIC / SMP
- shared-memory IPC fast paths
- service registry
- supervision / restart policy
- capability-granted device access
- broader config-driven startup policy beyond the current `sysinit.spawn` list

Open implementation work is tracked in `TASKS.md`.

## Repository Map
- `src/boot/`: UEFI loader
- `src/kernel/`: kernel core, runtime hosting, scheduler, IPC, memory
- `src/drivers/`: WASM and native drivers
- `src/services/`: WASM services
- `lib/libc/`: shared user-space libc surface and language shims
- `examples/`: application examples and smoke apps
- `tests/`: QEMU-driven integration and regression tests

## Validation Baseline
Every architecture-affecting change is expected to keep these green:
- `cmake --build build --target run-qemu-test`
- `cmake --build build --target run-qemu-cli-test`

QEMU backend caveat:
- the CLI write smoke keeps truncate/append/create plus nested unlink/rmdir
  checks, but avoids one top-level grown-file unlink sequence that can trigger
  a known `vvfat` host assertion on some QEMU builds

The architecture is only considered stable when both the non-interactive boot
check and the CLI integration suite pass.
