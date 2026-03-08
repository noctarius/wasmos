---
name: wasmos-system-service
description: Create, wire, and validate a new WASMOS system service (WASMOS-APP) including IPC ABI usage, build integration, PM/sysinit boot sequencing, and disk-based load. Use when adding a new service under src/services/ or migrating a service to load from esp/system/services in the WASMOS repo.
---

# Wasmos System Service

## Overview

Use this skill to add a new wasm-based system service in WASMOS. It covers service
module source, IPC ABI usage, build/packaging to `esp/system/services`, PM spawn flow,
and boot sequencing via `sysinit`.

## Workflow

1. Define the service’s IPC contract and entry export.
2. Implement the wasm service module under `src/services/<service-name>/`.
3. Integrate build rules to compile/pack the WASMOS-APP.
4. Ensure the service is copied to `esp/system/services` at build time.
5. Wire PM/sysinit to spawn the service from disk after prerequisites are up.
6. Verify in QEMU and remove debug logs.

## Step 1: Define the IPC contract

- Add opcodes in `src/drivers/include/wasmos_driver_abi.h`.
- Keep requests small; use shared memory for bulk data if needed.
- Use request/response pairs and set clear error codes.

Example (service `svc-foo`):
```
FOO_IPC_QUERY_REQ  = 0x600
FOO_IPC_QUERY_RESP = 0x680
FOO_IPC_ERROR      = 0x6FF
```

## Step 2: Implement the service module

Location rule: **all system services live in subdirectories of `src/services/`**.

Create `src/services/svc-foo/foo.c`.

Minimum exports:
1. `foo_step(...)` — called repeatedly by the PM process runner.
2. Optional helper exports for internal testing only.

Use `WASMOS_WASM_IMPORT` to access IPC and console primitives:
- `ipc_create_endpoint`, `ipc_send`, `ipc_recv`, `ipc_last_field`
- `console_write` (for minimal logging)

Keep heap/stack small; avoid dynamic allocation.

## Step 3: Build integration (CMake)

Add to `CMakeLists.txt`:
- `WASM_FOO_APP_SRC`, `WASM_FOO_APP_WASM`, `WASM_FOO_APP`
- A `clang --target=wasm32` build rule (similar to `sysinit` or `cli`)
- A `WASMOS_APP_PACKER` rule to package the `.wasm` into `.wasmosapp`
- Add the app to the build target list so it is produced with the rest of the services

## Step 4: Disk placement (ESP)

System services are stored on the ESP under:
`esp/system/services/<service>.wasmosapp`

Update the `run-qemu` and `run-qemu-test` copy steps to place the service there.
If the FS service only scans `\APPS`, extend it to scan `\SYSTEM\\SERVICES`
for service loads so PM can read them by name.

## Step 5: PM/sysinit wiring

- Ensure `sysinit` spawns prerequisite drivers/services (ATA, FAT, etc.) first.
- After FS is ready, request the service by name via `PROC_IPC_SPAWN_NAME`.
- PM reads the WASMOS-APP from disk and spawns it as an app/service.

## Step 6: Bring-up checklist

1. Build: `cmake --build build --target run-qemu-test`
2. Confirm logs: service start + IPC endpoint creation
3. Exercise the IPC path with a small client
4. Remove debug logs before committing
