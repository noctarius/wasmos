# Architecture Notes

This document captures the in-depth architecture for the UEFI bootloader and kernel scaffold.
It is intended to evolve as the project grows (WAMR integration, drivers, scheduler, etc.).

IMPORTANT: Keep this file and `README.md` up to date with every prompt execution and code iteration.
IMPORTANT: Create a git commit after each prompt iteration.

## Goals
- Boot an x86_64 system via UEFI and load a freestanding ELF64 kernel.
- Provide only minimal kernel primitives for a microkernel architecture.
- Establish clear seams for memory management, device discovery, and userland loading.

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
