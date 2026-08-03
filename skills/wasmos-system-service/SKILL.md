---
name: wasmos-system-service
description: Create, wire, and validate a new WASMOS system service (WASM or native) — the `.wap` model: an `initialize`/`async_initialize` entry that owns an event loop, service/class registration, capabilities in linker.metadata, build via the wasmos_add_*_app_target helpers, placement on the mounted FS, and startup via sysinit `.rc` (start/spawn/wait-svc) or a device-manager rule. Use when adding a service under src/services/.
---

# WASMOS System Service

## Overview

A service is a `.wap` package under `src/services/<name>/`, placed on the mounted
FS at `/boot/system/services/<name>.wap` (or in the initfs image), and started by
`sysinit` (`scripts/system/sysinit.rc`) or a device-manager rule. It exports a
single entry that **owns its own event loop** and registers itself with the
process manager. WASM services (`native = false`) export `initialize`; native
services (`native = true`) export `initialize` (blocking) or `async_initialize`
(coroutine).

Legacy patterns that are GONE — do not use them:
- `foo_step(...)` "called repeatedly by the PM process runner" — **there is no PM
  step-runner**; a service runs its own loop.
- spawn by name via `PROC_IPC_SPAWN_NAME` (opcode `0x204` still defined but has
  zero users) — spawn is **path-based** (`PROC_IPC_SPAWN_PATH` / `_SYNC`).
- "extend the FS service to scan `\SYSTEM\SERVICES`" — PM reads a specific `.wap`
  by absolute VFS path; nothing scans a directory.

## Workflow

1. Author the source + entry (WASM `initialize`, or native blocking/async).
2. Register the service; run a blocking/event-loop (never busy-spin).
3. Declare `linker.metadata` (package + capabilities + entry-arg bindings).
4. Add `CMakeLists.txt` with a `wasmos_add_*_app_target` helper; register it.
5. Place the `.wap` on the FS (initfs.toml and/or ESP).
6. Start it from `sysinit.rc` (or a device-manager rule); verify in QEMU.

## Step 1: Source + entry

Under `src/services/<name>/`. Three flavors (examples cited):

- **WASM** (`native = false`) — `src/services/cli/cli.c:2193`:
  ```c
  WASMOS_WASM_EXPORT int32_t initialize(int32_t proc_endpoint, int32_t, int32_t, int32_t) {
      proc_endpoint = wasmos_startup_proc_endpoint();   // from spawn-info, not the arg
      for (;;) { /* own state machine; blocks in wasmos_ipc_select_one */ }
  }
  ```
  (`WASMOS_WASM_EXPORT` = `src/libc/include/wasmos/imports.h:13`; endpoint via
  `wasmos_startup_proc_endpoint()`, `src/libc/include/wasmos/startup.h`.)

- **Native blocking** (`native = true`, ELF entry `-e initialize`) —
  `src/services/font_service/font_service.zig:891`:
  `int initialize(wasmos_driver_api_t* api, int module_count, ...)`. Everything
  goes through the `wasmos_driver_api_t` vtable (`console_write`, `ipc_recv`,
  `ipc_wait`, `spawn_info`, `proc_notify_ready`, …); the loop blocks with
  `api->ipc_wait(...)` at idle.

- **Native async** (`native = true`, ELF entry `-e async_initialize` provided by
  libsys) — `src/services/net_stack/net_stack.c:2536`: define a
  `wasmos_sys_native_async_service_config_t wasmos_async_service` and a root
  `int32_t wasmos_async_main(wasmos_driver_api_t*, wasmos_native_coroutine_runtime_t*, void*)`;
  cooperate with `wasmos_native_coroutine_yield()`.

**Liveness rule (hard):** never busy-spin — block on `ipc_wait` /
`ipc_select_wait` / `ipc_select_listen`, or yield a coroutine
(`[[feedback_no_busy_spin_prefer_event_loops]]`).

## Step 2: Register + receive/reply (libsys)

WASM helpers in `src/libc/include/wasmos/ipc.h`:
- `ep = wasmos_ipc_create_endpoint()`.
- Register by name: `wasmos_svc_register(proc_ep, ep, "font", req_id)` (`ipc.h:230`).
- Or by virtual class + instance: `wasmos_svc_register_class(proc_ep, ep, name, class, instance, req_id)` (`ipc.h:184`).
- Announce liveness: `wasmos_sys_notify_ready(proc_ep, ep)`
  (`src/libsys/wasm/include/wasmos/libsys.h:435`) — a synchronous (`start`) parent
  blocks until this arrives.
- Request/reply: `wasmos_ipc_call(dest, src, type, req_id, a0..a3, &reply)`
  (`ipc.h:93`, blocking); one-way `wasmos_ipc_send`; blocking receive
  `wasmos_ipc_select_one(ep)`; fields via `wasmos_ipc_last_field(...)`.
