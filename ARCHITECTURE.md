# Architecture Notes

This document captures the in-depth architecture for the UEFI bootloader and kernel scaffold.
It is intended to evolve as the project grows (WAMR integration, drivers, scheduler, etc.).

IMPORTANT: Keep this file and `README.md` up to date with every prompt execution and code iteration.
IMPORTANT: Create a git commit after each prompt iteration.

## Goals
- Boot an x86_64 system via UEFI and load a freestanding ELF64 kernel.
- Provide only minimal kernel primitives for a microkernel architecture.
- Establish clear seams for memory management, device discovery, and userland loading.

## Bootloader & Kernel Architecture (Rework)
This section restates the target architecture using the best-practice notes captured below. It is the baseline to follow before changing code.

### Bootloader Design
Intent: small, deterministic, and strict about inputs.

Responsibilities:
- Validate and load `kernel.elf` PT_LOAD segments with explicit alignment handling.
- Construct `boot_info_t` with memory map and platform descriptors.
- Preserve a clean handoff contract to `_start` without extra policy.
- Provide minimal diagnostics to UEFI console only.

Rules:
- Do not embed policy (no allocation heuristics beyond ELF requirements).
- Avoid dynamic dependencies; keep the loader deterministic and auditable.
- Preserve forward compatibility by versioning `boot_info_t` and extending only at the end.

### Kernel Design (Microkernel First)
Intent: minimal kernel mechanisms, all policy in user space (WASM services).

Kernel responsibilities:
- IPC transport and endpoint isolation.
- Basic scheduling and process lifecycle.
- Memory management mechanisms (page tables, region mapping, allocation primitives).
- Interrupt and timer dispatch to user space via nonblocking notifications.

User-space responsibilities:
- Drivers, filesystems, network stack, and services.
- Policy layers (scheduling policy, resource management, naming, service discovery).

### IPC Design Direction
These principles are derived from the IPC notes in this document.
- Request-reply semantics by default for kernel-bound calls.
- One-way notifications for events and signals, separate from request/reply IPC.
- Nonblocking IPC variants (polling send/recv) for latency-sensitive or re-entrant paths.
- Send permissions via explicit allowlists (capability-like send masks) and per-endpoint badges.
- Keep IPC messages small and register-sized where possible to enable a fast path.
- Shared-memory fast paths for bulk data with explicit buffer ownership rules and message-based synchronization.
- Unified notification path for asynchronous events to avoid deadlocks.
- Build higher-level IPC services over the core message-passing primitive.

### Memory Model Direction
- Keep kernel mechanisms small; delegate policies to services.
- Provide mapping primitives for shared memory regions and user-space paging.
- Avoid kernel-internal copying for bulk data; use shared memory plus message-based synchronization.

### Scheduling Direction
- Keep a small core scheduler in kernel with pluggable policy hints.
- Allow user-space services to set priorities and budgets via explicit calls.
- Ensure IPC paths do not block kernel tasks.

### Stepwise Plan
1. Freeze the `boot_info_t` contract and document versioning rules.
2. Specify IPC message formats, error codes, and permission rules.
3. Add separate notification objects and nonblocking IPC semantics at the API level.
4. Define shared-memory IPC protocol, buffer ownership rules, and synchronization points.
5. Add a service registry and discovery protocol in user space.
6. Introduce a supervisor service for crash recovery and restart policies.
7. Extend scheduling with priorities and IPC-aware budgeting.
8. Add tracing hooks for IPC latency and queue pressure.

## Kernel Architecture Guide
This guide captures microkernel-informed design decisions and a stepwise plan for WASMOS. It is based on the general monolithic vs. microkernel overview in `monolithic-and-microkernel.txt`.

### Design Decisions We Adopt
- Keep the kernel minimal: IPC, basic memory management, scheduling, and foundational I/O only.
- Run drivers and OS services as user-level WASM processes to isolate faults.
- Use IPC as the primary interaction mechanism between services and applications.
- Preserve modularity: add/remove services without kernel recompilation or reboot.
- Favor isolation and least privilege with context-bound endpoints and capability-like IPC handles.
- Prioritize maintainability via stable, versioned IPC protocols and clear ownership boundaries.
- Optimize IPC early to avoid performance regressions from context switches and data copying.
- Keep platform-specific code constrained to boot/arch layers to preserve portability.

### Trade-offs to Track
- IPC overhead and context switching cost can dominate performance if not tuned.
- User-level service design increases system complexity; requires robust supervision.

