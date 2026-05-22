<p align="center">
  <picture>
    <source srcset="wasmos_wordmark-light.webp" type="image/webp" media="(prefers-color-scheme: light)">
    <source srcset="wasmos_wordmark-dark.webp" type="image/webp" media="(prefers-color-scheme: dark)">
    <img src="wasmos_wordmark-light.svg" alt="WASMOS wordmark" width="520">
  </picture>
</p>

<p align="center">
  <picture>
    <source srcset="wasmo.webp" type="image/webp">
    <img src="wasmo.svg" alt="Wasmo the WASMOS mascot" width="180">
  </picture>
</p>

<p align="center"><strong>Small boot path. Small kernel. Large mascot and agent energy.</strong></p>

WASMOS is a minimal x86_64 UEFI OS playground with a small microkernel core
and a WASM-first user-space stack (`wasm3`), plus optional native drivers for
hardware paths that benefit from native execution.

It is designed for experimentation, not production use.

For contributors and coding agents: read `AGENTS.md` before making changes.
It defines repository workflow and documentation/update conventions.

## Current Highlights
- Deterministic UEFI boot handoff (`BOOTX64.EFI` -> `kernel.elf` + `initfs.img`) with a small x86_64 microkernel baseline (paging, scheduler, IPC, process lifecycle, exceptions).
- WASM-first userspace runtime (wasm3) with optional native drivers/services where hardware paths benefit from native execution.
- Shared `libsys` helper layer is split by runtime: `src/libsys/wasm` (hostcall-backed C helpers for wasm-compiled apps/services/drivers) and `src/libsys/native` (native-driver-backed helpers + Zig wrappers for native Zig services/drivers).
- `libsys` now also provides lightweight event-loop + intent helpers (event handler registry + request/response demux by `request_id`) so services can process unsolicited IPC events and solicited replies without ad-hoc blocking receive loops.
- Service-driven startup chain with endpoint registry and discovery (`register`/`lookup`) plus PCI-inventory-driven driver bring-up.
- Practical VT/CLI environment with multi-TTY switching, fail-fast script execution (`script <file>`), basic environment variables (`export`, `echo ${VAR}`), PATH-based app lookup, and core inspection commands (`ps`, `kmaps`, `mount`, `exec`, etc.).
- Device-manager policy roots are `/init/devmgr/rules` (bootstrap) and `/boot/system/devmgr/rules` (runtime override), using udev-style rule syntax. Storage bring-up is rule-driven from `/init/devmgr/rules/default.rules`: `SUBSYSTEM=="pci", ATTR{class}=="0x01", ATTR{subclass}=="0x01", ...` matches ATA-class controllers, optional BDF selectors (`ATTR{bus}`, `ATTR{slot}`, `ATTR{function}`) can pin rules to specific controllers, `SUBSYSTEM=="block", ATTR{unit}=="N", ENV{MOUNT}="..."` gates `fs-fat` spawns after block-device publication, and `/boot` rules can add other PCI-match rules such as `SUBSYSTEM=="pci", ATTR{class}=="0x03", ...`. Boot override rules that target bootstrap storage drivers (`ata`, `fs-fat`) are rejected at runtime.
- WASM `device-manager` now uses shared `libsys` reactor/intents across its reply, inventory/query, and boot-rules reply endpoints, replacing ad-hoc blocking/nonblocking receive loops while preserving existing startup policy behavior.
- Ring-3 isolation/hardening enabled by default, with stress/fault/self-test coverage across IPC, faults, threading, and shared memory.
- Thread lifecycle support is available end-to-end (`thread_create`, `thread_join`, `thread_detach`, `thread_yield`, `thread_exit`) for ring3.
- Shared-memory and capability plumbing supports owner/grant/revoke flows, including compositor/client buffer sharing and auto-mapping helpers.
- Borrow-based DMA path is integrated across capability contract, hostcalls, spawn transport, and storage/framebuffer paths (with validated fallback behavior).
- Graphics stack is active: framebuffer driver, native Zig `gfx-compositor`, shared-buffer present/damage model, and app-facing IPC ABI.
- Compositor interaction supports focus/z-order, pointer/key events, move/resize/close window controls, software cursor/chrome, and live resize notifications.
- Native Zig `font-service` scaffold is now available (`/boot/system/services/fontsvc.wap`) with TTF loading path and basic font-open/metrics IPC.
- Native Zig `font-service` now runs on the shared native `libsys` reactor/intent pattern (single endpoint poll + request-id intent demux), including explicit warnings for unhandled IPC event types.
- Manual graphics smoke app (`/boot/apps/gfx_smoke.wap`) validates multi-window focus/raise/drag/resize/close behavior; PS/2 mouse driver is available at `/boot/system/drivers/mouse.wap`.

