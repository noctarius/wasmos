# Current Task State (Console Ring + Shared Memory Baseline)

## Summary
The current baseline has moved framebuffer text output away from serial-to-
framebuffer IPC text messages and onto a kernel-owned shared-memory console
ring.

`serial_write` now writes to COM1 and appends bytes to a 1-page shared ring.
The native framebuffer driver maps that ring and drains it in its main loop,
while still serving framebuffer control IPC (`FBTEXT_IPC_*`).

Shared-memory APIs are now available in both native and WASM paths:
- native driver ABI: `shmem_create`, `shmem_map`, `shmem_unmap`, `console_ring_id`
- WASM imports: `wasmos_shmem_create`, `wasmos_shmem_map`, `wasmos_shmem_unmap`

## Current Behavior
- Kernel creates and owns a console ring shared-memory region.
- `serial_write` appends text bytes directly into that ring.
- Framebuffer native driver maps the ring and renders drained bytes.
- Early kernel log replay remains available and is replayed by framebuffer at init.
- Legacy serial→framebuffer text IPC path (`FBTEXT_IPC_PUT_CHAR_REQ` /
  `FBTEXT_IPC_PUT_STRING_REQ`) is removed.
- VT service currently focuses on keyboard subscription/routing and escape
  filtering, and forwards output through `wasmos_console_write`.

## Tests (Last Run)
- `cmake --build build --target run-qemu-test` OK
- `ctest --test-dir build --output-on-failure` OK (no tests discovered)

## Key Files
- `src/kernel/serial.c`
- `src/kernel/memory.c`
- `src/kernel/native_driver.c`
- `src/kernel/wasm3_link.c`
- `src/kernel/include/console_ring.h`
- `src/drivers/framebuffer/framebuffer_native.c`
- `src/drivers/include/wasmos_native_driver.h`
- `src/drivers/include/wasmos_driver_abi.h`
- `src/services/vt/vt_main.c`
- `README.md`
- `ARCHITECTURE.md`
- `VIRTUAL_TERMINAL.md`

## Pending / Next Steps
- Define clear ownership/lifetime rules for shared-memory users (especially
  mixed native/WASM flows).
- Decide how WASM `shmem_unmap` should handle restoring overwritten linear pages.
- Extend VT toward multi-TTY, richer ANSI semantics, and line discipline.
- Keep docs (`README.md`, `ARCHITECTURE.md`, `TASKS.md`, `VIRTUAL_TERMINAL.md`)
  aligned as behavior evolves.
