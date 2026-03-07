# Architecture Notes

This document captures the in-depth architecture for the UEFI bootloader and kernel scaffold.
It is intended to evolve as the project grows (WAMR integration, drivers, scheduler, etc.).

IMPORTANT: Keep this file and `README.md` up to date with every prompt execution and code iteration.
IMPORTANT: Create a git commit after each prompt iteration.

## Goals
- Boot an x86_64 system via UEFI and load a freestanding ELF64 kernel.
- Provide a minimal kernel environment for a WASM runtime (WAMR) and drivers.
- Establish clear seams for memory management, device discovery, and userland loading.

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
- Allocate pages and load segments at physical addresses.
- Construct `boot_info_t` and capture the UEFI memory map.
- Log basic status messages to the UEFI console.
- Exit boot services (retrying on invalid parameter) and transfer control to kernel entry.

## Kernel Entry Responsibilities
- Establish a known stack.
- Clear BSS for C runtime expectations.
- Call `kmain(boot_info_t *)` with boot data.

## Kernel Early Init (Planned)
- Physical memory manager from UEFI memory map.
- Page table setup and higher-half mapping (optional later).
- Early console (serial or framebuffer).
- Basic CPU init (GDT/IDT, exception handlers).

## WAMR Integration (Planned)
- WAMR is vendored via git subtree at `libs/wasm/wasm-micro-runtime`.
- Port or embed WAMR as a static library under `libs/wasm/`.
- `libs/wasm/wamr_runtime.c` provides a basic wrapper over the WAMR C API.
- Provide WASI-like shims or custom syscalls for drivers.
- Define a stable ABI between kernel and WASM modules.

## Driver Model (Planned)
- Enumerate hardware via ACPI and PCI.
- Provide driver registry and resource manager.
- Expose driver APIs to WASM userland.

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