## Quick Start

### Requirements
- `clang` + `lld`
- `llvm-objcopy`
- `cmake` 3.20+
- `qemu-system-x86_64`

macOS note:
- use Homebrew LLVM (`appleclang` is not sufficient for the UEFI target)
- install with: `brew install llvm lld qemu`

### Configure
```sh
cmake -S . -B build
```

Optional Kconfig-style flow:
```sh
cmake --build build --target kconfig-defconfig
cmake --build build --target menuconfig
cmake -S . -B build
```

Notes:
- `kconfig-defconfig` seeds `build/.config` from `configs/wasmos_defconfig`.
- `menuconfig` auto-detects a frontend binary (`menuconfig`, `nconfig`,
  `kconfig-mconf`, or `mconf`); if none are found, it falls back to the repo's
  Python `kconfiglib` interactive editor.
- `kconfiglib-menuconfig` runs the Python `kconfiglib` editor directly.
- Python fallback requirement: `python3 -m pip install kconfiglib`.
- If no frontend is installed, you can still edit `build/.config` directly and
  re-run `cmake -S . -B build` to import the changes.
- Current Kconfig symbols cover the core toggles already used by CMake:
  language example switches, tracing/ring3 smoke flags, kernel target triple,
  and QEMU GDB port.
  It also includes `WASMOS_PM_TEST_HOOKS` for process-manager test injection hooks.

If tool autodiscovery fails:
```sh
cmake -S . -B build \
  -DCLANG=/path/to/llvm/bin/clang \
  -DLLD_LINK=/path/to/lld-link
```

If OVMF autodiscovery fails:
```sh
cmake -S . -B build -DOVMF_CODE=/path/to/OVMF_CODE.fd
```

Optional vars image:
```sh
cmake -S . -B build \
  -DOVMF_CODE=/path/to/OVMF_CODE.fd \
  -DOVMF_VARS=/path/to/OVMF_VARS.fd
```

### C++ Usage
- C++ is supported for higher-level kernel, driver, service, and app code.
- Low-level boot/arch/interrupt/memory-management and ABI boundary code stays C/ASM.
- WASM C++ modules are built with `-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit`.
- Keep kernel/syscall/hostcall interfaces C ABI stable (`extern "C"` at boundaries).
- Prefer "C with classes" style and explicit ownership; avoid hidden runtime-heavy patterns.

WASM C++ app target helper:
```cmake
wasmos_add_wasm_cpp_app_target(my_cpp_app
  SOURCE ${CMAKE_SOURCE_DIR}/examples/cpp/my_app.cpp
  OUTPUT_WASM ${BUILD_DIR}/my_app.wasm
  OUTPUT_APP ${BUILD_DIR}/my_app.wap
  MANIFEST ${CMAKE_SOURCE_DIR}/examples/cpp/my_app.manifest.toml
  EXPORT wasmos_main
)
```

### Build
```sh
cmake --build build --target bootloader
cmake --build build --target kernel
cmake --build build --target make_wasmos_app
```

### Run
```sh
cmake --build build --target run-qemu
cmake --build build --target run-qemu-debug
cmake --build build --target run-qemu-ui-test
```

### Test
```sh
cmake --build build --target run-kernel-unit-tests
cmake --build build --target run-qemu-test
cmake --build build --target run-qemu-cli-test
cmake --build build --target run-qemu-ring3-test
cmake --build build --target run-qemu-ring3-threading-test
```

Target summary:
- `run-qemu`: normal boot in QEMU
- `run-qemu-debug`: paused boot for GDB attach
- `run-qemu-test`: compile + boot + halt smoke
- `run-qemu-cli-test`: CLI integration suite
- `run-qemu-ring3-test`: strict ring-3 smoke path
- `run-qemu-ring3-threading-test`: opt-in strict ring-3 threading smoke (ring3-threading spawn + ring3 thread `create`/`join`/`detach` syscall markers including detach-then-join deny + wait/kill wake marker)

## Startup Model
Boot sequence (high level):
1. `BOOTX64.EFI` loads `kernel.elf` and `initfs.img`
2. Kernel boots, initializes core subsystems, starts `init`
3. `init` starts `fs-manager`, then `fs-init`, then `device-manager`
4. `device-manager` starts `pci-bus` (via PM endpoint lookup), consumes inventory, applies udev-style match rules, then starts drivers/services through rules (`SUBSYSTEM=="boot"` + `RUN+=...` for forced spawns, `SUBSYSTEM=="pci"` + `ATTR{...}` for PCI matches, `SUBSYSTEM=="block"` + `ATTR{unit}`/`ENV{MOUNT}` for `fs-fat` bring-up)

