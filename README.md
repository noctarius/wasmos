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

macOS note: Use Homebrew LLVM clang (AppleClang cannot build UEFI targets). Configure with
`-DCLANG=/opt/homebrew/opt/llvm/bin/clang` and ensure `lld-link` is available. If it's elsewhere, pass
`-DLLD_LINK=/path/to/lld-link`.

### Dependencies
- WAMR (wasm-micro-runtime) is vendored via git subtree at `libs/wasm/wasm-micro-runtime`.

### WAMR scaffold
- `libs/wasm/wamr_runtime.c` provides a thin wrapper over the WAMR C API.
- Enable with `-DWAMR_ENABLE=ON` once you wire the WAMR library into the kernel link.

### Configure
```
cmake -S . -B build
```

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
