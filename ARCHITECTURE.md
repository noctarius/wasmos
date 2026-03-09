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

UEFI protocol usage:
- UEFI Boot Services are available only before `ExitBootServices()`; they are the
  mechanism OS loaders use to access devices and allocate memory. (UEFI Spec 2.11)
- File access can be done via `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` and
  `EFI_FILE_PROTOCOL` (supports FAT12/16/32) to load configs and WASMOS-APP blobs.
- Raw block access can be done via `EFI_BLOCK_IO_PROTOCOL` for sector reads/writes.

Design options for loading init/services:
- Preferred: bootloader uses UEFI file or block protocols to load `sysinit`, PM, and
  boot config into memory and passes them to the kernel in `boot_info_t`.
- Optional: kernel stays in UEFI environment temporarily and uses Boot Services
  (requires delaying `ExitBootServices()` and passing SystemTable/handles). This
  complicates kernel init and is not the default plan.

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

Privilege model:
- Normal applications run unprivileged (user mode).
- Drivers run privileged by default to access hardware directly.
- System services should prefer unprivileged mode and request privileged operations
  via drivers or kernel mechanisms when needed.

### IPC Design Direction
These principles are derived from the IPC notes in this document.
- Request-reply semantics by default for kernel-bound calls; blocking IPC provides implicit synchronization.
- One-way notifications for events and signals, separate from request/reply IPC.
- Nonblocking IPC variants (polling send/recv) for latency-sensitive or re-entrant paths.
- Send permissions via explicit allowlists (capability-like send masks) and per-endpoint badges.
- Keep IPC messages small and register-sized where possible to enable a fast path.
- Shared-memory fast paths for bulk data with explicit buffer ownership rules and message-based synchronization.
- Unified notification path for asynchronous events to avoid deadlocks.
- Build higher-level IPC services over the core message-passing primitive.
- Support a bound-notification model so threads can receive signals while waiting on IPC.

### Memory Model Direction
- Keep kernel mechanisms small; delegate policies to services.
- Provide mapping primitives for shared memory regions and user-space paging.
- Avoid kernel-internal copying for bulk data; use shared memory plus message-based synchronization.

### Virtual Memory Plan
Goal: every process runs in a controlled virtual address space; the kernel stays mapped in the higher half.

Address space layout:
- Kernel higher-half mapping at `0xFFFFFFFF80000000` with shared kernel mappings in all contexts.
- Identity map a minimal low-memory window for early boot and trampoline paths only.
- User-space region below the kernel split with guard pages around stacks and heaps.

Core mechanisms (kernel):
- Per-context page tables with shared kernel mappings.
- Map, unmap, and protect primitives with explicit permissions.
- Frame allocation and reclamation via the physical frame allocator.
- Explicit shared-memory mapping of the same frames into multiple contexts.

Policy (user space):
- Memory service decides address placement, growth, and reclamation.
- Page-fault handler runs in user space and requests mappings as needed.

Fault handling:
- Page faults deliver a structured fault message (address, access type, context).
- Kernel blocks the faulting thread and resumes after the memory service replies.
- Current scaffold uses a kernel-hosted memory service and synchronous fault IPC
  while scheduling/preemption and per-context page tables are brought up.

### CPU Security & Isolation Features
Required CPU features for protection:
- NX (IA32_EFER.NXE) to mark non-executable memory regions.
- User/supervisor page permissions (U/S) and write protection (CR0.WP).
- Ring 3 user mode for applications and most services; ring 0 for kernel and privileged drivers.
- SMEP/SMAP if available to prevent accidental kernel access to user memory.
- Separate user and kernel stacks with guard pages.
- Syscall entry via `syscall/sysret` (or `sysenter`) with a controlled user->kernel ABI.

Isolation rules:
- Kernel mappings are shared but read-only where possible; data is non-executable.
- User address spaces are isolated per context; shared memory is explicit and bounded.
- Privileged services must be explicitly granted capabilities; default is least privilege.

### Memory Management Steps
1. Freeze the kernel/user virtual address split and document the layout.
2. Define page table ownership rules and a minimal map/unmap API.
3. Add a user-space memory service and fault IPC protocol.
4. Enable shared-memory regions with explicit lifetime and ownership rules.
5. Add optional copy-on-write as a policy in user space (kernel supports refcounted frames).

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

