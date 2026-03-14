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
- Treat drivers, services, and applications as isolated WASM programs with
  explicit IPC contracts instead of implicit in-kernel calls.
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
- The runtime host uses `wasm3`, not WAMR.

## Architectural Direction

### Microkernel Split
Kernel mechanisms:
- Boot-time platform handoff.
- Physical and virtual memory management primitives.
- Preemptive scheduling and process lifecycle control.
- IPC transport, endpoint ownership, and wakeup rules.
- Interrupt handling and timer-driven preemption.
- WASM runtime hosting and WASMOS-APP loading hooks.

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
- raw WASM payload

Current flag roles:
- driver
- service
- normal application
- privileged request

Current memory-hint behavior:
- stack `min_pages` affects runtime stack sizing
- heap `min_pages` affects the preferred initial runtime heap chunk size
- heap `max_pages` is reserved metadata for future enforcement

Current entry expectations:
- applications export `wasmos_main` through a language shim
- drivers and services export `initialize`

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
- `chardev`
  - IPC-backed console/character device service

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
   `ata`, `fs-fat`, and the current smoke/bootstrap apps
3. kernel `init` spawns `hw-discovery`
4. `hw-discovery` starts `ata` and `fs-fat`
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

Current use:
- the blob is carried in initfs for stable packaging
- the bootloader exposes the blob through `boot_info_t`
- wasm processes can read it through `wasmos_boot_config_size()` and
  `wasmos_boot_config_copy()`
- `sysinit` validates and consumes the `sysinit.spawn` string list for its
  late-start process policy

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
- `src/drivers/`: WASM drivers
- `src/services/`: WASM services
- `lib/libc/`: shared user-space libc surface and language shims
- `examples/`: application examples and smoke apps
- `tests/`: QEMU-driven integration and regression tests

## Validation Baseline
Every architecture-affecting change is expected to keep these green:
- `cmake --build build --target run-qemu-test`
- `cmake --build build --target run-qemu-cli-test`

The architecture is only considered stable when both the non-interactive boot
check and the CLI integration suite pass.
