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

## Kernel build — one source list (src/kernel/BUILD.py)
A kernel source appears once, in the list for the configuration that compiles it:
- `CORE` — every configuration; the list order is the link order (keep
  `arch/x86_64/entry.S` first, the image layout follows it).
- `WARP_GLUE` / `WARP_LIB` — WARP only (`kernel-warp` toolchain profile).
- `WASM3_GLUE_KERNEL` / `WASM3_GLUE` / `WASM3_SHIM` / `WASM3_LIB` — wasm3 only.
Each object is its own Ninja edge with a compiler depfile; the two-pass kallsyms
relink (`kernel.base.elf` → `scripts/gen_kernel_kallsyms.py` → `kernel.elf`) and
the embedded blobs (`b.exports["kernel.blobs"]`, `RING3_PROBES`) are derived from
the same manifest, so a new `.c` needs no other edit.

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

## IDE indexing (CLion / clangd) — compile_commands.json
- CLion/clangd + clang-tidy index a file only if it has an entry in
  `out/<dir>/compile_commands.json`, which the generator writes beside
  `build.ninja` on every regeneration.
- Every source compiled through a toolchain profile (`b.toolchain(...).compile`)
  gets an entry automatically. Guest C sources built by `guest()` are indexed
  against the in-tree headers the SDK sysroot is staged from; host unit-test
  sources are indexed from their `TESTS` entry in `tests/unit/BUILD.py`; the
  kernel indexes the other runtime's first-party glue so one lint pass covers
  both runtimes.
- A source compiled through a wrapper or a multi-source custom command
  (`b.command`) has no automatic entry; the manifest adds one with
  `b.index(source, [compiler, *flags])`, or the file shows up in CLion as "not in
  a project target." Verify coverage: a file's realpath appears in
  `out/<dir>/compile_commands.json`. See `skills/wasmos-ide-targets`.

## Host unit tests
- Live in `tests/unit/`, wired into the `run-kernel-unit-tests` target by the
  `TESTS` table in `tests/unit/BUILD.py`: a `(CC, name, args)` entry builds the
  binary (args carry `-I` paths such as `src/kernel/include` or
  `src/libsys/wasm/include` plus the sources), and a `(RUN, name, args)` entry runs
  it. `ninja -C out/<dir> run-kernel-unit-tests` runs every suite in order.
- Modules using `list_t` link `list.c list_linked.c list_array_chunk.c kmem.c` +
  `tests/unit/stubs_slab.c` (see `test_subsystem_registry` / `test_service_class_registry`).
- `-Isrc/kernel/include` shadows `<stdio.h>` with the kernel's; use `printf` (not
  `stderr`) in these tests.

## Runtimes and packaging
- Two WASM runtimes: wasm3 (default interpreter) and WARP
  (Kconfig `WASMOS_WASM_RUNTIME_WARP=y`, e.g. `configs/x86_64_warp_smp_defconfig`;
  single-pass JIT). Both run guests under
  **ring-3 isolation** (the standard execution model — selecting WARP implies
  ring-3; non-ring3 is unsupported). `libs/wasm3`/`libs/warp` are git subtrees —
  never edit them.
- WASM host calls (`wasmos.*` / `wasi` / `env` imports) are generated from
  `abi/hostcalls.yaml` via `scripts/gen_abi_hostcalls.py` (the `HC_*` enum, the
  WARP `WASMOS_SYMBOLS` table + ring-3 dispatch, the wasm3 link table, and the
  AOT symbol table all come from it); only the kernel wrapper bodies stay
  hand-written. The error/status vocabulary is generated from `abi/errors.yaml`.
  See `skills/wasmos-add-hostcall` and `docs/architecture/34-abi-idl-and-error-model.md`.
- A component is a native `.wap` ELF vs a WASM app per `native = true|false` in its
  `linker.metadata`. Native services enter at `initialize(...)`; WASM apps export
  `initialize` / `wasmos_main`. Startup values arrive via the spawn-info buffer, not
  entry args (`docs/architecture/09` / `13`).
- Build/run: `./configure out/<dir> configs/<name>_defconfig`, then
  `ninja -C out/<dir> <target>`; targets `bootloader`, `kernel`, `esp`,
  `run-qemu-test` (compile+boot+halt smoke), `run-kernel-unit-tests`. ESP
  staleness: Python tests boot `out/<dir>/esp/kernel.elf`, staged by the `esp`
  target (a dependency of every `run-*` target and of the default build) — run
  `ninja -C out/<dir>` before running a Python test directly. A test run outside
  the `run-*` targets reads `WASMOS_ESP` / `WASMOS_KERNEL` (default `build/esp`,
  `build/kernel.elf`, `scripts/qemu_test_framework.py`), so point them at
  `out/<dir>`.

## FS subsystem topology (for reference)
- `fs_manager` (WASM app, `src/services/fs_manager/`) is the VFS broker; backends
  (`fs-fat`, `fs-init`) attach to it. `fs-fat` consumes the ata `"block"` device.
- ata driver (`src/drivers/ata/ata.c`) registers name `"block"`.