## Roadmap (Step by Step)
Each step includes a definition of done and a test plan to keep progress measurable.

1. Boot Contract Freeze
Scope: finalize `boot_info_t`, memory map handoff, and kernel entry contract.
Definition of Done: `boot_info_t` versioning rules documented; loader and kernel agree on layout; no breaking changes without version bump.
Tests: `cmake --build build --target bootloader` and `cmake --build build --target kernel` run; QEMU boot reaches `[kernel] kmain`.

2. IPC Core API
Scope: define IPC message schema, error codes, and permission rules.
Definition of Done: IPC spec documented; kernel IPC implementation matches spec; invalid endpoints/permissions return defined errors.
Tests: QEMU boot shows IPC smoke test (chardev roundtrip) success.

3. Notification Objects
Scope: add async notification mechanism separate from request/reply.
Definition of Done: kernel delivers notifications without blocking; services can receive notifications while waiting on IPC.
Tests: QEMU boot shows a driver or timer notification arriving while IPC waits.

4. Shared Memory IPC
Scope: shared memory mapping and synchronization for bulk payloads.
Definition of Done: shared buffer lifetime rules documented; at least one driver/service uses shared memory for bulk transfer.
Tests: QEMU boot runs a bulk transfer test that exercises shared memory path.

5. Memory Service + Fault IPC
Scope: user-space memory service and page-fault protocol.
Definition of Done: faults are delivered to memory service; mappings are installed on demand; kernel remains policy-free.
Tests: QEMU boot runs a process that triggers page faults and continues successfully.
Status: implemented with a kernel-hosted memory service and a pagefault-test process.

6. Process Manager
Scope: WASMOS-APP loading, WAMR context creation, process lifecycle management.
Definition of Done: PM loads a WASMOS-APP, resolves endpoints, starts entry export; lifecycle APIs (`spawn`, `wait`, `kill`) work.
Tests: QEMU boot loads a WASMOS-APP via PM and exits cleanly with status.
Status: implemented with a kernel init process that spawns a process manager service owning the `proc` endpoint, then requests it to load the `sysinit` WASMOS-APP boot module and supports IPC `spawn`, `wait`, `kill`, and `status`.

7. Init + Service Startup
Scope: sysinit reads config from EFI disk, starts PM, drivers, FAT32, CLI.
Definition of Done: sysinit loads config, spawns core services in order, registers names.
Tests: QEMU boot shows sysinit-driven startup and CLI prompt.
Status: user-space `sysinit` WASMOS-APP spawns `chardev-client` via the `proc` endpoint; config/FS-driven startup is still pending.

8. Storage Stack
Scope: virtio, ATA, SATA block driver + FAT32 filesystem service.
Definition of Done: FAT32 mounts EFI disk via block driver; files can be opened and read via IPC.
Tests: QEMU boot loads a config file from FAT32 and prints a known line.

9. Hardware Discovery + Driver Manager
Scope: `hw-discovery` publishes device inventory; `driver-manager` assigns drivers.
Definition of Done: devices appear in discovery events; driver-manager starts and wires drivers based on events.
Tests: QEMU boot shows discovery events and driver-manager spawn log.

10. Privilege Separation + CPU Hardening
Scope: ring-3 transition, NX, SMEP/SMAP where available.
Definition of Done: user processes run unprivileged; kernel rejects forbidden memory access; NX enforced.
Tests: QEMU boot runs a user app that attempts a prohibited access and receives a controlled fault.

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
- `src/boot/` UEFI application (PE/COFF) that loads `kernel.elf` from the ESP.
- `src/kernel/` Freestanding kernel (C + ASM).
- `libs/wasm/` Placeholder integration point for WAMR.
- `examples/` Example WASM applications used for driver/server/client bring-up (grouped by language).
- `src/drivers/` WASM driver sources and ABI headers. Each driver lives in its own subdirectory.
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
| (src/boot/boot.c)|
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
| (src/kernel/arch/x86_64)   |
+--------+---------------+
         |
         | Stack + BSS
         v
+------------------------+
| kmain                  |
| (src/kernel/kernel.c)      |
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

## Boot Contract (Freeze Item 1)
Definition:
- `boot_info_t` is versioned and append-only. New fields are added at the end.
- The bootloader and kernel must agree on `boot_info_t` size and version.

