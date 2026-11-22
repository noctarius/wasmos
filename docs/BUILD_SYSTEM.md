# WASMOS Build System

`./configure` generates a Ninja build from Kconfig and per-directory `BUILD.py`
manifests. This document covers the build directory model, tool discovery,
configuration, the manifest API, every kind of artifact the build produces, the
run and test targets, and how to add a component.

---

## Contents

1. [Quick Reference](#1-quick-reference)
2. [Build Directory](#2-build-directory)
3. [Requirements and Tool Discovery](#3-requirements-and-tool-discovery)
4. [Configuration (Kconfig)](#4-configuration-kconfig)
5. [Layout](#5-layout)
6. [Toolchain Profiles](#6-toolchain-profiles)
7. [Manifest API](#7-manifest-api)
8. [Artifacts](#8-artifacts)
9. [Run and Test Targets](#9-run-and-test-targets)
10. [Quality Gates and the Compile Database](#10-quality-gates-and-the-compile-database)
11. [ESP Layout](#11-esp-layout)
12. [Adding a Component](#12-adding-a-component)

---

## 1. Quick Reference

```sh
./configure out/wasm3-smp configs/wasm3_smp_defconfig   # once per build directory
ninja -C out/wasm3-smp                    # everything: kernel, loader, modules, ESP, images
ninja -C out/wasm3-smp kernel             # one piece (bootloader, modules, esp, wasmos-sdk, ...)
ninja -C out/wasm3-smp run-qemu-test      # boot + halt gate
ninja -C out/wasm3-smp run-kernel-unit-tests
ninja -C out/wasm3-smp fmt-check lint     # quality gates
ninja -C out/wasm3-smp menuconfig         # edit out/wasm3-smp/.config
```

Without a directory, `configure` uses `out/default` (`./configure
configs/warp_smp_defconfig`), the directory the shared CLion run configurations
and `scripts/quality.sh` assume.

`scripts/run_config.sh <config> [target]` configures `out/<config>` from
`configs/<config>_defconfig` on first use and builds the target.

---

## 2. Build Directory

A build directory is `.config` plus `build.ninja`. `configure` seeds `.config`
once from the given defconfig (`configs/wasmos_defconfig` by default) and never
re-reads the source tree's copy; `--reseed` replaces it, and `--set
SYMBOL=VALUE` (repeatable; `y`/`n` for bools) overrides individual symbols.
kconfiglib resolves the file against `Kconfig` on every generation, so defaults
and dependencies (for example `WASMOS_SMP` requiring `WASMOS_IRQ_IOAPIC`) apply
without any build-system logic, and the resolved file is written back.

`build.ninja` regenerates itself when `.config`, a Kconfig file, a `BUILD.py`
manifest, a directory a manifest globbed, `configure` or
`scripts/buildgen/*.py` changes. Re-running `configure` is needed only to change
tool paths.

One build directory holds one configuration. Two configurations (wasm3 and
WARP, single and SMP) are two directories; nothing in one can influence the
other.

Every compiled object is its own build edge with a compiler depfile, so header
dependencies are exact and compiles run in parallel. Final artifacts sit at the
build-directory root (`kernel.elf`, `BOOTX64.EFI`, `<module>.wasm`,
`<module>.wap`, `initfs.img`, `wfs.img`, ...); objects under
`obj/<profile>/<source path relative to the repo>.o`, so equal basenames in
different directories never collide.

---

## 3. Requirements and Tool Discovery

`configure` resolves every external tool once and records it in
`<dir>/buildgen-tools.json`; regeneration reuses that record, so a build
directory keeps the toolchain it was configured with.

| Tool | Purpose | Override |
|------|---------|----------|
| `clang`, `clang++` (LLVM, not AppleClang) | UEFI, kernel, native, wasm32 and host compiles | `--clang=`, `--clangxx=` |
| `ld.lld`, `lld-link` | kernel/native ELF links; UEFI PE/COFF link | `--ld.lld=`, `--lld-link=` |
| `llvm-objcopy`, `llvm-nm`, `llvm-ar` | blobs, kallsyms, archives | `--llvm-objcopy=` etc. |
| `wasm-ld` | recorded for the SDK's `wasmos-ld` face | `--wasm-ld=` |
| Python 3 + `kconfiglib` | the generator, Kconfig, staging scripts, test runners | run `configure` with that Python |
| `ninja` 1.10+ | executes the build | on `PATH` |
| `zig`, `asc`, `rustc`, `tinygo` + `wasm-opt` | optional guest languages | `--zig=`, `--asc=`, `--rustc=`, `--tinygo=`, `--wasm-opt=` |
| `qemu-system-x86_64`, OVMF firmware | x86_64 run and test targets | `--qemu=`, `--ovmf-code=`, `--ovmf-vars=` |
| `qemu-system-aarch64`, edk2 aarch64 firmware | aarch64 run and test targets | `--qemu-aarch64=`, `--aavmf-code=`, `--aavmf-vars=` |
| `clang-format`, `clang-tidy`, `llvm-config`, `black`, `ruff`, `gofmt`, `go`, `rustfmt` | quality gates | `CLANG_FORMAT` pins the formatter |

AppleClang is rejected. On macOS: `brew install llvm lld qemu ninja` and
`python3 -m pip install kconfiglib`. OVMF candidates are searched in the common
Homebrew and distribution paths; an i386 vars store beside x86_64 firmware is
dropped because it hangs the boot. No aarch64 vars store is discovered, so edk2
keeps its variables in RAM unless `--aavmf-vars=` names one. `WASMOS_CLANG_TIDY_INCLUDE_DIR` (or
`--clang-tidy-include=`) supplies `clang-tidy/*.h` when the LLVM install lacks
them.

---

## 4. Configuration (Kconfig)

Every build option is a Kconfig symbol (`Kconfig` at the repository root). The
tracked defconfigs in `configs/` pin the target architecture, the WASM runtime
and the CPU topology.

| Symbol | Default | Effect |
|--------|---------|--------|
| `WASMOS_ARCH_X86_64` / `_AARCH64` | `X86_64` | target architecture: compiler triples, code model, boot file name, QEMU machine and firmware (`ARCHES` in `toolchains.py`). aarch64 is under bring-up: its kernel runs wasm3 guests and stops at the first unimplemented interface; the IRQ-mode choice and WARP are x86_64-only |
| `WASMOS_WASM_RUNTIME_WASM3` / `_WARP` | choice | kernel runtime; WARP adds the WARP sources, `warp_aot` and AOT packing |
| `WASMOS_IRQ_PIC` / `_LAPIC` / `_IOAPIC` | `IOAPIC` | `-DWASMOS_IRQ_MODE=0/1/2` |
| `WASMOS_SMP` | `n` (depends on `IOAPIC`) | `-DWASMOS_SMP`; QEMU runs with 4 CPUs |
| `WASMOS_SCHED_CALLER_CPU_BIAS` | `y` | `-DWASMOS_SCHED_CALLER_CPU_BIAS` |
| `WASMOS_SCHED_SMP_STRESS` | `n` | `-DWASMOS_SCHED_SMP_STRESS=1` (for `run-qemu-sched-stress-test`) |
| `WASMOS_WARP_RUNTIME_LOAD_AOT` | `y` | load bundled AOT binaries at runtime |
| `WASMOS_TRACE` | `n` | `-DWASMOS_TRACE` for kernel and guests; gfx-compositor's `gfx_trace` |
| `WASMOS_RING3_SMOKE`, `WASMOS_RING3_THREAD_LIFECYCLE_SMOKE` | `n` | ring-3 probe spawn defaults |
| `WASMOS_PM_TEST_HOOKS` | `n` | process-manager test injection hooks |
| `WASMOS_PM_LIST_LINKED` / `_ARRAY_CHUNK`, `WASMOS_PM_LIST_ARRAY_CHUNK_CAP` | linked, 16 | PM list backend |
| `AS_ENABLE`, `RUST_ENABLE`, `GO_ENABLE`, `ZIG_ENABLE` | `y` | build that language's modules (also needs the tool) |
| `QEMU_GDB_PORT` | 1234 | `run-qemu-debug` |

The mapping from symbols to compile flags lives in one place,
`scripts/buildgen/toolchains.py`.

---

## 5. Layout

| Path | Role |
|------|------|
| `configure` | Entry point: seed `.config`, discover tools, generate |
| `scripts/buildgen/buildgen.py` | Project-agnostic core: Kconfig view, ninja writer, manifest API |
| `scripts/buildgen/toolchains.py` | Tool discovery and named toolchain profiles |
| `scripts/buildgen/wasmos_rules.py` | WASMOS helpers: guest and native modules, packing/AOT, ESP install, kernel blobs |
| `BUILD.py` (root) | Host packer, `[deps]` check, `b.subdir(...)` in dependency order, quality targets |
| `<component>/BUILD.py` | What one component builds |
| `scripts/sdk/BUILD.py`, `scripts/sdk_stage.py` | The staged SDK |
| `scripts/system/BUILD.py`, `scripts/stage_esp.py` | initfs, ESP, disk images, `run-qemu*` targets (loaded after every component) |
| `tests/unit/BUILD.py` | Host unit tests |
| `tools/clang_tidy_wasmos/BUILD.py` | The wasmos clang-tidy plugin |

Exports flow downward: a manifest reads what an earlier one published in
`b.exports`, so the root manifest's `subdir` order is the dependency order.

---

## 6. Toolchain Profiles

A profile is one target class: compiler, the flags every object of that class
shares, and linker. Manifests select one with `b.toolchain(name)` and add only
component-specific flags.

| Profile | Target | Used for |
|---------|--------|----------|
| `efi` | `x86_64-unknown-uefi` / `aarch64-unknown-windows`, lld-link | `BOOTX64.EFI` / `BOOTAA64.EFI` |
| `kernel` | `x86_64-elf` / `aarch64-elf`, ld.lld | kernel C/C++/asm |
| `kernel-warp` | as `kernel`, C++14 with exceptions and RTTI | WARP glue and vendored WARP |
| `probe` | `<arch>-unknown-none-elf` | ring-3 native probe blobs |
| `native` | `x86_64-elf` / `aarch64-elf`, flags per module | native ring-3 drivers and services |
| `wasm32` | `wasm32-unknown-unknown` | sysroot libc/libsys |
| `host` | host clang | `make_wasmos_app`, `mkfs_wfs`, `warp_aot` |

Every per-architecture value outside the sources is one `Arch` entry in
`ARCHES` (`toolchains.py`): triples, code-model flags, the ELF format of kernel
blobs, the Zig target, the native coroutine assembly, the boot file name, and
the QEMU binary, machine and firmware keys. Profiles, manifests
(`toolchains.arch(b.cfg)`) and the QEMU harness read it, so a new architecture
is a new entry there rather than an edit to every manifest. clang has no
aarch64 UEFI triple; the Windows triple emits the same PE/COFF with AAPCS64. `b.toolchain(name, overrides={...})` evaluates the
profiles as if some Kconfig symbols had other values; the kernel uses it to
index the other runtime's glue.

---

## 7. Manifest API

A manifest defines `build(b)`. The `Builder` it receives is scoped to the
manifest's directory (`scripts/buildgen/buildgen.py`):

- `b.cfg.SYMBOL` — the resolved Kconfig value (bool, int or str);
  `b.cfg.flag(name)` gives a bool as `0`/`1` for `-DNAME=` defines.
- `b.src(path)` / `b.srcs(*paths)` — absolute source paths; `b.glob(pattern,
  root=...)` globs at generation time (`**` recurses) and makes every matched
  directory a regeneration input.
- `b.toolchain(name).compile(sources, flags=..., cflags=..., cxxflags=...,
  subdir=...)`, `.archive(out, objs)`, `.link(out, objs, flags=...)`.
- `b.command(outputs, argv, inputs=..., implicit=..., cwd=..., depfile=...)` —
  a custom step (an argv, or a list of argvs run in sequence); runs in the build
  directory unless `cwd` is given.
- `b.run_target(name, argv, deps=..., env=..., cwd=...)` — an always-run target
  with the terminal attached (QEMU sessions, test runners).
- `b.write_file(path, text)` — a generated input written at generation time,
  touched only when its content changes.
- `b.index(source, argv)` — a `compile_commands.json` entry without a build edge
  (Section 10).
- `b.alias(name, *targets, default=False)` — a phony target; `b.exports` — the
  shared namespace; `b.subdir(*dirs)` — load child manifests.

`scripts/buildgen/wasmos_rules.py` adds the WASMOS-specific steps:

- `guest(b, name, sources, manifest=None, lang="c", flags=(), wasm=None,
  cxx=False)` — build a module with the staged SDK driver for its language
  (`c`, `zig`, `as`, `rust`, `go`) and pack it into `<name>.wap`; `manifest`
  defaults to the directory's `linker.metadata`. C sources compile one object
  each with a depfile and link in the listed order, which is part of the image.
  A language whose Kconfig switch is off or whose tool is missing returns
  `None` and builds nothing.
- `pack(b, name, image, manifest)` — under WARP, a wasm module whose manifest
  says `aot = true` is compiled by `warp_aot` and bundled with its `.warpbin`.
- `native(b, name, objs, entry, image_base=None, elf=None)` and
  `zig_object(b, out, root, includes, modules)` — native ring-3 modules: static
  ELF, packed without AOT.
- `asc_tool(b, name, source, wasm)` — a single-file AssemblyScript tool built
  with `asc` directly (`date`, `sched_info`).
- `install(b, dest, path)` — stage a file at `dest` on the ESP (`None` stages
  nothing); `kernel_blob(b, payload)` — wrap a build output as an ELF object
  for the kernel link.

---

## 8. Artifacts

**Kernel** (`src/kernel/BUILD.py`): `CORE` plus the runtime's sources (wasm3
glue and vendored wasm3, or WARP glue and vendored WARP), embedded blobs
(`chardev_client.wasm` and the ring-3 native probes), linked twice: the first
link's symbols feed `scripts/gen_kernel_kallsyms.py`, whose output links into
the final `kernel.elf`. The source lists are in link order; the image layout
follows it.

**Bootloader** (`src/boot/BUILD.py`): `boot.c` compiled with the `efi` profile
and linked by lld-link into `BOOTX64.EFI` (`BOOTAA64.EFI` on aarch64).

**SDK** (`src/libc/BUILD.py`, `src/libsys/BUILD.py`, `scripts/sdk/BUILD.py`):
`libc.a`, `libsys.a` and `crt1.o` in
`wasmos-sdk/sysroot/lib/wasm32-unknown-wasmos`, then `scripts/sdk_stage.py`
stages headers, the `wasmos-*` drivers, libexec, `share/` (including the CMake
toolchain file for out-of-tree app builds) and `wasmos-sdk.conf`
(`ninja -C <dir> wasmos-sdk`). Every guest module depends on the staged SDK.
`docs/toolchain.md` documents the SDK.

**Guest modules** (`guest()`): drivers, services, utilities and examples in C,
Zig, AssemblyScript, Rust and Go. Rust and TinyGo embed the path their driver
staged sources at (beside the `-o` output), so those modules are not
byte-identical between two build directories; every other artifact is.

**Native modules** (`native()`): framebuffer and framebuffer-pci drivers (C),
gfx-compositor and font-service (Zig + C), net-stack (C with vendored lwIP and
mbedTLS).

**Host tools**: `make_wasmos_app` (the packer), `mkfs_wfs` (WFS formatter,
built from the driver's own layout sources), `warp_aot` (WARP trees only).

**System image** (`scripts/system/BUILD.py`): `initfs.img` and
`initfs_config.bin` from `scripts/initfs.toml` (payloads are found by name in
the build directory), `esp/` (Section 11), `wfs.img` and `wfs_virtio.img`
(`mkfs_wfs --populate scripts/wfs`), `user.img` (`make_gpt_image.py` over
`userfs/`), and `run/` copies of the three images that interactive sessions
boot, so a session never mutates the fixtures the tests expect.

**Checks**: `deps-check.stamp` runs `make_wasmos_app --check-deps` over every
`linker.metadata`; a cycle in `[deps]` fails the build.

Default targets: `kernel`, `bootloader`, `modules` (every packed module),
`esp`, `images`, `check-deps`.

---

## 9. Run and Test Targets

Every run target passes the tree's own paths to the harness (`WASMOS_ESP`,
`WASMOS_KERNEL`, `WASMOS_USERFS_IMAGE`, `WASMOS_WFS_IMAGE`,
`WASMOS_WFS_VIRTIO_IMAGE`, `WASMOS_OVMF_CODE`, `WASMOS_OVMF_VARS`,
`WASMOS_QEMU_SMP_COUNT`, `WASMOS_ARCH`, `WASMOS_QEMU`), so a suite tests the
tree that launched it, on that tree's QEMU machine. Without
`WASMOS_QEMU_ACCEL` the harness picks KVM for a host-native guest with
`/dev/kvm`, HVF for an aarch64 guest on Apple Silicon, and TCG otherwise; the
interactive `run-qemu*` targets make the same choice when the tree is
generated. QEMU
targets share the tree's `esp/`; never run two at once.

| Target | What it runs |
|--------|--------------|
| `run-qemu` / `run-qemu-ui` | interactive boot, serial / display, on the `run/` image copies; COM2 to `syslog.log` |
| `run-qemu-debug` | as `run-qemu`, paused with a gdb stub on `QEMU_GDB_PORT` |
| `run-qemu-test` | `scripts/qemu_halt_test.py`: boot to the CLI and halt (the pre-commit gate); `WASMOS_QEMU_TEST_TIMEOUT` sets its budget (default 120 s) |
| `run-qemu-bringup-test` | `scripts/qemu_bringup_test.py`: boot until `[test] preempt ok` appears (MMU, scheduler, timer interrupt and preemption all working), tolerating a later panic; the gate for an architecture that cannot reach the CLI yet (valid on any tree) |
| `run-qemu-cli-test` | `scripts/run_unittest_suite.py --start-dir tests` with an isolated ESP per session; `WASMOS_TEST_BATTERY` selects one battery of `tests/batteries.json` |
| `run-qemu-ui-test` | `scripts/qemu_ui_test.py` |
| `run-qemu-sched-stress-test` | `scripts/qemu_sched_stress_test.py`; needs `WASMOS_SCHED_SMP_STRESS=y` |
| `run-qemu-ring3-check`, `-ring3-threading-check`, `-ring3-fault-storm-check` | ring-3 marker tests in a tree configured with the probes |
| `run-qemu-ring3-test`, `-ring3-threading-test`, `-ring3-fault-storm-test` | configure `<dir>/ring3*` (wasm3, `WASMOS_RING3_SMOKE`, `WASMOS_PM_TEST_HOOKS`, the parent's `WASMOS_TRACE`) and run its check |
| `strict-ring3` | `run-qemu-test` then `run-qemu-ring3-test` |
| `run-kernel-unit-tests` | every host unit suite (`tests/unit/BUILD.py`), in order, from the build directory |
| `run-fat-image-tests` | `scripts/run_fat_image_test.sh` against formatter-built FAT images (skips without a formatter) |

Host unit tests: C/C++ test binaries are build edges (`unit-test-binaries`),
so they compile in parallel and rebuild only when an input changes; the run
target executes them, then the AssemblyScript, Go, Rust and Zig suites, and
stops at the first failure. Sanitizer builds use the platform SDK's clang on
macOS. The WARP host coroutine test builds on x86_64 hosts of a WARP tree.

---

## 10. Quality Gates and the Compile Database

`fmt`, `fmt-check`, `lint` and `quality` run `scripts/quality.sh` with the
tools the tree resolved and `QUALITY_BUILD_DIR` set to the tree; `lint` builds
the wasmos clang-tidy plugin first. Run directly, the script reads the same tool
paths from `$QUALITY_BUILD_DIR/buildgen-tools.json` (default `out/default`).

`compile_commands.json` carries every first-party source with the flags of the
build step that compiles it. Real compile edges record themselves; `b.index`
adds entries where the real compile is not a plain clang command a tool can
replay or happens only in another configuration: guest C sources (indexed with
the `wasm32` flags against the in-tree headers), C sources a Zig/Rust/Go driver
compiles, each source of a multi-source unit test (with that test's flags), the
other runtime's kernel glue (with that runtime's flags), libsys entry shims, the
SDK hello app, `warp_aot` in a wasm3 tree, the WARP host test and the plugin.
`skills/wasmos-ide-targets` covers auditing and fixing coverage.

### IDE Entry Point

`configure` maintains `<srcdir>/compile_commands.json` (gitignored), a symlink
to one build directory's database, so CLion and clangd find it at the source
root. It is created when missing or dangling and otherwise left alone, so the
first configured directory keeps it; `--ide-link` re-points it at the directory
being configured, `--no-ide-link` leaves it untouched (the ring-3 shadow trees
pass it), and a regular file at that path is never replaced. The index follows
that directory's `.config`: `#if` branches and the runtime-specific kernel glue
resolve as that tree builds them.

CLion opens the source root as a compilation-database project; a project last
opened when the tree used CMake still carries CMake state in `.idea/` and fails
with `CMakeLists.txt not found`. Closing the project, removing `.idea/` and
reopening the directory loads the symlinked database instead. Enabling
auto-reload (Settings → Build, Execution, Deployment → Compilation Database)
follows its regeneration.

A compilation-database project has no build targets of its own. `.run/` holds
shared run configurations against `out/default` (CLion's *wasmos* folder):
shell configurations for the build, `run-qemu`, `run-qemu-test`,
`run-kernel-unit-tests`, `fmt-check` + `lint` and the paused `run-qemu-debug`, plus a GDB
remote-debug configuration that attaches to `localhost:1234` with
`out/default/kernel.elf` symbols once `run-qemu-debug` is waiting.

---

## 11. ESP Layout

`scripts/system/BUILD.py` writes `esp-plan.json` (every `install()` destination
plus the fixed files below) and `scripts/stage_esp.py` populates `esp/` from it,
copying only files whose content changed. Entries outside the plan are left
alone: QEMU's `fat:rw:` backend writes `NvVars`, and tests write fixtures.

```
<dir>/esp/
├── EFI/BOOT/BOOTX64.EFI       # UEFI bootloader
├── kernel.elf                 # kernel
├── kernel.cmdline             # kernel command line (scripts/system/kernel.cmdline)
├── initfs.img                 # boot-phase drivers and services
├── startup.nsh                # UEFI shell startup script
├── large_read.txt, write_smoke.txt   # test fixtures
├── apps/                      # applications and smoke-test apps
└── system/
    ├── drivers/               # hardware drivers loaded by device-manager
    ├── services/              # system services
    ├── utils/                 # /system/utils tools
    ├── devmgr/rules/          # device-manager policy (scripts/system/devmgr/rules)
    ├── keymaps/               # scripts/system/keymaps
    ├── fonts/                 # TTF fonts for the font service
    ├── net/interfaces, net/certificates/ca-certs.pem
    └── sysinit.rc
```

`initfs.img` holds the drivers and services needed before `/boot` is mounted
(`scripts/initfs.toml`); hardware drivers live on the ESP and are loaded by
device-manager afterwards. The TLS trust store is fetched on demand
(`scripts/fetch-ca-certs.sh`); without it the ESP carries an empty placeholder
and the build stays offline.

---

## 12. Adding a Component

A component is a `BUILD.py` in its directory plus that directory in the
parent's `b.subdir(...)`:

```python
"""ata: driver guest module."""

from wasmos_rules import guest, install


def build(b):
    wap = guest(b, "ata", [b.src("ata.c")])
    install(b, "system/drivers/ata.wap", wap)
```

- **Multi-file module:** list every source, in link order, in `guest()`.
  Sources outside the directory are `b.root / "path"`; extra include
  directories go in `flags=[f"-I{...}"]`.
- **Async service or driver:** list `src/libsys/wasm/service_async_entry_wasm.c`
  first (see `src/services/logger/BUILD.py`).
- **Zig, AssemblyScript, Rust, Go:** `guest(..., lang="zig")` etc., listing the
  module's own sources and the `.c` shims it links
  (`src/drivers/virtio_blk/BUILD.py`).
- **Native ring-3 module:** compile with `b.toolchain("native")` and link with
  `native()` (`src/drivers/framebuffer/BUILD.py`,
  `src/services/gfx_compositor/BUILD.py`).
- **Boot-phase module:** name its `.wap` in `scripts/initfs.toml` instead of
  installing it; initfs finds it by name in the build directory.
- **Host unit test:** add an entry pair to `TESTS` in `tests/unit/BUILD.py` —
  `(CC, "test_x", [*C11, ..., sources])` and `(RUN, "test_x", [])` — see
  `skills/wasmos-regression-test`.

The next `ninja` regenerates the build, compiles the module, and stages it.
