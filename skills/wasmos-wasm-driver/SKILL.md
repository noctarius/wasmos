---
name: wasmos-wasm-driver
description: Create, wire, and validate a new WASMOS wasm-based device driver (block, chardev, fs, etc.) including IPC ABI usage, build integration, kernel host wiring, and boot-time bring-up. Use when adding a new wasm driver module or converting an in-kernel driver to wasm in the WASMOS repo.
---

# Wasmos Wasm Driver

## Overview

Use this skill to add a new wasm-based driver for a device in WASMOS. It covers the full path:
driver module source, IPC ABI, kernel host glue, build/packaging, and boot-time wiring.

## Workflow

1. Define the driver’s IPC contract.
2. Implement the wasm driver module under `src/drivers/<driver-name>/`.
3. Integrate build rules to compile the wasm and embed it.
4. Add kernel host glue to start the driver and dispatch IPC.
5. Wire endpoint resolution and boot sequencing.
6. Verify in QEMU and remove debug logs.

## Step 1: Define the IPC contract

- Add opcodes in `src/drivers/include/wasmos_driver_abi.h`.
- Keep requests small; use shared memory for bulk data if needed.
- Use request/response pairs and set clear error codes.

Example (new device `foo`):
```
FOO_IPC_READ_REQ  = 0x500
FOO_IPC_READ_RESP = 0x580
FOO_IPC_ERROR     = 0x5FF
```

## Step 2: Implement the wasm driver

Location rule: **all wasm drivers live in subdirectories of `src/drivers/`**.

Create `src/drivers/foo/foo.c` (or `examples/` only for test clients).

Minimum exports:
1. `foo_init(...)` — called once by the kernel host (use args for endpoints, buffers).
2. `foo_ipc_dispatch(type, arg0, arg1, arg2, arg3)` — handles IPC requests.

Use `WASMOS_WASM_IMPORT` to access IPC and console primitives:
- `ipc_create_endpoint`, `ipc_send`, `ipc_recv`, `ipc_last_field`
- `console_write` (for minimal logging)

Keep heap/stack tiny and avoid dynamic allocation.

## Step 3: Build integration (CMake)

Add to `src/drivers/<driver-name>/CMakeLists.txt`:
- `WASM_FOO_DRIVER_SRC`, `WASM_FOO_DRIVER_WASM`, `WASM_FOO_DRIVER_BLOB`
- A `clang --target=wasm32` build rule (similar to chardev or FAT)
- An `llvm-objcopy` rule to embed the wasm as a blob (if it is kernel-embedded)
- For disk-loaded drivers: pack with `make_wasmos_app` and register via `WASMOS_WASM_APPS`
- For kernel-embedded blobs: register via `WASMOS_KERNEL_BLOBS`
- Create a target (e.g. `foo_app` or `foo_blob`) and append it to `WASMOS_WASM_APP_TARGETS`
  or `WASMOS_KERNEL_BLOB_TARGETS` so aggregate builds pull it in

Add the subdirectory in `src/drivers/CMakeLists.txt` so the driver is built.

## Step 4: Kernel host glue

Add a kernel wrapper like `src/kernel/wasm_foo.c` and header:
- Start the driver with `wasm_driver_start`.
- Provide `wasm_foo_endpoint()` and `wasm_foo_service_once()`.
- Translate IPC into driver dispatch and reply.

If the driver needs extra init args (endpoints, buffer phys):
- Extend `wasm_driver_manifest_t` `init_argc/init_argv`.
- Pass args from the kernel wrapper before `wasm_driver_start`.

## Step 5: Endpoint wiring

- Expose the service in `wasmos_endpoint_resolve` (kernel).
- If needed, pass endpoints via PM to apps (e.g. CLI).
- Ensure ownership permissions are correct (context-bound endpoints).

## Step 6: Bring-up checklist

1. Build: `cmake --build build --target run-qemu`
2. Confirm logs: `[wasm-driver] started` and driver-specific ready log
3. Run a minimal client (CLI or test module) to hit IPC paths
4. Remove debug logs before committing

## Pitfalls

- wasm3 native signatures must match exactly (e.g. `(iiii)i` vs `(*~)i`).
- Don’t use `*~` unless you want the runtime to translate pointers.
- Kernel IPC send requires the source endpoint to be owned by the caller’s context.
- For block drivers, only pass physical addresses the kernel can access.
