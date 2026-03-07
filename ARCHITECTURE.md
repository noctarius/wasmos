# Architecture Notes

This document captures the in-depth architecture for the UEFI bootloader and kernel scaffold.
It is intended to evolve as the project grows (WAMR integration, drivers, scheduler, etc.).

IMPORTANT: Keep this file and `README.md` up to date with every prompt execution and code iteration.
IMPORTANT: Create a git commit after each prompt iteration.

## Goals
- Boot an x86_64 system via UEFI and load a freestanding ELF64 kernel.
- Provide only minimal kernel primitives for a microkernel architecture.
- Establish clear seams for memory management, device discovery, and userland loading.

## Microkernel Model
- The kernel only provides minimal primitives (boot, memory, isolation boundaries, IPC transport, and runtime hosting hooks).
- Drivers, OS services, and applications are implemented as WASM programs instead of in-kernel subsystems.
- Each WASM program runs in its own WAMR context bound to its own memory regions.
- Cross-context interaction is explicit and IPC-based; direct shared mutable state between contexts is not the default model.

## Repository Layout
- `boot/efi/` UEFI application (PE/COFF) that loads `kernel.elf` from the ESP.
- `kernel/` Freestanding kernel (C + ASM).
- `libs/wasm/` Placeholder integration point for WAMR.
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

## Kernel Early Init (Planned)
- Physical memory manager from UEFI memory map.
- Page table setup and higher-half mapping (optional later).
- Early console (serial or framebuffer). Current scaffold uses COM1 serial.
- Basic CPU init (GDT/IDT, exception handlers).

## Memory Management Scaffold
- Microkernel model: every driver/service/app runs in its own WAMR context.
- Each context owns a bounded set of memory regions (linear memory, IPC, device, stack, heap, code).
- Current scaffold tracks per-context regions and defers real allocation to later.
- A simple physical frame allocator scans the UEFI memory map and tracks usable ranges.
- The frame allocator now supports freeing pages and backing context region allocation.
- `mm_init` provisions a root context with basic linear/stack/heap regions plus placeholder IPC/device regions.
- `mm_context_create` can allocate new contexts with default linear/stack/heap regions.
- WAMR initialization currently uses a fixed pool allocator and per-context bindings for linear/stack/heap sizing.
- WAMR is enabled by default and links the runtime library unless `WAMR_LINK=OFF` is set.
- The WAMR runtime build uses a minimal `wasmos` platform from `platform/wasmos/`.
- The custom WAMR object build path forwards upstream runtime feature defines and disables `-Werror` for third-party WAMR sources.
- Freestanding builds set `WAMR_DISABLE_APP_ENTRY=1` and link the generated `libwamr_runtime.a` into the kernel.
- The `wasmos` platform adapter includes WAMR shared math sources and minimal libc/fortify shims required by freestanding linkage.

## IPC Model
- IPC is the default communication mechanism between WASM drivers, services, and applications.
- The kernel mediates IPC primitives and endpoint isolation; higher-level protocols are implemented in WASM services.
- Device-facing drivers expose capability-like IPC interfaces rather than direct shared driver calls.

## Kernel Primitives (Current Scaffold)
- `spinlock` primitive in `kernel/spinlock.c` provides low-level mutual exclusion for shared kernel objects.
- `ipc` primitive in `kernel/ipc.c` provides endpoint allocation and bounded per-endpoint message queues.
- IPC messages carry basic routing and payload fields (`type`, `source`, `destination`, `request_id`, `arg0..arg3`).
- Endpoints are associated with owner context IDs and protect queues via spinlocks.
- `process` primitive in `kernel/process.c` provides a small cooperative process table and scheduler.
- `process_spawn` binds each process to a new memory context (`mm_context_create(pid)`), establishing per-process isolation boundaries.

## Process Model (Current Scaffold)
- The scheduler is cooperative and tick-based (`process_schedule_once`), scanning for READY processes in round-robin order.
- Process entries return run results (`YIELDED`, `IDLE`, `BLOCKED`, `EXITED`) to drive state transitions.
- The kernel main loop schedules processes instead of invoking service handlers directly.
- The current system starts a dedicated `chardev-server` process and assigns its context ID as the owner of the chardev IPC endpoint.

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
- Current scaffold includes a minimal WASM-backed character device service (`kernel/wasm_chardev.c`).
- The service exposes an IPC endpoint and handles read/write requests with request/response semantics.
- Internally, the service dispatches byte I/O to exported WASM functions (`chardev_read_byte`, `chardev_write_byte`) and serializes runtime access with a spinlock.
- The service is executed by the spawned `chardev-server` process through the cooperative scheduler.

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
