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
- Deterministic UEFI boot handoff (`BOOTX64.EFI` -> `kernel.elf` + `initfs.img`)
- Microkernel baseline: paging, scheduling, IPC, process lifecycle, exceptions
- WASM-first runtime model with optional native driver payloads
- FS-first startup chain (`init` -> `fs-manager` -> `fs-init` -> `device-manager` -> `pci-bus` -> `ata` -> `fs-fat` -> `sysinit`) with PCI-inventory-driven matching
- Spawn-time driver capability profiles for PCI paths (PIO range + IRQ mask)
- PM service registry for user-space endpoint discovery (`register`/`lookup`)
- Usable VT/CLI stack with multi-TTY switching
- CLI now includes `mount` command for active mount/device inspection
- CLI `cat` now uses regular libc file I/O against the current working directory (no dedicated root-cat FS IPC opcode)
- libc now includes `listdir` (`FS_IPC_READDIR_REQ` + stream response handling) and language wrappers expose matching directory-read helpers (`ReadDir`/`read_dir`/`readDir`)
- libc `read`/`write` now route stdio FDs (`0`/`1`/`2`) to console hostcalls
- libc and language wrappers now provide line-oriented console input helpers (`readline`)
- libc now includes process-manager metadata helper APIs in `wasmos/proc.h` (`PROC_IPC_MODULE_META` / `PROC_IPC_MODULE_META_PATH`)
- kernel process-manager internals are now split into focused modules (`process_manager_buffers`, `process_manager_services`, `process_manager_spawn`) to keep lifecycle, registry, and buffer-borrow logic isolated
- kernel CPU implementation is now split into generic dispatch (`src/kernel/cpu.c`) and x86_64-specific internals (`src/kernel/arch/x86_64/cpu_x86_64.c`)
- kernel IRQ implementation is now split into generic dispatch (`src/kernel/irq.c`) and x86_64-specific internals (`src/kernel/arch/x86_64/irq_x86_64.c`)
- kernel init-process bootstrap state machine is now extracted into `src/kernel/kernel_init_runtime.c` (`kernel_init_entry`), reducing `kernel.c` orchestration surface
- kernel boot-shadow copy, low-slot diagnostic, and scheduler-loop runtime are now extracted into `src/kernel/kernel_boot_runtime.c`, further narrowing `kernel.c` responsibilities
- baseline kernel selftests (page-fault recovery, IPC wake/send, optional preempt probe) are now extracted into `src/kernel/kernel_selftest_runtime.c`
- kernel threading selftests (internal-worker/join-order/ipc-stress orchestration) are now extracted into `src/kernel/kernel_threading_selftest_runtime.c`
- ring3 fault-policy orchestration (fault-status checks, containment liveness, churn/watchdog validation) is now extracted into `src/kernel/kernel_ring3_fault_runtime.c`
- ring3 probe/fault spawn helpers (native/thread-lifecycle probe setup and ring3 fault-probe builders) are now extracted into `src/kernel/kernel_ring3_probe_runtime.c`
- ring3 smoke + shared-memory isolation selftest runtime (ring3 smoke process staging and shmem owner/misuse checks) is now extracted into `src/kernel/kernel_ring3_smoke_runtime.c`
- ring3 suite orchestration in `kmain` is now collapsed into `src/kernel/kernel_ring3_suite_runtime.c` (spawns smoke/native/threading/fault probes and wires fault-policy runtime)
- kernel runtime modules now log via a small `klog` facade (`src/kernel/klog.c`) instead of calling serial APIs directly, keeping logging backend indirection centralized
- kernel core/drivers glue files now also route log output through `klog_*` (instead of direct serial calls), unifying logging callsites across kernel code
- libc string/ctype/stdio coverage now includes common helpers (`memmove`, `strnlen`, `strchr`/`strrchr`, `strcpy`/`strncpy`, `isspace`/`isdigit`/`isxdigit`/etc., `getchar`/`putchar`/`fputs`)
- drivers/services CMake now also emits IDE-only C source targets with include paths so editor indexers can resolve headers in non-native WASM modules
- drivers/services CMake now uses shared root helper functions for wasm-C compile/packaging + IDE-target wiring, reducing per-module duplication
- Ring-3 hardening enabled by default in normal test boots
- Phase 0 DMA contract scaffolding is now defined: DMA capability bit
  (`DEVMGR_CAP_DMA`), direction/status constants, spawn-caps-v2 IPC id, and
  shared ABI structs for DMA spawn descriptors (`wasmos_spawn_caps_v2_t`)