### Steps Forward (Bit by Bit)
1. Define and freeze the kernel/user boundary: document IPC message formats, error codes, and endpoint ownership rules.
2. Implement IPC fast paths: reduce copies, add reply matching, and define backpressure behavior for queue depth.
3. Add shared-memory IPC for bulk payloads with explicit buffer ownership and lifecycle rules.
4. Introduce a service supervisor: detect crashes, restart policy, and health checks for critical services.
5. Build a service registry and discovery mechanism with stable names and capability distribution.
6. Flesh out memory-context policy: user virtual layout, region permissions, and page-fault handling.
7. Extend scheduling: priorities, service budgets, and latency caps for IPC-heavy services.
8. Add observability: IPC tracing hooks, per-endpoint stats, and structured kernel logs.
9. Harden security: audit IPC permission checks, fuzz message parsing, and lock down privileged paths.

## Microkernel Model
- The kernel only provides minimal primitives (boot, memory, isolation boundaries, IPC transport, and runtime hosting hooks).
- Drivers, OS services, and applications are implemented as WASM programs instead of in-kernel subsystems.
- Each WASM program runs in its own WAMR context bound to its own memory regions.
- Cross-context interaction is explicit and IPC-based; direct shared mutable state between contexts is not the default model.

## Repository Layout
- `boot/efi/` UEFI application (PE/COFF) that loads `kernel.elf` from the ESP.
- `kernel/` Freestanding kernel (C + ASM).
- `libs/wasm/` Placeholder integration point for WAMR.
- `examples/wasm/` Example WASM applications used for driver/server/client bring-up.
- `scripts/` Optional helper scripts.

## Boot Flow (High Level)
```
UEFI firmware
  -> BOOTX64.EFI (UEFI app)
     -> parse/load kernel.elf (PT_LOAD)
     -> collect memory map
     -> ExitBootServices
     -> jump to kernel entry
        -> setup stack, clear BSS
        -> kmain(boot_info)
        -> init subsystems (memory, drivers)
        -> init WAMR
```

## Boot Flow (Detailed)
```
+-----------------+
| UEFI Firmware   |
+--------+--------+
         |
         v
+-----------------+
| BOOTX64.EFI     |
| (boot/efi/boot.c)|
+--------+--------+
         |
         | Open ESP volume
         | Read kernel.elf
         | Validate ELF header
         | Load PT_LOAD segments
         v
+------------------------+
| Memory map + boot_info |
+--------+---------------+
         |
         | ExitBootServices
         v
+------------------------+
| Kernel entry (_start)  |
| (kernel/arch/x86_64)   |
+--------+---------------+
         |
         | Stack + BSS
         v
+------------------------+
| kmain                  |
| (kernel/kernel.c)      |
+--------+---------------+
         |
         | Memory manager
         | Drivers
         | WAMR runtime
         v
+------------------------+
| Userland (WASM)        |
+------------------------+
```

## Bootloader Responsibilities
- Locate `kernel.elf` in the ESP root.
- Parse ELF64 header and PT_LOAD program headers.
- Allocate page-aligned regions and load segments at physical addresses (handling misaligned paddr and overlaps).
- Construct `boot_info_t` and capture the UEFI memory map.
- Copy the memory map into kernel-owned pages before exiting boot services.
- Log basic status messages to the UEFI console.
- Exit boot services (retrying on invalid parameter) and transfer control to kernel entry.

## Kernel Entry Responsibilities
- Establish a known stack.
- Clear BSS for C runtime expectations.
- Preserve incoming `boot_info_t *` (UEFI uses MS ABI; pointer arrives in `RCX`) and call `kmain(boot_info_t *)`.

## Kernel Early Init
Fixed:
- Physical memory manager from UEFI memory map.
- Page table setup with a kernel-owned root table and higher-half alias mapping.
- Basic CPU init with GDT/IDT installation and early exception handlers.
- Early console via COM1 serial.

Remaining:
- Framebuffer console path (serial is implemented; framebuffer is still pending).

## Memory Management Scaffold
- Microkernel model: every driver/service/app runs in its own WAMR context.
- Each context owns a bounded set of memory regions (linear memory, IPC, device, stack, heap, code).
- Current scaffold tracks per-context regions and defers real allocation to later.
- A simple physical frame allocator scans the UEFI memory map and tracks usable ranges.
- The frame allocator now supports freeing pages and backing context region allocation.
- `mm_init` provisions a root context with basic linear/stack/heap regions plus placeholder IPC/device regions.
- `mm_init` now installs kernel-owned x86_64 page tables and reloads `CR3`.
- The paging scaffold keeps low-memory identity mapping and adds higher-half aliases at `0xFFFFFFFF80000000`.
- `mm_context_create` can allocate new contexts with default linear/stack/heap regions.
- WAMR initialization uses a kernel-owned static pool allocator (currently 2 MiB) with per-context bindings for linear/stack/heap sizing.
- WAMR runtime initialization is performed on-demand by the kernel wasm driver host when the first wasm driver is started.
- WAMR is enabled by default and links the runtime library unless `WAMR_LINK=OFF` is set.
- The WAMR runtime build uses a minimal `wasmos` platform from `platform/wasmos/`.
- The custom WAMR object build path forwards upstream runtime feature defines and disables `-Werror` for third-party WAMR sources.
- Freestanding builds set `WAMR_DISABLE_APP_ENTRY=1` and link the generated `libwamr_runtime.a` into the kernel.
- The `wasmos` platform adapter includes WAMR shared math sources and minimal libc/fortify shims required by freestanding linkage.
- The `wasmos` platform adapter now provides runtime-backed `os_mmap`/`os_mremap` allocation behavior required for interpreter linear-memory mapping during module instantiation.

