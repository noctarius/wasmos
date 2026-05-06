# Current Task State (VT/TTY Split + Input Lockup Hardening)

## Summary
The current baseline uses split terminal ownership:
- `tty0` is the system console mirror (`serial_write` -> COM1 + shared ring).
- `tty1+` are VT-managed virtual terminals rendered by framebuffer IPC.

Recent work added per-tty shell instances and input ownership:
- process-manager assigns each CLI a home tty (`tty1..tty3`)
- `sysinit` now keeps one CLI instance running per VT-managed tty
- CLI gates keyboard reads by VT active-tty state (foreground tty only)

Lockup hardening remains in place:
- keyboard notify events use fire-and-forget IPC (`request_id = 0`)
- stale framebuffer backend publish call was removed from native framebuffer
  driver init
- VT->framebuffer and CLI->VT output loops use bounded retries on
  `IPC_ERR_FULL` and fail soft (drop output chunk) instead of spinning forever

## Current Behavior
- Kernel creates and owns a console ring shared-memory region.
- `serial_write` appends text bytes directly into that ring.
- Framebuffer native driver maps the ring and renders drained bytes.
- Early kernel log replay remains available and is replayed by framebuffer at init.
- Legacy serial→framebuffer text IPC path (`FBTEXT_IPC_PUT_CHAR_REQ` /
  `FBTEXT_IPC_PUT_STRING_REQ`) is removed.
- VT keeps per-tty state and switches framebuffer mode:
  - `tty0`: console-ring drain enabled (serial/system console view)
  - `tty1+`: console-ring drain disabled, VT cell replay active
- CLI instances are pinned one-per-tty on `tty1..tty3`; only the foreground
  tty's CLI consumes keyboard input and emits prompts.

## Tests (Last Run)
- `cmake --build build --target run-qemu-test` OK
- `cmake --build build --target run-qemu-test` stress loop x10 OK
- `cmake --build build --target run-qemu-test` stress loop x5 OK (post-fix)
- `cmake --build build --target run-qemu-cli-test` OK (28 tests)
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
- `docs/ARCHITECTURE.md`
- `docs/VIRTUAL_TERMINAL.md`

## Pending / Next Steps
- Define clear ownership/lifetime rules for shared-memory users (especially
  mixed native/WASM flows).
- Decide how WASM `shmem_unmap` should handle restoring overwritten linear pages.
- Extend coverage with explicit tty-switch behavior tests (`tty 1/2/3`) that
  verify shell state isolation across ttys.
- Deferred investigation: intermittent framebuffer-only prompt duplication /
  spacing artifact during rapid `Ctrl+Shift+Fn` switches. Not reproducing in
  recent runs; keep VT debug-mark traces as the primary future capture path.
- Extend VT toward richer ANSI semantics and line discipline.
- Keep docs (`README.md`, `docs/ARCHITECTURE.md`, `TASKS.md`, `docs/VIRTUAL_TERMINAL.md`)
  aligned as behavior evolves.