Required fields:
- `version` (uint32): boot info version, incremented on incompatible changes.
- `size` (uint32): total size of the `boot_info_t` structure.
- `flags` (uint32): feature flags (e.g., GOP present, modules present).
- Memory map fields (current scaffold): `memory_map`, `memory_map_size`, `memory_desc_size`, `memory_desc_version`.

Optional future fields:
- Framebuffer/GOP descriptor.
- Boot module list (for preloaded WASMOS-APP images).
- RSDP/ACPI pointers (implemented).

Definition of Done:
- `boot_info_t` layout and versioning rules documented here.
- Bootloader populates `version`, `size`, `flags` and memory map fields.
- Kernel validates `version` and `size` before use.

Current scaffold status:
- `boot_info_t` now includes module list fields (`modules`, `module_count`, `module_entry_size`).
- `boot_info_t` now includes the ACPI RSDP pointer and length for early discovery.
- Bootloader optionally preloads `\apps\chardev_client.wasmosapp` from ESP and
  passes it as `BOOT_MODULE_TYPE_WASMOS_APP`.
- Kernel consumes boot modules when launching the test WASMOS-APP process.

Test:
- QEMU boot reaches `[kernel] kmain` and prints detected `boot_info_t` version/size.

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
- WAMR initialization uses a kernel-owned static pool allocator (currently 4 MiB) with per-context bindings for linear/stack/heap sizing.
- WAMR runtime initialization is performed on-demand by the kernel wasm driver host when the first wasm driver is started.
- WAMR is enabled by default and links the runtime library unless `WAMR_LINK=OFF` is set.
- The WAMR runtime build uses a minimal `wasmos` platform from `src/wasm-micro-runtime/platform/wasmos/`.
- The custom WAMR object build path forwards upstream runtime feature defines and disables `-Werror` for third-party WAMR sources.
- Freestanding builds set `WAMR_DISABLE_APP_ENTRY=1` and link the generated `libwamr_runtime.a` into the kernel.
- The `wasmos` platform adapter includes WAMR shared math sources and minimal libc/fortify shims required by freestanding linkage.
- The `wasmos` platform adapter now provides runtime-backed `os_mmap`/`os_mremap` allocation behavior required for interpreter linear-memory mapping during module instantiation.

## IPC Model
- IPC is the default communication mechanism between WASM drivers, services, and applications.
- The kernel mediates IPC primitives and endpoint isolation; higher-level protocols are implemented in WASM services.
- Device-facing drivers expose capability-like IPC interfaces rather than direct shared driver calls.

### IPC Message Format
```
type        // message type or opcode
source      // source endpoint ID
destination // destination endpoint ID
request_id  // client-chosen correlation ID
arg0..arg3  // payload words (use shared memory for bulk data)
```

### IPC Message Types (Kernel Services)
- `IPC_MEM_FAULT` (0x1000): page-fault request.
  - `arg0..arg1` = fault address (low/high)
  - `arg2` = page fault error code
  - `arg3` = faulting context ID
- `IPC_MEM_FAULT_REPLY` (0x1001): page-fault reply.
  - `arg0` = status (`0` ok, negative on failure)
  - `arg1..arg2` = mapped base (low/high)

### Notification Objects
- Notifications are separate endpoints that carry a count (no payload).
- `notify` increments the count and wakes the owning context.
- `wait` decrements the count or returns `IPC_EMPTY` if none pending.
- Only the kernel or the owning context may send notifications (initial rule).

### IPC Error Codes
- `IPC_OK` = 0
- `IPC_EMPTY` = 1 (no message available)
- `IPC_ERR_INVALID` = -1 (bad endpoint/args)
- `IPC_ERR_PERM` = -2 (permission denied)
- `IPC_ERR_FULL` = -3 (queue full / no free endpoint)

### IPC Permission Rules
- Sender must own the `source` endpoint (unless the sender is the kernel).
- Receiver must own the destination endpoint (unless the receiver is the kernel).
- Endpoint ownership is bound to a memory context; permissions are enforced in the kernel.

### IRQ Handling and Delegation
- IRQs are handled in the kernel and delegated to drivers via notification endpoints.
- The kernel installs IRQ stubs for vectors `32..47` and remaps the legacy PIC.
- Each IRQ line can be registered to a driver-owned notification endpoint.
- The ISR performs minimal work: sends a notification and issues PIC EOI.
- Drivers consume IRQ events by waiting on their notification endpoint.
- The current scaffold uses the legacy PIC; APIC/IOAPIC support is a later step.