## IPC Model
- IPC is the default communication mechanism between WASM drivers, services, and applications.
- The kernel mediates IPC primitives and endpoint isolation; higher-level protocols are implemented in WASM services.
- Device-facing drivers expose capability-like IPC interfaces rather than direct shared driver calls.

## IPC Best Practices (Herder Thesis Notes)
Source: `herder_thesis.pdf` (MINIX 3 microkernel conversion).

Design takeaways:
- Favor structural deadlock prevention over detection. Enforce a directed message ordering with replies only in the reverse direction.
- Avoid blocking kernel tasks. Require request-reply (`sendrec`) style interactions for kernel-bound calls.
- Provide nonblocking IPC variants so services can poll when blocking is unsafe.
- Validate IPC calls strictly: known call numbers, valid endpoints, and explicit error returns.
- Enforce communication permissions via per-process send masks (policy/mechanism separation).
- Handle asynchronous events via a unified notification construct that never blocks the caller and defers delivery until the receiver is ready.
- Minimize IPC overhead by reducing context switches and data copying; small kernels help cache behavior.

IPC improvement notes for WASMOS:
- Add a directed IPC policy for service layers with explicit reply-only bottom-up flows.
- Introduce nonblocking send/recv APIs and propagate `ENOTREADY`-style errors to callers.
- Add per-process IPC allowlists (send masks) beyond endpoint ownership checks.
- Add a notification path for asynchronous kernel-to-service events that is nonblocking and deferrable.
- Tighten IPC validation and error codes for illegal calls, invalid endpoints, and dead destinations.

## IPC Best Practices (Aigner PhD Notes)
Source: `aigner_phd.pdf` (microkernel communication and stub generation).

Design takeaways:
- Treat IPC performance as a primary system concern: marshaling/unmarshaling overhead is significant even when kernel IPC is fast.
- Optimize for two dominant cases: small messages (latency, setup/parse costs) and large transfers (throughput, copy avoidance).
- Use in-place message buffers when possible to avoid extra copies.
- Support one-way messages for notifications/synchronization alongside RPC-style request/response.
- Prefer asynchronous server patterns when a server depends on lower-level services: store request state, issue async call, resume on reply.
- Keep communication code resource-bounded (no unbounded heap/stack usage in low-level paths) to avoid DoS and latency spikes.
- Use shared memory for bulk payloads, but establish long-lived regions; avoid per-call map/unmap costs.
- Treat shared-memory policy as user-space policy: kernel provides only mechanisms to establish/revoke regions.

IPC improvement notes for WASMOS:
- Define distinct fast paths for small control messages vs. bulk data (shared memory + notification).
- Add optional one-way notification messages to avoid forced request/response coupling.
- Introduce async server helpers for multi-tier services (store state, reply-only continuation).
- Ensure IPC APIs can be used without dynamic allocation in hot paths.

## IPC Best Practices (seL4 and QNX Notes)
Sources: seL4 documentation and QNX Neutrino microkernel documentation.

Design takeaways:
- Separate synchronous IPC endpoints from notification objects for asynchronous events.
- Provide optional nonblocking receive/polling to decouple event handling from blocking IPC.
- Use endpoint badges to identify the sender without exposing mutable shared state.
- Keep IPC messages small for a fast path; avoid extra capabilities in the critical path.
- Use message passing as the foundation for higher-level services; other IPC builds on it.
- Pair shared memory with message passing for bulk data movement and synchronization.

