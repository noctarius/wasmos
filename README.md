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

macOS note: Use Homebrew LLVM clang (AppleClang cannot build UEFI targets). CMake will try to auto-discover
`clang` and `lld/lld-link` in common Homebrew locations. If that fails, pass
`-DCLANG=/path/to/llvm/bin/clang` and optionally `-DLLD_LINK=/path/to/lld-link` or `-DLLD=/path/to/lld`.

macOS install (Homebrew):
```
brew install llvm lld qemu
```

Note: Homebrew does not provide an `edk2-ovmf` formula. QEMU often ships the firmware at
`/opt/homebrew/share/qemu/edk2-x86_64-code.fd` (or `/usr/local/share/qemu/...` on Intel Macs).
If it isn't present, download OVMF from edk2 and pass `-DOVMF_CODE=/path/to/OVMF_CODE.fd`.

### Dependencies
- WAMR (wasm-micro-runtime) is vendored via git subtree at `libs/wasm/wasm-micro-runtime`.

### WAMR scaffold
- `libs/wasm/wamr_runtime.c` provides a thin wrapper over the WAMR C API.
- Enable with `-DWAMR_ENABLE=ON` once you wire the WAMR library into the kernel link.

### Configure
```
cmake -S . -B build
```

If OVMF is not found on your system, set:
```
cmake -S . -B build -DOVMF_CODE=/path/to/OVMF_CODE.fd
```
For QEMU on macOS, we use `-drive if=pflash` with OVMF code/vars. If `OVMF_VARS.fd` is available,
set `-DOVMF_VARS=/path/to/OVMF_VARS.fd` for persistent variables.
Note: Homebrew QEMU may not ship an x86_64 vars file; if none is present, omit `OVMF_VARS`.

The `run-qemu` target copies `scripts/startup.nsh` into the ESP to auto-run `BOOTX64.EFI`.
On macOS with Homebrew, install OVMF via `brew install edk2-ovmf`.

### Targets
- `cmake --build build --target bootloader` builds `build/BOOTX64.EFI`
- `cmake --build build --target kernel` builds `build/kernel.elf`
- `cmake --build build --target run-qemu` runs QEMU with an ESP image (serial console via `-nographic`)

### Next steps
1. Verify the UEFI toolchain flags for your host.
2. Integrate WAMR into `libs/wasm/` and wire into `kernel/kernel.c`.
3. Implement hardware drivers and a basic scheduler.

## Notes
- The bootloader loads `kernel.elf` from the EFI System Partition (ESP).
- The bootloader logs basic status messages to the UEFI console and retries `ExitBootServices` on invalid parameters.
- The bootloader copies the UEFI memory map into kernel-owned pages before exiting boot services.
- PT_LOAD segments are loaded with page-aligned allocations (misaligned physical addresses are handled).
- Overlapping PT_LOAD segments reuse existing allocations instead of re-allocating pages.
- The kernel entry receives a `boot_info_t` with framebuffer/memory map placeholders.
- The kernel entry preserves the incoming `boot_info_t *` (UEFI passes it in `RCX`) through early init.
- The kernel emits early serial output on COM1 (QEMU `-serial`).
- Memory management scaffolding tracks per-WAMR-context memory regions (microkernel model).
- A minimal physical frame allocator ingests the UEFI memory map (conventional + boot services memory).
- The physical frame allocator supports freeing pages, and contexts can allocate regions from it.
