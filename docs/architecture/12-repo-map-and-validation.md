## Repository Map
- `src/boot/`: UEFI loader
- `src/kernel/`: kernel core, runtime hosting, scheduler, IPC, memory
- `src/drivers/`: WASM and native drivers
- `src/services/`: WASM services
- `lib/libc/`: shared user-space libc surface and language shims
- `examples/`: application examples and smoke apps
- `tests/`: QEMU-driven integration and regression tests

## Validation Baseline
Every architecture-affecting change is expected to keep these green:
- `cmake --build build --target run-qemu-test`
- `cmake --build build --target run-qemu-cli-test`
- `cmake --build build --target strict-ring3`

QEMU backend caveat:
- the CLI write smoke keeps truncate/append/create plus nested unlink/rmdir
  checks, but avoids one top-level grown-file unlink sequence that can trigger
  a known `vvfat` host assertion on some QEMU builds

The architecture is only considered stable when non-interactive boot, CLI
integration, and strict-ring3 gate checks all pass.