Current driver match/capability policy source:
- driver metadata is embedded in each driver’s WASMOS-APP package
- `device-manager` queries module metadata from process-manager at runtime and matches against PCI inventory
- process-manager now also supports initfs metadata lookup by module path (`PROC_IPC_MODULE_META_PATH`) so driver startup can resolve metadata without relying only on boot-module indices
- kernel `.wap` metadata parsing/mapping helpers are now extracted into `wasmos_app_meta` so process-manager logic can reuse a focused metadata module
- all in-tree apps, drivers, and services now provide `linker.metadata` metadata consumed by `make_wasmos_app`
Current FS namespace model:
- `fs-manager` is the canonical `fs` endpoint for PM/runtime file I/O and CLI mount namespace routing (registered as `fs.vfs`)
- `mount` reporting is now served directly by `fs-manager` (`FSMGR_IPC_QUERY_MOUNTS_REQ`) so filesystem mount ownership/query no longer depends on device-manager mount indices
- `fs-fat` and `fs-init` are backend filesystem drivers registered into `fs-manager`; `fs-init` now serves real initfs file entries through normal filesystem open/read/readdir operations (no embedded fallback payload for `default.rules`)
- bootstrapping now brings up `fs-manager` + `fs-init` before `device-manager`, so later startup lookups can resolve via the VFS namespace rather than only early boot-module ordering
- kernel now exposes generic cross-context buffer borrows (`buffer_borrow`/`buffer_release`) with typed buffer classes and read/write grants; `fs-manager` uses the FS class for zero-copy backend proxying
- native framebuffer driver mapping now uses the same generic borrow path (`PM_BUFFER_KIND_FRAMEBUFFER`) instead of a dedicated framebuffer mapper callback
- native driver ABI now has explicit magic/version fields and fails fast on mismatch to avoid mixed-kernel/driver function-table corruption
- PM `spawn_name` busy responses now carry a transient error code (`arg1=-2`); `sysinit` and `device-manager` retry/yield on busy so boot-time service/driver spawns no longer race each other
- QEMU now wires a second FAT source directory from repo-root `userfs/`; `fs.vfs` reserves `/user` for that secondary FAT backend while `/boot` remains the primary FAT mount and `/init` is routed to `fs-init` (`fs.init`)
- device-manager rule roots are now reserved at `/init/devmgr/rules` (bootstrap) and `/boot/system/devmgr/rules` (runtime override) as the starting point for udev-like dynamic device/mount policy
5. `init` requests `sysinit` load from FAT via process manager
6. `sysinit` starts configured services/apps from boot config

## Repository Layout
- `src/boot/`: UEFI bootloader
- `src/kernel/`: kernel core
- `src/drivers/`: drivers (WASM and native)
- `src/services/`: services
- `src/libc/`: shared user-space libc + shims
- `examples/`: sample/smoke apps
- `userfs/`: host-backed user filesystem directory attached as a second FAT drive in QEMU
- `scripts/initfs/devmgr/rules/`: bootstrap device-manager rules packaged into initfs at `/init/devmgr/rules`
- `scripts/system/devmgr/rules/`: runtime override rules copied to ESP at `/boot/system/devmgr/rules`
- `tests/`: QEMU-driven tests
- `scripts/`: build/test helpers
- `docs/`: architecture/design docs

## Documentation Index
- `docs/ARCHITECTURE.md`: architecture index
- `docs/architecture/`: feature-level architecture docs
- `docs/architecture/14-ring3-isolation-and-separation.md`: ring-3 isolation and kernel/user-space separation design
- `docs/architecture/15-threading-and-lifecycle.md`: threading design and rollout
- `docs/architecture/16-dma-transfers.md`: DMA transfer capability model, phased rollout plan, and validation gates
- `docs/architecture/17-graphics-framebuffer-and-compositor.md`: microkernel graphics stack design (framebuffer driver, shared-buffer IPC model, compositor ABI, and phased implementation plan)
- `docs/TASKS.md`: active and planned work
- `AGENTS.md`: contributor/agent workflow and repository rules

## Runtime Model (Brief)
- Runtime host: `wasm3`
- Process manager loads WASMOS-APP payloads
- Payloads can be WASM apps/services or native driver payloads

For the complete ABI/runtime contract and subsystem details, use the
architecture docs under `docs/architecture/`.