- Discovery (client side) does NOT retry internally — either loop with
  `wasmos_sched_yield` (`sysinit/init.c:286`) or, better, subscribe to the
  `SVC_IPC_CLASS_EVENT` push (`net_stack.c:159`).

Native services use the same semantics through the vtable + Zig/C libsys wrappers
(`sys.svcRegister`, `sys.NativeEventLoop`/`eventLoopPoll`,
`wasmos_sys_native_event_loop_*`).

## Step 3: `linker.metadata`

Real example (`src/services/net_stack/linker.metadata`):
```toml
version = 1
# aot = true            # add for WASM/AOT services (cli, vt, fs_manager, sysinit)
[package]
name  = "net-stack"
entry = "initialize"    # async native links -e async_initialize
kind  = "service"
native = true           # true = native ELF; false = WASM/AOT
[resources]
stack_pages = 16
heap_pages  = 512       # for WASM services this sizes the WARP reserved linmem slot
[ipc]
required_endpoint_name  = "proc"
entry_arg_bindings = ["proc.endpoint"]   # cli adds "cli.tty.alloc"; sysinit adds module.count/index
[[capabilities]]
name  = "ipc.basic"     # or "system.control" (cli, fs_manager)
flags = 0
```

## Step 4: Build (CMake)

Per-service `CMakeLists.txt`, using the root-`CMakeLists.txt` helpers:
- WASM/AOT: `wasmos_add_wasm_c_app_target(...)` (`CMakeLists.txt:476`); Zig via
  `cmake/WasmosZigApp.cmake`.
- Native C: `wasmos_add_native_c_app_target(...)` (`CMakeLists.txt:589`) — links
  `ld.lld -e <entry> -static` and packs with the app packer. `net_stack` hand-rolls
  the equivalent (`-e async_initialize`) and links the libsys native runtime objects
  (`coroutine_native.c`, `ipc_future_native.c`, `service_async_entry_native.c`,
  `heap_native.c`, `coroutine_native_x86_64.S`).
- Native Zig: hand-rolled (`font_service/CMakeLists.txt`, `zig build-obj -target
  x86_64-freestanding`, link `-e initialize`).

Pack: `${WASMOS_APP_PACKER} --manifest linker.metadata --in <elf/wasm> --out
<name>.wap` (`make_wasmos_app` builds the packer). Register into the build:
```cmake
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APPS        <path>.wap)
set_property(GLOBAL APPEND PROPERTY WASMOS_WASM_APP_TARGETS <target>)
```
and add the dir in `src/services/CMakeLists.txt`.

## Step 5: Place the `.wap` on the FS

Services load by absolute VFS path (no directory scan). Place via:
- **initfs image** (early/RAM): add to `scripts/initfs.toml`
  (`path = "system/services/<name>.wap"`, `source = "<name>.wap"`); built into
  `build/initfs.img`.
- **ESP/FAT** (`/boot`): assembled into `build/esp/system/services/<name>.wap` by
  the QEMU run targets.

## Step 6: Startup wiring

Add the service to `scripts/system/sysinit.rc` (run by sysinit via the wasmos
script engine, `src/services/sysinit/init.c:340`):
- `spawn <path>` — fire-and-forget; sends `PROC_IPC_SPAWN_PATH` (autoreap).
- `start <path>` — synchronous; sends `PROC_IPC_SPAWN_PATH_SYNC` and blocks until
  the child sends `PROC_IPC_NOTIFY_READY`.
- `wait-svc <name>` — block until a dependency registers (ordering).
- `if -f <path> then … endif`, `echo`, `export`, `exec`.

```
start /boot/system/services/vt.wap
if -f /boot/system/services/fontsvc.wap then
    start /boot/system/services/fontsvc.wap
endif
spawn /boot/system/services/gfxcomp.wap
wait-svc gfx
start /boot/system/services/cli.wap
```

For very-early bring-up (before sysinit), use a device-manager rule instead
(`scripts/initfs/devmgr/rules/default.rules`, e.g.
`SUBSYSTEM=="boot", RUN+="system/services/net_stack.wap"`). The kernel itself
path-spawns only `sysinit` (`src/kernel/kernel_init_runtime.c:393`).

## Step 7: Verify

```sh
cmake -S . -B build && cmake --build build --target run-qemu-test
```
Confirm the service's ready log and endpoint registration; exercise its IPC from a
client. Remove debug markers before committing.

## Guardrails
- Never busy-spin; block on a select set / `ipc_wait`, or yield a coroutine.
- Keep `libc`/`libsys` and their per-runtime wrappers in sync (repo rule).
- Never modify `libs/warp` or `libs/wasm3`.
- Prefer class registration + discovery (`svc.class`) over hardwired names.