### Shared Memory IPC (Initial Mechanism)
- Shared regions are created in kernel memory and mapped into multiple contexts.
- `mm_shared_create(pages, flags)` returns a shared region ID + base.
- `mm_shared_map(ctx, id)` adds the region to a context; `mm_shared_unmap` detaches.
- Bulk IPC uses shared memory for data and a small IPC message for synchronization.

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
- `spinlock` primitive in `src/kernel/spinlock.c` provides low-level mutual exclusion for shared kernel objects.
- `ipc` primitive in `src/kernel/ipc.c` provides endpoint allocation and bounded per-endpoint message queues.
- IPC messages carry basic routing and payload fields (`type`, `source`, `destination`, `request_id`, `arg0..arg3`).
- Endpoints are associated with owner context IDs and protect queues via spinlocks.
- IPC permissions are enforced by context-aware operations:
- `ipc_send_from` requires non-kernel senders to use a source endpoint owned by their context.
- `ipc_recv_for` requires non-kernel receivers to own the destination endpoint context.
- IPC enqueue wakes blocked processes that own the destination endpoint context.
- `process` primitive in `src/kernel/process.c` provides a small cooperative process table and scheduler.
- `timer` primitive in `src/kernel/timer.c` programs PIT IRQ0 and tracks a tick counter; logging is deferred to the scheduler loop.
- `process_spawn` binds each process to a new memory context (`mm_context_create(pid)`), establishing per-process isolation boundaries.
- Lifecycle primitives now include `process_wait`, `process_kill`, and `process_get_exit_status`.
- WAMR native IPC imports use the `exec_env` calling convention to align with WAMR native argument marshalling.

## Process Model (Current Scaffold)
- The scheduler is cooperative and tick-based (`process_schedule_once`), scanning for READY processes in round-robin order.
- Process entries return run results (`YIELDED`, `IDLE`, `BLOCKED`, `EXITED`) to drive state transitions.
- `PROCESS_RUN_BLOCKED` entries remain blocked until a kernel primitive wakes them; IPC uses `process_wake_by_context` on message enqueue.
- Exited processes transition to a zombie state carrying `exit_status` until reaped by `process_wait`.
- The kernel main loop schedules processes instead of invoking service handlers directly.
- A kernel IPC wakeup smoke test spawns `ipc-wait-test` and `ipc-send-test` and logs `[test] ipc wake ok` on success.
- The current system starts a dedicated `chardev-server` process and assigns its context ID as the owner of the chardev IPC endpoint.
- The current system starts a kernel `init` process that is the root parent for all kernel-spawned processes, and it spawns the `process-manager` (owner of the `proc` endpoint).
- The process manager spawns the user-space `sysinit` WASMOS-APP boot module after a kernel-init request and passes the `proc` endpoint plus boot module metadata.
- The user-space `sysinit` module spawns remaining boot modules via `proc`, and the chardev client uses imported IPC primitives to issue write/read requests.
- A minimal PIO ATA block driver runs as a WASMOS-APP service (`src/drivers/ata`), and the process manager assigns it the `block` IPC endpoint.
- A FAT12/16/32 filesystem driver runs as a WASMOS-APP service, uses the `block` IPC endpoint for sector reads, and exposes the `fs` IPC endpoint (root + single-subdir `ls`/`cat`/`cd` with VFAT LFN support).
- A minimal user-space `cli` WASMOS-APP is loaded as a boot module, reads serial input, and supports `help`, `ps`, `ls`, and `cat` via small native helpers.
- The chardev server returns `BLOCKED` when no IPC message is pending, reducing scheduler churn while idle.

## WAMR Integration (Current Scaffold)
- WAMR is vendored via git subtree at `libs/wasm/wasm-micro-runtime`.
- WAMR is built as a static library and linked into the kernel.
- `libs/wasm/wamr_runtime.c` provides a basic wrapper over the WAMR C API.
- A stable ABI is defined via `src/drivers/include/wasmos_driver_abi.h` and the WASMOS-APP container format.

Planned:
- Provide WASI-like shims or richer custom syscalls for drivers.

