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
- 64-bit (`x86_64`) UEFI microkernel OS scaffold with deterministic boot handoff (`BOOTX64.EFI` -> `kernel.elf` + `initfs.img`).
- WASM-first userspace (`wasm3`) that runs apps, services, and drivers, plus optional native drivers where hardware access needs it.
- Explicit microkernel primitives: paging, scheduler, IPC, process lifecycle, capabilities, and ring-3 isolation enabled by default.
- Service-driven system bring-up (`init` -> `fs-manager`/`fs-init` -> `device-manager` -> `sysinit`) with discovery/registration and policy-driven driver spawning.
- Early windowing/graphics stack: framebuffer driver, compositor, shared-buffer rendering, input routing, and runtime display mode control.
- Practical interactive environment with VT/CLI, multi-TTY switching, and scriptable boot-time userspace workflows.

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
- `run-qemu-ring3-test`: strict ring-3 smoke path (includes PM owner-deny test-hook marker checks)
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
- `fs-manager` client session state is now tracked in a heap-grown chunk list (no fixed 64-client table cap)
- `mount` reporting is now served directly by `fs-manager` (`FSMGR_IPC_QUERY_MOUNTS_REQ`) so filesystem mount ownership/query no longer depends on device-manager mount indices
- `fs-fat` and `fs-init` are backend filesystem drivers registered into `fs-manager`; `fs-init` now serves real initfs file entries through normal filesystem open/read/readdir operations (no embedded fallback payload for `default.rules`)
- bootstrapping now brings up `fs-manager` + `fs-init` before `device-manager`, so later startup lookups can resolve via the VFS namespace rather than only early boot-module ordering
- kernel now exposes generic cross-context buffer borrows (`buffer_borrow`/`buffer_release`) with typed buffer classes and read/write grants; `fs-manager` uses the FS class for zero-copy backend proxying
- process-manager FS/framebuffer per-context borrow slots are list-backed (no fixed `PROCESS_MAX_COUNT` slot arrays)
- MM context registry and per-context capability state are list-backed (no fixed `MM_MAX_CONTEXTS` runtime slot cap)
- Per-context MM regions are list-backed (no fixed `MM_MAX_REGIONS` table per context)
- native framebuffer driver mapping now uses the same generic borrow path (`PM_BUFFER_KIND_FRAMEBUFFER`) instead of a dedicated framebuffer mapper callback
- native driver ABI now has explicit magic/version fields and fails fast on mismatch to avoid mixed-kernel/driver function-table corruption; shared-memory operations include a native `shmem_flush` hook for explicit writeback to shared pages when needed.
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
