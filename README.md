# WASMOS

Minimal x86_64 UEFI boot + C/ASM kernel scaffold intended to host a WASM runtime (wasm3) and hardware drivers.

IMPORTANT: Keep this file and `ARCHITECTURE.md` up to date with every prompt execution and code iteration.
IMPORTANT: Create a git commit after each prompt iteration.

## Layout
- `src/boot/` UEFI application (PE/COFF) that loads `kernel.elf` and jumps to its entry.
- `src/kernel/` Freestanding kernel (C + ASM) with a tiny boot-time runtime.
- `lib/libc/` Minimal user-space libc, shared C-side WASMOS wrapper headers, and per-language shims shared by WASMOS applications, drivers, and services.
- `libs/wasm/` WASM runtime sources (currently wasm3).
- `examples/c/` Example C WASM applications.
- `examples/go/` Example Go (TinyGo) WASM applications.
- `examples/rust/` Example Rust WASM applications.
- `examples/zig/` Example Zig WASM applications.
- `src/drivers/` WASM driver sources and ABI headers (each driver lives in its own subdirectory).
- `src/services/` System services (WASM-based).
- WASMOS-APPs export `main` for applications, while drivers and services export `initialize`.
- Embedded WASM drivers (for example the chardev server) expose an `initialize` entry for setup.
- `scripts/` Helper scripts (optional).
- `scripts/wasm_inspect.py` inspects `.wasm` or `.wasmosapp` files to list imports/exports and basic section counts.
- `ARCHITECTURE.md` In-depth architecture notes and boot process diagrams.
- CMake is split across per-component `CMakeLists.txt` files (boot, kernel, drivers, services, and each example language).

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
- wasm3 is vendored via git subtree at `libs/wasm/wasm3`.
- This branch uses a per-process bump allocator for `malloc/calloc/realloc` in `src/kernel/wasm3_shim.c` and disables the wasm3 fixed heap.
- Kernel-side wasm3 runtime create/load/call/free paths execute with preemption disabled so timer IRQs do not interrupt runtime mutation.
- `lib/libc` now provides a shared userland C surface for common helpers such as `strlen`, `strcmp`, `strncmp`, `tolower`, `toupper`, `putsn`, `printf`, `snprintf`, `abs`, `atoi`, and `strtol`, alongside reusable WASMOS host wrapper headers in `lib/libc/include/wasmos/`.
- The shared `printf` layer is intentionally minimal: `%s`, `%c`, `%d`/`%i`, `%u`, `%x`/`%X`, `%p`, `%%`, basic width/zero padding, and `l` length modifiers are supported; floating-point formatting is not.
- `lib/libc` now also exposes minimal read-only file handling through `open`, `read`, `close`, `fopen`, `fread`, `fgets`, `fgetc`, and `fclose`, using the shared process-manager FS buffer and the `fs-fat` service under the hood.
- The current shared file API is intentionally narrow: read-only access only, no seek/write/stat yet, and `fs-fat` still limits file reads to files that fit within the first FAT cluster.

### AssemblyScript (optional)
AssemblyScript can be used to write WASMOS drivers, services, and applications. Install AssemblyScript via npm and ensure `asc` is available in your PATH (for example via a global install or `npm link`):
```
npm install -g assemblyscript
```

AssemblyScript builds are enabled by default (`AS_ENABLE=ON`). If `asc` is missing, configure will fail.
To disable AssemblyScript builds:
```
cmake -S . -B build -DAS_ENABLE=OFF
```

Build the sample AssemblyScript WASMOS-APP:
```
cmake --build build --target assemblyscript_examples
```

The sample uses `asc` with release/size settings and the `stub` runtime (no GC).
The AssemblyScript shim in `lib/libc/assemblyscript/wasmos.ts` now exposes AssemblyScript-facing `std` and `fs` wrappers so AssemblyScript modules can use shared libc-style behavior without binding directly to the raw WASMOS C-shaped import surface.