## Driver Model (Current Scaffold)
- Run drivers as isolated WASM contexts.
- Expose driver APIs through IPC endpoints to other WASM contexts.
- Current scaffold includes project-owned wasm application examples under `examples/` (C, AssemblyScript, Rust, Go, and Zig).
- The build compiles the chardev server driver (`src/drivers/chardev`) and chardev client example into `.wasm` binaries and embeds them into the kernel image as binary blobs.
- Driver wasm binaries are linked with explicit stack and initial/max memory bounds to keep freestanding instantiation deterministic.
- A kernel wasm driver host (`src/kernel/wasm_driver.c`) loads embedded modules, instantiates them with WAMR, allocates IPC endpoints, and dispatches IPC messages to a driver export.
- The chardev service (`src/kernel/wasm_chardev.c`) runs in the spawned `chardev-server` process and bridges IPC request/response traffic to the wasm export `chardev_ipc_dispatch`.
- The wasm chardev module also exports `chardev_init` and optional direct byte helpers (`chardev_read_byte`, `chardev_write_byte`).
- The wasm chardev test-client module exports `chardev_client_step` and consumes imported IPC APIs (`ipc_create_endpoint`, `ipc_send`, `ipc_recv`, `ipc_last_field`) from native module `wasmos`.

Planned:
- Enumerate hardware via ACPI and PCI.
- Provide a driver registry and resource manager.

## Driver Framework (Planned)
Drivers are privileged by default but remain isolated WASM processes with explicit
hardware access grants.

Hardware access model:
- MMIO: drivers request mapping of device BARs into their address space.
- Port I/O: drivers request controlled access to specific I/O ports.
- DMA: drivers request DMA-capable memory regions and IOMMU mappings if available.
- IRQs: drivers register for interrupts and receive async notifications via IPC.

Access control:
- The kernel exposes minimal mechanisms: map/unmap, port access, IRQ registration.
- A resource manager (or sysinit/PM in early boot) grants capabilities to drivers.
- System services request privileged operations via driver IPC, not by direct hardware access.

Driver IPC shape:
- Each driver exports a stable endpoint (e.g., `block`, `net`, `gpio`).
- Requests are small control messages; bulk data uses shared memory plus notification.

Early boot:
- `sysinit` (or a dedicated resource manager) enumerates devices (ACPI/PCI) and assigns
  BARs/IRQs to the appropriate driver processes.

Runtime device management:
- `hw-discovery` publishes device inventory and hotplug events.
- `driver-manager` owns driver lifecycle, matches devices to drivers, and requests
  resource grants for MMIO/PIO/DMA/IRQ.

## Drivers & Services (Current Scaffold)
Drivers:
- `ata` (PIO ATA block): exposes block read and identify via the `block` IPC endpoint.
- `fs-fat` (FAT12/16/32): mounts a block device and exposes `fs` root list/cat.
- `chardev` (IPC chardev): exposes byte read/write via chardev IPC messages.

Services:
- `process-manager` (PM): spawns processes, tracks lifecycle, owns PID namespace.
  - Current scaffold: init spawns PM as a kernel process, which owns the `proc` endpoint and spawns the `sysinit` WASMOS-APP boot module.
  - Reads WASMOS-APP containers, validates headers and tables.
  - Copies WASM payload into managed memory and tracks lifetime.
  - Creates the process context (memory regions, IPC endpoints, permissions).
  - Instantiates WAMR context and binds linear/stack/heap sizes from hints.
  - Resolves required IPC endpoints/capabilities before start.
  - Starts the entry export and registers the process with the scheduler.
- `sysinit` (root task / init process): the first user-space task that bootstraps the system.
  - Starts core services (PM, hw-discovery).
- `hw-discovery` (udev-like): scans the ACPI RSDP and starts core drivers (ATA, FAT) from boot modules.
- `sysinit` launches the CLI after `fs-fat` is running.
- `cli` (simple CLI): provides a basic command loop and service/status queries.

Planned:
Drivers:
- `disk-virtio` (virtio block): exposes block read/write and identify; privileged by default.
- `disk-sata` (AHCI/SATA): exposes block read/write and identify; privileged by default.
- `fs-fat32` (FAT32): mounts a block device, exposes file open/read/dir listing.

Services:
- `hw-discovery` extensions: enumerate ACPI/PCI device inventory and emit hotplug events.
- `driver-manager`: starts, stops, and assigns drivers in response to hardware events.
  - Maps devices to driver images, allocates resources, and wires endpoints.
