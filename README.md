# WASMOS

Minimal x86_64 UEFI boot + C/ASM kernel scaffold intended to host a WASM runtime (WAMR) and hardware drivers.

IMPORTANT: Keep this file and `ARCHITECTURE.md` up to date with every prompt execution and code iteration.
IMPORTANT: Create a git commit after each prompt iteration.

## Layout
- `boot/efi/` UEFI application (PE/COFF) that loads `kernel.elf` and jumps to its entry.
- `kernel/` Freestanding kernel (C + ASM) with a tiny boot-time runtime.
- `libs/wasm/` Placeholder for integrating WAMR.
- `examples/wasm/` Example WASM applications (driver/server/client samples).
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
- WASMOS follows a microkernel direction: the kernel keeps only minimal primitives, while drivers/services/apps are WASM programs.
- Each WASM program is expected to run in an isolated WAMR context with its own memory regions.
- Inter-component communication is IPC-based.
- The bootloader loads `kernel.elf` from the EFI System Partition (ESP).
- The Kernel Architecture Guide in `ARCHITECTURE.md` outlines microkernel design decisions and the stepwise roadmap.
- The Bootloader & Kernel Architecture rework section in `ARCHITECTURE.md` is the current design baseline.
- IPC guidance now incorporates seL4/QNX-style separation of synchronous IPC and asynchronous notifications.
- The virtual memory plan (address space layout, page tables, fault handling) is defined in `ARCHITECTURE.md`.
- The privilege model (apps unprivileged, drivers privileged, services least-privileged) is defined in `ARCHITECTURE.md`.
- CPU security and isolation features (NX, SMEP/SMAP, ring separation) are defined in `ARCHITECTURE.md`.
- The WASMOS WASM application container format (WASMOS-APP) is defined in `ARCHITECTURE.md`.
- The planned driver/service baseline (virtio + SATA + FAT32, PM, init, CLI) is defined in `ARCHITECTURE.md`.
- Init process responsibilities (root task bootstrap, config-driven startup) are outlined in `ARCHITECTURE.md`.
- The driver framework (MMIO/PIO/DMA/IRQ access via capabilities) is defined in `ARCHITECTURE.md`.
- Hardware discovery (`hw-discovery`) and driver lifecycle management (`driver-manager`) are defined in `ARCHITECTURE.md`.
- The step-by-step roadmap with definition of done and tests is defined in `ARCHITECTURE.md`.
- UEFI protocol usage and bootloader loading options are documented in `ARCHITECTURE.md`.
- The minimal CLI command set is documented in `ARCHITECTURE.md`.
- The boot contract freeze (versioned `boot_info_t`) is documented in `ARCHITECTURE.md`.
- `boot_info_t` now includes version/size/flags and is validated in the kernel.
- IPC message format, error codes, and permission rules are documented in `ARCHITECTURE.md`.
- IPC notification endpoints and nonblocking semantics are documented in `ARCHITECTURE.md`.
- Shared memory IPC primitives are documented in `ARCHITECTURE.md`.
- Memory service + page-fault IPC (kernel-hosted scaffold) and pagefault-test are implemented and documented in `ARCHITECTURE.md`.
- IRQ handling and notification-based delegation (PIC remap, IRQ stubs, IRQ routing) are implemented and documented in `ARCHITECTURE.md`.
- WASMOS-APP loading scaffold is implemented (`kernel/wasmos_app.c`); the bootloader now preloads `esp/apps/chardev_client.wasmosapp` and passes it via boot modules.
- WASMOS-APP required endpoints and capability requests are now enforced during app start via kernel policy hooks.
- The bootloader logs basic status messages to the UEFI console and retries `ExitBootServices` on invalid parameters.
- The bootloader copies the UEFI memory map into kernel-owned pages before exiting boot services.
- PT_LOAD segments are loaded with page-aligned allocations (misaligned physical addresses are handled).
- Overlapping PT_LOAD segments reuse existing allocations instead of re-allocating pages.
- The kernel entry receives a `boot_info_t` with framebuffer/memory map placeholders.
- The kernel entry preserves the incoming `boot_info_t *` (UEFI passes it in `RCX`) through early init.
- The kernel emits early serial output on COM1 (QEMU `-serial`).
- Basic CPU init now installs a minimal kernel GDT/IDT and exception stubs for vectors `0..31`.
- Memory management scaffolding tracks per-WAMR-context memory regions (microkernel model).
- A minimal physical frame allocator ingests the UEFI memory map (conventional + boot services memory).
- The physical frame allocator supports freeing pages, and contexts can allocate regions from it.
- `mm_init` scaffolds a root context with basic linear/stack/heap regions.
- `mm_init` now sets up kernel-owned page tables, reloads `CR3`, and installs a higher-half alias mapping at `0xFFFFFFFF80000000`.
- The root context also reserves placeholder IPC and device regions.
- `mm_context_create` allocates a new context and default regions.
- WAMR runtime init uses a fixed page pool and `wamr_context_bind` ties a context's regions to WAMR sizing.
- WAMR runtime initialization is on-demand through the kernel wasm driver host layer and currently uses a kernel-owned static pool (2 MiB).
- WAMR is enabled by default and links the WAMR runtime library unless `-DWAMR_LINK=OFF` is set.
- `WAMR_LINK` builds the WAMR runtime with a minimal `wasmos` platform in `platform/wasmos/`.
- WAMR custom object builds propagate upstream runtime feature defines and compile third-party sources with `-Wno-error`.
- `WAMR_DISABLE_APP_ENTRY=1` is set for the freestanding kernel profile.
- The `wasmos` platform adapter includes WAMR's shared math implementation and provides freestanding libc/fortify shims (e.g. `__memcpy_chk`, `__memset_chk`).
- The `wasmos` platform adapter now backs WAMR memory APIs (`os_mmap`/`os_mremap` and related allocators) with runtime allocator calls so module instantiation can map linear memory in freestanding mode.
- Kernel primitives now include a minimal spinlock (`kernel/spinlock.c`) and IPC transport with per-endpoint queues (`kernel/ipc.c`).
- Kernel primitives now include basic cooperative process management (`kernel/process.c`) with per-process memory-context binding.
- Process lifecycle primitives now include `wait`, `kill`, and tracked `exit_status` via zombie processes until reaped.
- Blocked processes can now be resumed by context (`process_wake_by_context`) when IPC traffic arrives for owned endpoints.
- A minimal init process runs in the kernel, spawns the process manager, and then blocks.
- The process manager owns a `proc` IPC endpoint and can `spawn`, `wait`, `kill`, and `status` processes on behalf of callers.
- The process manager spawns a user-space `init` WASMOS-APP boot module, passing the `proc` endpoint and boot module metadata.
- The user-space `init` module iterates boot modules (excluding itself) and spawns them via `proc`.
- A minimal PIO ATA block driver runs as a kernel process, exposes a `block` IPC endpoint, and supports identify/read requests.
- A FAT12/16/32 service runs as a kernel process on top of the block endpoint and logs the detected FAT variant on boot.
- A minimal user-space `cli` WASMOS-APP is loaded as a boot module, reads input from serial, and supports `help`, `ps`, `ls`, and `cat` via small native helpers.
- IPC endpoint permissions are enforced by context-aware APIs (`ipc_send_from`, `ipc_recv_for`) for source-endpoint ownership and endpoint receive ownership.
- The kernel now builds and embeds example WASM applications from `examples/wasm/` (including `chardev_server` and `chardev_client`).
- Driver wasm link settings currently constrain module stack/linear memory for low-footprint instantiation in the freestanding runtime pool.
- A generic kernel wasm driver host (`kernel/wasm_driver.c`) loads embedded modules, instantiates them via WAMR, and dispatches IPC requests to exported driver handlers.
- The WASM-backed chardev runs as an IPC service endpoint in a dedicated `chardev-server` process (`kernel/wasm_chardev.c`) using `examples/wasm/chardev_server/chardev_server.c`.
- Boot modules now include `init.wasmosapp` and `chardev_client.wasmosapp`; the chardev client performs one IPC write/read roundtrip via imported IPC primitives.
- The chardev server process blocks when its IPC queue is empty and is woken by incoming IPC messages.
- The chardev service path uses permission-aware IPC send/receive calls tied to its owner context.
- The chardev module export contract is `chardev_init` and `chardev_ipc_dispatch` (with optional direct `chardev_read_byte`/`chardev_write_byte` exports).
- Chardev IPC protocol uses request/response message types for byte read/write (`WASM_CHARDEV_IPC_*` in `drivers/wasm/include/wasmos_driver_abi.h`).
- WAMR native IPC imports now follow the WAMR `exec_env` calling convention for correct argument marshalling.
- IPC best-practice notes and improvement targets (from Herder’s MINIX thesis and Aigner’s microkernel communication work) are tracked in `ARCHITECTURE.md`.