There is also a minimal C-based example at `examples/c/hello/hello_c.c`, packed as `hello_c.wasmosapp`.
`examples/c/chardev_preempt/chardev_preempt.c` is a small preemption stress test for the embedded chardev server. Run it from the CLI with `exec chardev-preempt`.
`examples/c/init_smoke/init_smoke.c` is a tiny init-entry smoke test; run it from the CLI with `exec init-smoke`.
`examples/c/native_call_smoke/native_call_smoke.c` is a tiny smoke test that directly calls `console_write`; run it from the CLI with `exec native-call-smoke`.
`examples/c/native_call_min/native_call_min.c` is the minimal native-call probe (debug_mark + console_write); run it from the CLI with `exec native-call-min`.
`examples/c/fs_open_smoke/fs_open_smoke.c` exercises the shared libc file wrappers by opening and reading `/startup.nsh`; run it from the CLI with `exec fs-open-smoke`.

### Rust (optional)
Rust can be used to write WASMOS drivers, services, and applications. Install Rust and the WebAssembly target:
```
rustup target add wasm32-unknown-unknown
```

Rust builds are enabled by default (`RUST_ENABLE=ON`). If `rustc` is missing, configure will fail.
To disable Rust builds:
```
cmake -S . -B build -DRUST_ENABLE=OFF
```

Build the sample Rust WASMOS-APP:
```
cmake --build build --target rust_examples
```

The sample lives at `examples/rust/hello/hello_rust.rs` and is packed as `hello_rust.wasmosapp`.
The Rust shim in `lib/libc/rust/wasmos.rs` now exposes Rust-facing `std` and `fs` wrappers so Rust modules can use shared libc-style behavior without binding directly to the raw WASMOS C-shaped import surface.

### Go (TinyGo) (optional)
Go can be used to write WASMOS applications via TinyGo. Install TinyGo and ensure it is in your PATH.

Build the sample Go WASMOS-APP:
```
cmake --build build --target go_examples
```

The sample lives at `examples/go/hello/hello_go.go` and is packed as `hello_go.wasmosapp`.
Note: TinyGo exports a small `wasmos_entry` wrapper that calls `main` to satisfy both the Go runtime and the WASMOS entry contract.
The Go shim in `lib/libc/go/wasmos.go` now exposes Go-facing `std` and `fs` wrappers so TinyGo modules can use shared libc-style behavior without binding directly to the raw WASMOS C-shaped import surface.

### Zig (optional)
Zig can be used to write WASMOS applications. Install Zig and ensure it is in your PATH.

Zig builds are enabled by default (`ZIG_ENABLE=ON`). If `zig` is missing, Zig examples are skipped with a warning.
To disable Zig builds:
```
cmake -S . -B build -DZIG_ENABLE=OFF
```

Build the sample Zig WASMOS-APP:
```
cmake --build build --target zig_examples
```

The sample lives at `examples/zig/hello/hello_zig.zig` and is packed as `hello_zig.wasmosapp`.
Note: Zig requires an explicit wasm export for the WASMOS entry; the build adds
`--export=wasmos_entry` so the module retains the entry function.
The Zig shim in `lib/libc/zig/wasmos.zig` now exposes Zig-facing `stdlib` and `fs` wrappers so Zig modules can use shared libc-style functionality without binding directly to the raw WASMOS C-shaped import surface.

### Scheduler
The kernel uses a round-robin scheduler with a fixed time slice per process
(see `PROCESS_DEFAULT_SLICE_TICKS` in `src/kernel/include/process.h`).
User space can call the `sched_yield` wasm native to explicitly yield during busy loops.
IPC receive host calls now mark the current process as non-preemptible until the receive path either returns or fully blocks, which closes a lost-wakeup race for long-lived IPC servers.
The CLI yields while polling for console input so spawned apps can continue making progress while the prompt is idle.

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
If an i386 vars file is detected while `OVMF_CODE` is x86_64, CMake ignores `OVMF_VARS`
to avoid QEMU boot hangs.