- `sysinit` extensions:
  - Reads boot configuration from the EFI boot disk via FAT32.
  - Performs minimal namespace/service registration (names, endpoints).
  - Acts as a minimal loader for PM if the kernel does not spawn PM directly.
    - Option A: PM is embedded as a WASMOS-APP blob and sysinit parses/loads it.
    - Option B: PM is a simpler boot module and sysinit passes it to PM for re-load.

IPC expectations (high level):
- Disk drivers provide a `block` endpoint: `read(sector,count,reply_ep)` and `write(sector,count,reply_ep)`.
- FAT32 consumes a `block` endpoint and exports `fs` endpoints for file and directory operations.
- Process manager exports `proc` endpoint: `spawn`, `wait`, `kill`, `status`.
- Init uses `fs` endpoints to read config and uses `proc` to spawn services.
- CLI uses `proc` and `fs` for simple shell commands.
- `hw-discovery` emits `device` events to `driver-manager`.
- `driver-manager` requests resource grants and spawns drivers via `proc`.

### Minimal CLI (Current Scaffold)
The CLI is intentionally tiny and runs in user space on top of `fs` and `proc`.

Required commands:
- `ls` — list directory contents via `fs`.
- `cat <path>` — print file contents.
- `ps` — list processes via `proc`.
- `help` — list built-in commands.
- `cd <path>` — change working directory (root or a single subdirectory).
- `exec <app>` — load a WASMOS-APP from disk and ask `proc` to spawn it (applications only; drivers/services are rejected).
- `halt` — request system shutdown via ACPI poweroff.
- `reboot` — request system reboot via ACPI reset.

Planned:
- `echo <args...>` — print arguments to console.

### Init Process Responsibilities (Microkernel Practice)
Common patterns in microkernel systems:
- The initial user task/root task bootstraps the user-space system and launches core services.
- It often serves as a resource/bootstrap coordinator (memory/IRQ/task IDs or namespaces).
- It reads a configuration or startup script to decide which services to start.

WASMOS adaptation:
- `sysinit` receives the initial boot resources and is responsible for delegating them
  (capabilities/handles) to the PM and other core services.
- `sysinit` starts PM first, then disk drivers, FAT32, and CLI.
- `sysinit` loads configuration from EFI disk (FAT32) to determine service set and order.
- `sysinit` registers basic names/endpoints for service discovery.
- `sysinit` may host a minimal loader for PM if PM is not started by the kernel.
- If using a script-driven startup (analogous to `/etc/rc`), sysinit remains minimal
    and only spawns what the config demands.

## Interfaces
### boot_info_t
Defined in `src/kernel/include/boot.h` and populated by the bootloader.

Fields (current scaffold):
- `memory_map`, `memory_map_size`, `memory_desc_size`, `memory_desc_version`
- `modules`, `module_count`, `module_entry_size`
- `rsdp`, `rsdp_length` (ACPI RSDP pointer and length)
- framebuffer fields (placeholders for future integration)

### WASMOS WASM Application Format (WASMOS-APP)
Purpose: a minimal container for WASM modules with explicit metadata for boot-time
loading, IPC wiring, and resource sizing. The container is a simple header followed by
optional tables and the raw WASM bytes.

Design goals:
- Keep parsing trivial (fixed-size header + linear tables).
- Make metadata explicit (name, exports, IPC endpoints, memory sizing).
- Preserve the raw WASM object code verbatim.

Binary layout (little-endian):
```
struct wasmos_app_header {
    char     magic[8];      // "WASMOSAP"
    uint16_t version;       // format version (start at 1)
    uint16_t header_size;   // bytes from start to end of fixed header
    uint32_t flags;         // bitfield (see below)
    uint32_t name_len;      // bytes, UTF-8, not NUL-terminated
    uint32_t entry_len;     // bytes, UTF-8 export name for entry
    uint32_t wasm_size;     // bytes of WASM payload
    uint32_t req_ep_count;  // number of required IPC endpoints
    uint32_t cap_count;     // number of capability requests
    uint32_t mem_hint_count;// number of memory hints
    uint32_t reserved;      // must be zero
    // followed by variable data sections in order:
    // name bytes
    // entry bytes
    // required endpoints table
    // capability requests table
    // memory hints table
    // wasm bytes
};
```

