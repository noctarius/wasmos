---
name: wasmos-kernel-internals
description: Authoritative locations and non-obvious wiring for WASMOS kernel/driver/service work — capabilities, the kernel build's parallel source lists, service discovery (name + class registries), IPC ABI, host unit-test wiring, and runtime/packaging facts. Read this BEFORE re-deriving where something lives, and consult docs/architecture/*.md (the architecture register) and each component's linker.metadata rather than reverse-engineering from code.
---

# WASMOS Kernel Internals — where facts live

Before changing kernel/driver/service/build code, read the relevant
`docs/architecture/NN-*.md` (the authoritative register) and the component's
`linker.metadata`. Do not re-derive these facts from code.

## Capabilities
- Declared per component in `src/<drivers|services>/<name>/linker.metadata` under
  `[[capabilities]] name = "..."` blocks. This is the source of truth for what a
  driver/service is granted at spawn — not the code.
- Names map to `CAP_*` kinds in `capability_grant_name()` (`src/kernel/capability.c`);
  the kind→bit map is `kind_to_mask()`; the enum is `src/kernel/include/capability.h`;
  the mapping table is documented in `docs/architecture/10-capability-and-policy.md`.
- Adding a capability requires all of: enum entry (`capability.h`), `kind_to_mask`
  case, `capability_grant_name` name string (`capability.c`), bump `CAP_ALL_MASK`
  (`capability.c`, kernel context gets all), the doc-10 mapping table, and a
  `[[capabilities]] name = "..."` block in each granted component's `linker.metadata`.
- Existing names: `io.port`, `irq.route`, `mmio.map`, `dma.buffer`,
  `system.control`, `subsystem.register`, `svc.class`, `ipc.basic`.

## Kernel build — THREE parallel lists (src/kernel/CMakeLists.txt)
Adding a kernel `.c` requires updating all three or the link fails:
1. `KERNEL_SOURCES` — dependency tracking (also drives the two-pass kallsyms relink).
2. `KERNEL_OBJS` — the object list linked into `kernel.elf`.
3. An explicit `COMMAND ${CLANG} ${CFLAGS_KERNEL} -c <src> -o <obj>` compile line.

## Service discovery
- Flat name table: `src/kernel/process_manager_services.c` (`g_pm.services`, a
  `list_t`). `svc_lookup(name) -> endpoint`.
- Class table (multi-provider): `src/kernel/service_class_registry.c` —
  `(class, instance) -> {endpoint, owner, pid}` + subscribers, self-contained
  (injected event/liveness callbacks), unit-tested by
  `tests/unit/test_service_class_registry.c`.
- IPC opcodes: `SVC_IPC_*` (0x220–0x2AF) in `src/drivers/include/wasmos_driver_abi.h`;
  dispatched in `src/kernel/process_manager.cpp`.
- Register/lookup wrappers: WASM/libc in `src/libc/include/wasmos/ipc.h`
  (`wasmos_svc_register[_class]`, `wasmos_svc_lookup[_class]`); native in
  `src/libsys/native/zig/libsys_native.c`. Keep both sides in sync (AGENTS rule).
- Design: `docs/architecture/09-process-and-ipc.md`.

## Host unit tests
- Live in `tests/unit/`, wired into the `run-kernel-unit-tests` target in the root
  `CMakeLists.txt` (one `COMMAND` per test: compile with `-I${KERNEL_DIR}/include`
  or `-I${LIBSYS_WASM_DIR}/include`, then run the binary).
- Modules using `list_t` link `list.c list_linked.c list_array_chunk.c kmem.c` +
  `tests/unit/stubs_slab.c` (see `test_subsystem_registry` / `test_service_class_registry`).
- `-Isrc/kernel/include` shadows `<stdio.h>` with the kernel's; use `printf` (not
  `stderr`) in these tests.

## Runtimes and packaging
- Two WASM runtimes: wasm3 (default interpreter) and WARP
  (`-DWASMOS_WASM_RUNTIME_WARP=ON`, single-pass JIT, ring 0). `libs/wasm3`/`libs/warp`
  are git subtrees — never edit them.
- A component is a native `.wap` ELF vs a WASM app per `native = true|false` in its
  `linker.metadata`. Native services enter at `initialize(...)`; WASM apps export
  `initialize` / `wasmos_main`. Startup values arrive via the spawn-info buffer, not
  entry args (`docs/architecture/09` / `13`).
- Build/run: `cmake -S . -B build`; targets `bootloader`, `kernel`, `run-qemu-test`
  (compile+boot+halt smoke), `run-kernel-unit-tests`. ESP staleness: Python tests boot
  `build/esp/kernel.elf`, staged by the `run-*` targets — rebuild via cmake or restage
  `build/kernel.elf` before running a Python test directly.

## FS subsystem topology (for reference)
- `fs_manager` (WASM app, `src/services/fs_manager/`) is the VFS broker; backends
  (`fs-fat`, `fs-init`) attach to it. `fs-fat` consumes the ata `"block"` device.
- ata driver (`src/drivers/ata/ata.c`) registers name `"block"`.