The `run-qemu` target copies `scripts/startup.nsh` into the ESP to auto-run `BOOTX64.EFI`.
On macOS with Homebrew, install OVMF via `brew install edk2-ovmf`.

### Targets
- `cmake --build build --target bootloader` builds `build/BOOTX64.EFI`
- `cmake --build build --target kernel` builds `build/kernel.elf`
- `cmake --build build --target make_wasmos_app` builds the WASMOS-APP packer used by all `.wasmosapp` outputs
- `cmake --build build --target run-qemu` runs QEMU with an ESP image (serial console via `-nographic`)
- `cmake --build build --target run-qemu-debug` runs QEMU paused for GDB on port `1234` (override with `-DQEMU_GDB_PORT=1234`)
- `cmake --build build --target run-qemu-test` runs QEMU, waits for the CLI prompt, issues `halt`, and expects a clean shutdown
- `cmake --build build --target run-qemu-cli-test` runs the CLI integration tests via the Python QEMU test framework (`python3 -m unittest discover -s tests`)
- The QEMU test framework force-stops hung runs via the monitor sequence (`Ctrl+A` then `x`) when a timeout is reached.
- CLI integration tests include per-app hello tests (`test_hello_*.py`).
- CLI integration tests include `tests/test_fs_open_smoke.py` to cover the shared libc file-open/read/close path.
- QEMU smoke tests include a PIT timer tick marker check (`tests/test_timer_tick.py`).
- QEMU smoke tests include an IPC wakeup marker check (`tests/test_ipc_wakeup.py`).
- QEMU smoke tests include a preemption marker check (`tests/test_preempt_smoke.py`).
- CLI integration tests include `exec chardev-preempt` to exercise the embedded chardev IPC path under scheduler activity.
- The CLI tests include running `exec hello-zig` and asserting the Zig app prints its banner and returns to the prompt.
- `cmake --build build --target zig_examples` builds the Zig hello WASMOS-APP when Zig is available
- `run-qemu`, `run-qemu-test`, and `run-qemu-cli-test` copy `sysinit.wasmosapp`, `cli.wasmosapp`, and `hw_discovery.wasmosapp` into `esp/system/services` in addition to `esp/apps` (where applicable).
- `ata.wasmosapp` and `fs_fat.wasmosapp` are now copied into `esp/system/drivers` for the bootloader to preload as drivers.

Use `run-qemu-test` as the default compile+boot+halt check after code changes. Use `run-qemu-cli-test` for scripted CLI assertions (e.g. `ls` output).

### Next steps
1. Verify the UEFI toolchain flags for your host.
2. Implement hardware drivers and a basic scheduler.