Flags:
- `WASMOS_APP_FLAG_DRIVER` (bit 0): module is a driver (privileged by default).
- `WASMOS_APP_FLAG_SERVICE` (bit 1): module is a system service.
- `WASMOS_APP_FLAG_APP` (bit 2): module is a normal application.
- `WASMOS_APP_FLAG_NEEDS_PRIV` (bit 3): module requests privileged mode.

Required endpoints table:
```
struct wasmos_req_endpoint {
    uint32_t name_len;      // bytes of endpoint name
    uint32_t rights;        // bitmask (send/recv/notify)
    // name bytes (UTF-8)
};
```

Capability requests table:
```
struct wasmos_cap_request {
    uint32_t name_len;      // bytes of capability name
    uint32_t flags;         // policy-defined
    // name bytes (UTF-8)
};
```

Memory hints table:
```
struct wasmos_mem_hint {
    uint32_t kind;          // linear/stack/heap/ipc/device
    uint32_t min_pages;     // minimum pages
    uint32_t max_pages;     // maximum pages (0 = unspecified)
};
```

Notes:
- The loader validates sizes and bounds; tables are optional if counts are zero.
- The `entry` name maps to the WASM export to call on start.
- IPC endpoint names resolve through the service registry; rights are enforced by
  kernel IPC permissions.

Current implementation (kernel scaffold):
- `src/kernel/wasmos_app.c` provides the initial WASMOS-APP parser/loader.
- Loader validates header/version/magic, all section bounds, and extracts wasm bytes.
- Memory hints are parsed; stack/heap min-page hints are mapped to WAMR stack/heap sizes.
- Required endpoints and capability tables are enforced during `wasmos_app_start` via kernel policy hooks.
- Startup fails if endpoint resolution or capability grant fails for any required entry.
- The test client path now consumes a boot-preloaded WASMOS-APP module and starts it through the loader API.

## Build & Run Notes
- `cmake -S . -B build`
- `cmake --build build --target bootloader` -> `build/BOOTX64.EFI`
- `cmake --build build --target kernel` -> `build/kernel.elf`
- `cmake --build build --target make_wasmos_app` -> `build/make_wasmos_app` (packer for `.wasmosapp`)
- `cmake --build build --target run-qemu` creates a FAT ESP for testing (requires OVMF) and uses a serial console (`-nographic`).
- `run-qemu`, `run-qemu-test`, and `run-qemu-cli-test` copy `sysinit.wasmosapp` and `cli.wasmosapp` into `esp/system/services` in addition to `esp/apps`.
- `run-qemu`, `run-qemu-test`, and `run-qemu-cli-test` copy driver WASMOS-APPs (`ata.wasmosapp`, `fs_fat.wasmosapp`) into `esp/system/drivers`, and the bootloader preloads them from that path.
- `run-qemu-cli-test` uses the Python QEMU test framework to execute CLI commands and assert output while the VM is running via `python3 -m unittest discover -s tests`.
- CLI integration tests include per-app hello tests (`test_hello_*.py`).
- CMake is split across per-component `CMakeLists.txt` files (boot, kernel, drivers, services, and each example language).
- CLI integration tests include executing `hello-zig` and asserting its banner output before returning to the prompt.
- IDE indexing targets (`bootloader_ide`, `kernel_ide`) include all project C/H sources for CLion, including drivers/services/examples and WAMR platform stubs.
  - IDE targets also export include directories for kernel/drivers/WAMR headers to improve CLion resolution.
- Zig WASMOS-APP builds force-export the entry symbol (the build passes `--export=hello_zig_step`) so the entry function is retained.
If OVMF isn't found, pass `-DOVMF_CODE=/path/to/OVMF_CODE.fd` at configure time.
macOS/Homebrew: `brew install edk2-ovmf`.
When running QEMU, we use `if=pflash` with OVMF code/vars; set `-DOVMF_VARS=/path/to/OVMF_VARS.fd` if available.

macOS note: Use Homebrew LLVM clang (AppleClang cannot build UEFI targets). Ensure `lld-link` is available.
If auto-discovery fails, pass `-DCLANG=/path/to/llvm/bin/clang` and `-DLLD_LINK=/path/to/lld-link` or `-DLLD=/path/to/lld`.

## Future Diagrams
- Memory layout (UEFI, kernel image, stack, heap)
- Page table setup
- Driver call graph and resource flow