- Phase 1 borrow-based DMA hostcall enforcement is now in kernel for WASM
  callers (`dma_map_borrow`, `dma_sync_borrow`, `dma_unmap_borrow`) with
  owner-context validation, borrow-grant checks, capability direction/range
  checks, and fail-closed unmap/release behavior
- Phase 2 spawn-caps-v2 transport is now wired end-to-end between
  process-manager IPC and kernel validation (`PROC_IPC_SPAWN_CAPS_V2`) with
  descriptor copy/validation and fail-closed rejection for invalid DMA
  descriptor payloads (including malformed variable-length window lists)
- Phase 3 storage-path integration now attempts borrow-based DMA lifecycle in
  ATA read/write request handling with deterministic fallback to the existing
  PIO/copy path and explicit one-shot fallback/active markers
- Native framebuffer driver borrow path now also attempts kernel-managed
  borrow-based DMA map/sync/unmap lifecycle with explicit active/fallback
  boot markers (`[framebuffer] dma path active` / `[framebuffer] dma fallback active`)
- Framebuffer DMA hardening now includes a kernel selftest matrix marker
  (`[test] framebuffer dma phase4 matrix ok`) for wrong-source deny, repeated
  map/sync/unmap churn, and stale-unmap deny behavior
- Ring-3 user-slot mapping now requires explicit `MEM_REGION_FLAG_USER` (legacy implicit bridge removed from page-map path)
- Syscall boundary now rejects lossy 64-bit-to-32-bit exit-status arguments (`EXIT` / `THREAD_EXIT` require valid signed-32 representation)
- Hostcall boundary now rejects negative endpoint IDs in `wasmos_serial_register` before `uint32_t` conversion
- Ring-3 IPC adversarial coverage now includes stale/future `request_id` replay denial marker (`[test] ring3 ipc call stale id deny ok`)
- Ring-3 IPC adversarial coverage now includes out-of-order pending-reply retention marker (`[test] ring3 ipc call out-of-order retain ok`)
- Ring-3 IPC adversarial coverage now includes invalid-source spoof denial marker (`[test] ring3 ipc call spoof invalid source deny ok`)
- Ring-3 IPC control-plane deny coverage now includes explicit endpoint-policy marker (`[test] ring3 ipc call control endpoint deny ok`)
- Ring-3 IPC stress now includes endpoint-ownership + sender-context auth marker (`[test] ring3 ipc owner+sender stress ok`)
- Hostcall pointer paths now consistently validate user VA/range before copies; remaining host-view sync bridge is explicitly tracked with TODOs
- Ring-3 smoke includes process-local `#PF`, `#UD`, `#GP`, `#DE`, `#DB`, `#BP`, `#OF`, `#NM`, `#SS`, and `#AC` fault-policy checks
- Ring-3 fault-policy smoke now includes containment liveness marker (`[test] ring3 containment liveness ok`)
- Ring-3 fault-policy mixed-churn liveness marker remains enforced (`[test] ring3 mixed stress ok`)
- Dedicated strict ring-3 multi-process fault-storm profile is available via `run-qemu-ring3-fault-storm-test` and asserts watchdog cleanliness/progress (`[test] ring3 watchdog clean ok`, `[test] sched progress ok`)
- CLI integration target now isolates each QEMU session to a private ESP copy (`WASMOS_QEMU_ISOLATE_ESP=1`) and emits deterministic suite status marker (`[test] cli suite status ok`)
- Ring-3 smoke includes shared-memory owner/grant/revoke isolation checks (kernel and user-space app-pair paths)
- Shared-memory app-pair smoke now also checks forged-ID deny, map-argument policy deny, and post-revoke stale-ID deny
- Strict ring3 boot smoke now includes a kernel-level shared-memory misuse matrix marker (`[test] ring3 shmem misuse matrix ok`)
- Kernel threading Phase B now includes schedulable internal worker threads with per-thread kernel stacks (`[test] threading internal worker ok`)
- Kernel threading join-order smoke now validates in-process thread-join wake ordering in a dedicated probe path (`[test] threading join wake order ok`)
- Kernel threading Phase B now includes a targeted multi-thread IPC stress marker (`[test] threading ipc stress ok`)
- Threading Phase C syscall baseline now includes native ring3 `gettid` and `thread_yield` coverage (`[test] ring3 native gettid ok`, `[test] ring3 thread yield syscall ok`)
- Threading Phase C syscall baseline now includes native ring3 `thread_exit` coverage (`[test] ring3 thread exit syscall ok`)
- Threading Phase C now includes native ring3 `thread_create` coverage with per-thread user context setup (`[test] ring3 thread create syscall ok`)
- Threading Phase C syscall baseline now includes native ring3 `thread_join` entry and self-join deny coverage (`[test] ring3 thread join syscall ok`, `[test] ring3 thread join self deny ok`)
- Threading Phase C syscall baseline now includes native ring3 `thread_detach` entry plus invalid-argument and detach-then-join deny coverage (`[test] ring3 thread detach syscall ok`, `[test] ring3 thread detach invalid deny ok`, `[test] ring3 thread detach join deny ok`)
- Threading Phase C now includes a user-facing continuation-style native thread API wrapper (`wasmos/thread_x86_64.h`) for native ring3 callers
- Threading lifecycle smoke now also validates kill-while-blocked wait wakeup behavior (`[test] threading wait kill wake ok`)
- Threading Phase D hardening markers now include join-after-kill ordering and kill-during-join waiter wakeup checks (`[test] threading join after kill order ok`, `[test] threading join kill wake ok`)

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
4. `device-manager` starts `pci-bus` (via PM endpoint lookup), consumes inventory, applies early PCI matching rules, then starts storage drivers/services (`ata`, `fs-fat`) with spawn-time capability profiles