## Notes
- WASMOS follows a microkernel direction: the kernel keeps only minimal primitives, while drivers/services/apps are WASM programs.
- Each WASM program is expected to run in an isolated runtime context with its own memory regions.
- Inter-component communication is IPC-based.
- Userland C code should prefer the shared `lib/libc` headers and helpers instead of declaring per-module WASMOS imports locally.
- Debugging: the `debug_mark(tag)` wasm native logs a tag and PID to serial to confirm app execution paths.
- Debugging: PM logs app flags and entry returns, and `sysinit` emits debug_mark tags `0x1101..0x11FF` to trace its loop behavior in preemptive runs.
- Debugging: the kernel init path can temporarily bypass boot module spawning via `g_skip_wasm_boot` in `src/kernel/kernel.c` when isolating the wasm3 probe.
- The #GP exception handler logs err/rip/cs/rflags plus current PID/name and stack bounds for debugging non-canonical instruction pointers.
- Process stacks carry canaries at base/top and are checked on each process entry to catch overflows early.
- Process stacks reserve an unmapped guard page above and below the usable stack to catch overflows as page faults.
- The bootloader loads `kernel.elf` from the EFI System Partition (ESP).
- CLion IDE targets (`bootloader_ide`, `kernel_ide`) aggregate all project C/H sources and include directories (kernel, drivers, wasm3) for indexing.
- The Kernel Architecture Guide in `ARCHITECTURE.md` outlines microkernel design decisions and the stepwise roadmap.
- The Bootloader & Kernel Architecture rework section in `ARCHITECTURE.md` is the current design baseline.
- IPC guidance now incorporates seL4/QNX-style separation of synchronous IPC and asynchronous notifications.
- The virtual memory plan (address space layout, page tables, fault handling) is defined in `ARCHITECTURE.md`.
- The privilege model (apps unprivileged, drivers privileged, services least-privileged) is defined in `ARCHITECTURE.md`.
- CPU security and isolation features (NX, SMEP/SMAP, ring separation) are defined in `ARCHITECTURE.md`.
- The WASMOS WASM application container format (WASMOS-APP) is defined in `ARCHITECTURE.md`.
- The planned driver/service baseline (virtio + SATA + FAT32, PM, sysinit, CLI) is defined in `ARCHITECTURE.md`.
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
- WASMOS-APP loading scaffold is implemented (`src/kernel/wasmos_app.c`); the bootloader now preloads `esp/apps/chardev_client.wasmosapp` and passes it via boot modules.
- WASMOS-APP required endpoints and capability requests are now enforced during app start via kernel policy hooks.
- The bootloader logs basic status messages to the UEFI console and retries `ExitBootServices` on invalid parameters.
- The bootloader copies the UEFI memory map into kernel-owned pages before exiting boot services.
- PT_LOAD segments are loaded with page-aligned allocations (misaligned physical addresses are handled).
- Overlapping PT_LOAD segments reuse existing allocations instead of re-allocating pages.
- The kernel entry receives a `boot_info_t` with framebuffer/memory map placeholders.
- `boot_info_t` now includes the ACPI RSDP pointer and length for early hardware discovery.
- A minimal `hw-discovery` service now scans the ACPI RSDP and starts core drivers (ATA + FAT) from boot modules; `sysinit` launches the CLI after `fs-fat` is running.
- The kernel entry preserves the incoming `boot_info_t *` (UEFI passes it in `RCX`) through early init.
- The kernel emits early serial output on COM1 (QEMU `-serial`).
- Basic CPU init now installs a minimal kernel GDT/IDT and exception stubs for vectors `0..31`.
- Memory management scaffolding tracks per-runtime memory regions (microkernel model).
- A minimal physical frame allocator ingests the UEFI memory map (conventional + boot services memory).
- The physical frame allocator supports freeing pages, and contexts can allocate regions from it.
- `mm_init` scaffolds a root context with basic linear/stack/heap regions.
- `mm_init` now sets up kernel-owned page tables, reloads `CR3`, and installs a higher-half alias mapping at `0xFFFFFFFF80000000`.
- The root context also reserves placeholder IPC and device regions.
- `mm_context_create` allocates a new context and default regions.
- The wasm3 runtime uses a per-process bump allocator in `src/kernel/wasm3_shim.c` and does not use the wasm3 fixed heap.
- Kernel primitives now include a minimal spinlock (`src/kernel/spinlock.c`) and IPC transport with per-endpoint queues (`src/kernel/ipc.c`).
- Kernel primitives now include basic cooperative process management (`src/kernel/process.c`) with per-process memory-context binding.
- PIT timer init now programs IRQ0 and increments a kernel tick counter; the kernel logs a tick milestone via a deferred poll in the scheduler loop.
- The scheduler now tracks per-process tick accounting and sets a reschedule flag when a time slice expires (preemption still pending).
- READY processes are now managed via a simple run queue instead of a full scan each scheduling step.
- Scheduler metrics (timer ticks, ready queue depth, current running PID) are exposed via wasm natives and shown in `ps`.
- The scheduler now switches into process contexts via a minimal context-switch trampoline; timer-driven preemption is enabled.
- Timer IRQ preemption now redirects the interrupted RIP to a kernel preempt trampoline before yielding to the scheduler.
- Spinlocks now disable preemption while held to keep IPC and scheduler paths safe.
- WASMOS apps can terminate themselves via a `proc_exit` wasm native; the process manager reaps exited apps and releases their runtime instances.
- An explicit idle task runs `hlt` when no READY tasks are available.
- Process stacks are allocated from the physical frame allocator, and the kernel image range is reserved before handing out pages.
- Process lifecycle primitives now include `wait`, `kill`, and tracked `exit_status` via zombie processes until reaped.
- Blocked processes can now be resumed by context (`process_wake_by_context`) when IPC traffic arrives for owned endpoints.
- A minimal init process runs in the kernel and is the root parent for all kernel-spawned processes (mem-service, chardev-server, pagefault-test, and the process manager).
- The process manager owns a `proc` IPC endpoint and can `spawn`, `wait`, `kill`, and `status` processes on behalf of callers.
- The kernel `init` process requests the process manager to spawn the `sysinit` WASMOS-APP boot module, passing the `proc` endpoint and boot module metadata.
- The user-space `sysinit` module iterates boot modules (excluding itself) and spawns them via `proc`.
- A minimal PIO ATA block driver runs as a WASMOS-APP service (`src/drivers/ata`), exposes a `block` IPC endpoint, and supports identify/read requests.
- A FAT12/16/32 filesystem driver runs as a WASMOS-APP service, uses the block IPC endpoint, and exposes the `fs` IPC endpoint (now includes VFAT LFN support for `ls`, `cd`, and `cat`).
- A minimal user-space `cli` WASMOS-APP is loaded as a boot module, reads input from serial, and supports `help`, `ps`, `ls`, `cat`, `cd`, and `exec` (loads WASMOS-APPs from disk; drivers/services are rejected).
- IPC endpoint permissions are enforced by context-aware APIs (`ipc_send_from`, `ipc_recv_for`) for source-endpoint ownership and endpoint receive ownership.
- The kernel now builds and embeds example WASM applications from `examples/` (including `chardev_client`).
- `lib/libc` provides a separate user-space libc layer (minimal `string`/`stdio` plus host-import wrappers) for C, Go, Rust, Zig, and AssemblyScript examples.
- Driver wasm link settings currently constrain module stack/linear memory for low-footprint instantiation in the freestanding runtime pool.
- A generic kernel wasm driver host (`src/kernel/wasm_driver.c`) loads embedded modules, instantiates them via wasm3, and provides an entry call for long-running drivers.
- The WASM-backed chardev runs as an IPC service in a dedicated `chardev-server` process (`src/kernel/wasm_chardev.c`) using `src/drivers/chardev/chardev_server.c`.
- The wasm3 host ABI wrappers and import registration live in `src/kernel/wasm3_link.c`, leaving `src/kernel/kernel.c` focused on boot flow and process bring-up.
- Boot modules now include `sysinit.wasmosapp` and `chardev_client.wasmosapp`; the chardev client performs one IPC write/read roundtrip via imported IPC primitives.
- The chardev server process blocks in `ipc_recv` when its IPC queue is empty and is woken by incoming IPC messages.
- The chardev service path uses permission-aware IPC send/receive calls tied to its owner context.
- The chardev module export contract is `initialize` (the driver handles IPC internally).
- Chardev IPC protocol uses request/response message types for byte read/write (`WASM_CHARDEV_IPC_*` in `src/drivers/include/wasmos_driver_abi.h`).
- IPC best-practice notes and improvement targets (from Herder’s MINIX thesis and Aigner’s microkernel communication work) are tracked in `ARCHITECTURE.md`.
