---
name: wasmos-system-util
description: Create, wire, and validate a new WASMOS system utility — a one-shot CLI tool (`.wap`, kind="app") under src/utils/ that reads an argument string, does its work via libc (stdio/FS) and service IPC, then exits. Covers the main()/STARTUP_SHIM entry, arg parsing, service-client IPC, linker.metadata, build via wasmos_add_wasm_c_app_target, ESP placement under /boot/system/utils, and CLI PATH resolution. Use when adding a tool under src/utils/ (cat, ip, host, ps, curl, …).
---

# WASMOS System Utility

## Overview

A utility is a **one-shot** program (a `.wap` with `kind = "app"`) under
`src/utils/<name>/`, run on demand from the CLI. Unlike a service it does **not**
register an endpoint or run a forever loop — it reads its argument string, does
its work (libc stdio + FS, and IPC *as a client* of services), returns an exit
code, and the process exits. Examples: `src/utils/{cat,ip,host,ps,curl,date,
sysinfo,explorer,sched_info}`.

Contrast with `skills/wasmos-system-service` (long-lived: exports `initialize`,
registers a service, owns an event loop). A util is the opposite: short-lived,
unregistered, `int main()`.

## Workflow

1. Author `src/utils/<name>/<name>.c` as an ordinary `int main(void)`.
2. Read args, do work via libc (stdio/FS) and — if needed — service IPC as a client; return an exit code.
3. Declare `linker.metadata` (`kind = "app"`, `entry = "wasmos_main"`).
4. Add `CMakeLists.txt` with `wasmos_add_wasm_c_app_target(... STARTUP_SHIM)`; register it.
5. Copy the `.wap` to `esp/system/utils/` and attach it to the `system_utils` target.
6. Run it from the CLI (typed by name via PATH); verify in QEMU.

## Step 1: Author `main()`

Write an ordinary C program with `int main(void)`. The build's `STARTUP_SHIM`
supplies the actual `wasmos_main` export (`src/libc/src/startup.c`), which calls
`main()` and `proc_exit`s with its return value — so you never write
`wasmos_main` or `proc_exit` yourself. Real example `src/utils/host/host.c:58`:

```c
#include "wasmos/startup.h"
/* ... */
int main(void) {
    char args[128];
    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));   // host.c:70 — the argument string
    /* parse args, do work, print, return an exit code */
    puts("...");           // libc stdio → stdout (routed to the CLI/VT)
    return 0;              // STARTUP_SHIM turns this into proc_exit(0)
}
```

## Step 2: Args, I/O, and service IPC (as a client)

- **Args** arrive as a single NUL-terminated string, not an argv array. Copy it
  with `wasmos_startup_args(buf, cap)` (`src/libc/include/wasmos/startup.h`) and
  tokenize it yourself (see `host.c` `first_token`).
- **Output/FS** use libc: `puts`/`printf` to stdout, and `open`/`read`/`write`
  for files (e.g. `cat`, `curl -o`).
- **Talk to a service as a client** — look it up and call it; do NOT register or
  loop. `host` looks up `net.stack` and issues a resolve request; use
  `wasmos_svc_lookup(...)` (`src/libc/include/wasmos/ipc.h:238`) then
  `wasmos_ipc_call(dest, src, type, req_id, a0..a3, &reply)` (`ipc.h:93`).
- Return non-zero on error (the exit code is observable by the CLI).

## Step 3: `linker.metadata`

Real example (`src/utils/host/linker.metadata`) — note `kind = "app"`:
```toml
version = 1
[package]
name  = "host"
entry = "wasmos_main"     # provided by the STARTUP_SHIM
kind  = "app"
native = false
aot = true
[resources]
stack_pages = 16
heap_pages  = 16          # utils are small and short-lived; keep these modest
[ipc]
required_endpoint_name  = "proc"
[[capabilities]]
name  = "ipc.basic"       # add more only if the tool needs them
flags = 0
```
Most utils need only `ipc.basic` (they reach hardware indirectly, via a service).

## Step 4: Build (CMake)

Per-util `CMakeLists.txt`, using the shared helper with `EXPORT wasmos_main` +
`STARTUP_SHIM` (`src/utils/host/CMakeLists.txt`):
```cmake
wasmos_add_wasm_c_app_target(host_util
  SOURCE      ${WASM_HOST_SRC}
  OUTPUT_WASM ${WASM_HOST_WASM}
  OUTPUT_APP  ${WASM_HOST_APP}
  MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/linker.metadata
  EXPORT      wasmos_main
  NO_BUILTIN
  STARTUP_SHIM
)
```
The helper (root `CMakeLists.txt:476`) compiles the wasm, packs the `.wap`, and
appends to `WASMOS_WASM_APPS` / `WASMOS_WASM_APP_TARGETS`. Add
`add_subdirectory(<name>)` in `src/utils/CMakeLists.txt`, and attach the target to
the `system_utils` aggregate there (the `foreach(_util …)` list) so the run
targets rebuild it. (Zig/AssemblyScript utils exist too, e.g. `date`; mirror the
driver/service skills' non-C helpers.)

## Step 5: Place the `.wap` on the ESP

Utilities live at `/boot/system/utils/<name>.wap`. Add a copy step in the root
`CMakeLists.txt` alongside the others (`CMakeLists.txt:1363+`):
```cmake
COMMAND ${CMAKE_COMMAND} -E copy ${WASM_HOST_APP} ${BUILD_DIR}/esp/system/utils/host.wap
```
(No `sysinit.rc` entry — utilities are on-demand, not boot-started.)

## Step 6: Run + verify

The CLI resolves a bare command name through its `PATH`
(`/boot/apps:/boot/system/services:/boot/system/drivers:/boot/system/utils`,
`src/services/cli/cli.c:237`) and path-spawns the `.wap` with the typed argument
string. So:
```sh
cmake -S . -B build && cmake --build build --target run-qemu-test
# then in the guest CLI:
#   host example.com
#   cat /boot/system/net/interfaces
```
Confirm the tool's output and exit; remove debug markers before committing.

## Guardrails
- A util is a **client**: never `wasmos_svc_register`; never run a forever loop —
  do the work and `return`.
- Keep `libc`/`libsys` and their wrappers in sync (repo rule).
- Never modify `libs/warp` or `libs/wasm3`.
- Use distinct/named error codes and a meaningful non-zero exit on failure.
