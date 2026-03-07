# WASMOS

Minimal x86_64 UEFI boot + C/ASM kernel scaffold intended to host a WASM runtime (WAMR) and hardware drivers.

IMPORTANT: Keep this file and `ARCHITECTURE.md` up to date with every prompt execution and code iteration.
IMPORTANT: Create a git commit after each prompt iteration.

## Layout
- `boot/efi/` UEFI application (PE/COFF) that loads `kernel.elf` and jumps to its entry.
- `kernel/` Freestanding kernel (C + ASM) with a tiny boot-time runtime.
- `libs/wasm/` Placeholder for integrating WAMR.
- `scripts/` Helper scripts (optional).
- `ARCHITECTURE.md` In-depth architecture notes and boot process diagrams.

## Build (scaffold)
This repository is intentionally minimal and may require toolchain adjustments for your environment.

### Toolchain
- `clang` + `lld` (COFF/UEFI support)
- `llvm-objcopy`
- `cmake` (3.20+)

### Dependencies
- WAMR (wasm-micro-runtime) is vendored via git subtree at `libs/wasm/wasm-micro-runtime`.

### Configure
```\ncmake -S . -B build\n```

### Targets
- `cmake --build build --target bootloader` builds `build/BOOTX64.EFI`
- `cmake --build build --target kernel` builds `build/kernel.elf`
- `cmake --build build --target run-qemu` runs QEMU with an ESP image

### Next steps
1. Verify the UEFI toolchain flags for your host.
2. Integrate WAMR into `libs/wasm/` and wire into `kernel/kernel.c`.
3. Implement hardware drivers and a basic scheduler.

## Notes
- The bootloader loads `kernel.elf` from the EFI System Partition (ESP).
- The kernel entry receives a `boot_info_t` with framebuffer/memory map placeholders.