Current driver match/capability policy source:
- driver metadata is embedded in each driver’s WASMOS-APP package
- `device-manager` queries module metadata from process-manager at runtime and matches against PCI inventory
- process-manager now also supports initfs metadata lookup by module path (`PROC_IPC_MODULE_META_PATH`) so driver startup can resolve metadata without relying only on boot-module indices
- kernel `.wap` metadata parsing/mapping helpers are now extracted into `wasmos_app_meta` so process-manager logic can reuse a focused metadata module
- all in-tree apps, drivers, and services now provide `linker.metadata` metadata consumed by `make_wasmos_app`
Current FS namespace model:
- `fs-manager` is the canonical `fs` endpoint for PM/runtime file I/O and CLI mount namespace routing (registered as `fs.vfs`)
- `fs-fat` and `fs-init` are backend filesystem drivers registered into `fs-manager`
- bootstrapping now brings up `fs-manager` + `fs-init` before `device-manager`, so later startup lookups can resolve via the VFS namespace rather than only early boot-module ordering
- kernel now exposes generic cross-context buffer borrows (`buffer_borrow`/`buffer_release`) with typed buffer classes and read/write grants; `fs-manager` uses the FS class for zero-copy backend proxying
- native framebuffer driver mapping now uses the same generic borrow path (`PM_BUFFER_KIND_FRAMEBUFFER`) instead of a dedicated framebuffer mapper callback
- native driver ABI now has explicit magic/version fields and fails fast on mismatch to avoid mixed-kernel/driver function-table corruption
- PM `spawn_name` busy responses now carry a transient error code (`arg1=-2`); `sysinit` and `device-manager` retry/yield on busy so boot-time service/driver spawns no longer race each other
- `/` is virtual in `fs.vfs`, with `/boot` routed to FAT and `/init` routed to `fs-init` (`fs.init`)
5. `init` requests `sysinit` load from FAT via process manager
6. `sysinit` starts configured services/apps from boot config

## Repository Layout
- `src/boot/`: UEFI bootloader
- `src/kernel/`: kernel core
- `src/drivers/`: drivers (WASM and native)
- `src/services/`: services
- `lib/libc/`: shared user-space libc + shims
- `examples/`: sample/smoke apps
- `tests/`: QEMU-driven tests
- `scripts/`: build/test helpers
- `docs/`: architecture/design docs

## Documentation Index
- `docs/ARCHITECTURE.md`: architecture index
- `docs/architecture/`: feature-level architecture docs
- `docs/architecture/14-ring3-isolation-and-separation.md`: ring-3 isolation and kernel/user-space separation design
- `docs/architecture/15-threading-and-lifecycle.md`: threading design and rollout
- `docs/architecture/16-dma-transfers.md`: DMA transfer capability model, phased rollout plan, and validation gates
- `docs/TASKS.md`: active and planned work
- `AGENTS.md`: contributor/agent workflow and repository rules

## Runtime Model (Brief)
- Runtime host: `wasm3`
- Process manager loads WASMOS-APP payloads
- Payloads can be WASM apps/services or native driver payloads

For the complete ABI/runtime contract and subsystem details, use the
architecture docs under `docs/architecture/`.