## Kernel Primitives (Current Scaffold)
- `spinlock` primitive in `kernel/spinlock.c` provides low-level mutual exclusion for shared kernel objects.
- `ipc` primitive in `kernel/ipc.c` provides endpoint allocation and bounded per-endpoint message queues.
- IPC messages carry basic routing and payload fields (`type`, `source`, `destination`, `request_id`, `arg0..arg3`).
- Endpoints are associated with owner context IDs and protect queues via spinlocks.
- IPC permissions are enforced by context-aware operations:
- `ipc_send_from` requires non-kernel senders to use a source endpoint owned by their context.
- `ipc_recv_for` requires non-kernel receivers to own the destination endpoint context.
- IPC enqueue wakes blocked processes that own the destination endpoint context.
- `process` primitive in `kernel/process.c` provides a small cooperative process table and scheduler.
- `process_spawn` binds each process to a new memory context (`mm_context_create(pid)`), establishing per-process isolation boundaries.
- Lifecycle primitives now include `process_wait`, `process_kill`, and `process_get_exit_status`.
- WAMR native IPC imports use the `exec_env` calling convention to align with WAMR native argument marshalling.

## Process Model (Current Scaffold)
- The scheduler is cooperative and tick-based (`process_schedule_once`), scanning for READY processes in round-robin order.
- Process entries return run results (`YIELDED`, `IDLE`, `BLOCKED`, `EXITED`) to drive state transitions.
- `PROCESS_RUN_BLOCKED` entries remain blocked until a kernel primitive wakes them; IPC uses `process_wake_by_context` on message enqueue.
- Exited processes transition to a zombie state carrying `exit_status` until reaped by `process_wait`.
- The kernel main loop schedules processes instead of invoking service handlers directly.
- The current system starts a dedicated `chardev-server` process and assigns its context ID as the owner of the chardev IPC endpoint.
- The current system also starts a `chardev-test-client-wasm` process that runs a wasm module and uses imported IPC primitives to create a reply endpoint and issue write/read requests.
- The chardev server returns `BLOCKED` when no IPC message is pending, reducing scheduler churn while idle.

## WAMR Integration (Planned)
- WAMR is vendored via git subtree at `libs/wasm/wasm-micro-runtime`.
- Port or embed WAMR as a static library under `libs/wasm/`.
- `libs/wasm/wamr_runtime.c` provides a basic wrapper over the WAMR C API.
- Provide WASI-like shims or custom syscalls for drivers.
- Define a stable ABI between kernel and WASM modules.

## Driver Model (Planned)
- Enumerate hardware via ACPI and PCI.
- Provide driver registry and resource manager.
- Run drivers as isolated WASM contexts.
- Expose driver APIs through IPC endpoints to other WASM contexts.
- Current scaffold includes project-owned wasm application examples under `examples/wasm/`.
- The build compiles the chardev server and chardev client examples into `.wasm` binaries and embeds them into the kernel image as binary blobs.
- Driver wasm binaries are linked with explicit stack and initial/max memory bounds to keep freestanding instantiation deterministic.
- A kernel wasm driver host (`kernel/wasm_driver.c`) loads embedded modules, instantiates them with WAMR, allocates IPC endpoints, and dispatches IPC messages to a driver export.
- The chardev service (`kernel/wasm_chardev.c`) runs in the spawned `chardev-server` process and bridges IPC request/response traffic to the wasm export `chardev_ipc_dispatch`.
- The wasm chardev module also exports `chardev_init` and optional direct byte helpers (`chardev_read_byte`, `chardev_write_byte`).
- The wasm chardev test-client module exports `chardev_client_step` and consumes imported IPC APIs (`ipc_create_endpoint`, `ipc_send`, `ipc_recv`, `ipc_last_field`) from native module `wasmos`.

## Interfaces
### boot_info_t
Defined in `kernel/include/boot.h` and populated by the bootloader.

Fields (current scaffold):
- `memory_map`, `memory_map_size`, `memory_desc_size`, `memory_desc_version`
- framebuffer fields (placeholders for future integration)

## Build & Run Notes
- `cmake -S . -B build`
- `cmake --build build --target bootloader` -> `build/BOOTX64.EFI`
- `cmake --build build --target kernel` -> `build/kernel.elf`
- `cmake --build build --target run-qemu` creates a FAT ESP for testing (requires OVMF) and uses a serial console (`-nographic`).
If OVMF isn't found, pass `-DOVMF_CODE=/path/to/OVMF_CODE.fd` at configure time.
macOS/Homebrew: `brew install edk2-ovmf`.
When running QEMU, we use `if=pflash` with OVMF code/vars; set `-DOVMF_VARS=/path/to/OVMF_VARS.fd` if available.

macOS note: Use Homebrew LLVM clang (AppleClang cannot build UEFI targets). Ensure `lld-link` is available.
If auto-discovery fails, pass `-DCLANG=/path/to/llvm/bin/clang` and `-DLLD_LINK=/path/to/lld-link` or `-DLLD=/path/to/lld`.

## Future Diagrams
- Memory layout (UEFI, kernel image, stack, heap)
- Page table setup
- Driver call graph and resource flow
