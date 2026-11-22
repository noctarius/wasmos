---
name: wasmos-build-and-run
description: Configure, build and run WASMOS QEMU/test targets with the right runtime (wasm3 vs WARP) via configs/*_defconfig and a build directory's .config. Explains the one-build-dir-per-config rule, how a build directory is seeded and edited, how to tell what a tree is configured as and what it actually booted, and the full run-target list. Use before running run-qemu-test / run-qemu-cli-test / run-kernel-unit-tests or when a build boots the wrong runtime.
---

# WASMOS: Build & Run Targets (runtime selection)

## Why this exists

The WASM runtime (wasm3 interpreter vs WARP JIT/AOT) is a Kconfig choice
(`WASMOS_WASM_RUNTIME_WASM3` / `WASMOS_WASM_RUNTIME_WARP`) baked into a build
directory's `.config`. WARP is the Kconfig default because its guests run at
CPL=3 and are preempted like any other thread, while the wasm3 interpreter --
the only runtime component at CPL=0 -- executes its guest in a kernel-mode
frame, so a wasm3 guest is never timer-preempted (see `docs/architecture/11`).

`./configure <dir> <defconfig>` copies the defconfig to `<dir>/.config` once.
After that the directory's `.config` is the only input: re-running `configure`
with a different defconfig keeps the existing file (it says so) unless
`--reseed` is given. The tracked defconfigs `configs/{wasm3,warp}_{single,smp}_defconfig`
are the source-of-truth configurations CI's matrix uses.

## The rule: one build dir per configuration

Give each configuration its own directory, mirroring CI:

```sh
# wasm3 (interpreter)
./configure out/wasm3-smp configs/wasm3_smp_defconfig
ninja -C out/wasm3-smp run-qemu-test

# WARP (JIT/AOT)
./configure out/warp-smp configs/warp_smp_defconfig
ninja -C out/warp-smp run-qemu-test
```

`*_single` vs `*_smp` selects UP vs SMP (SMP is where scheduler/IPC races
surface — prefer it for boot smokes). `scripts/run_config.sh <config> [target]`
does both steps with `out/<config>`.

## Which runtime is a build dir?

```sh
grep RUNTIME <dir>/.config     # CONFIG_WASMOS_WASM_RUNTIME_WARP=y → WARP
```
At boot the log also tells you: `[warp-driver] ... using AOT binary` ⇒ WARP; no
AOT/`warp-driver` lines ⇒ wasm3.

The load-bearing marker is `[subsystem] register request=WASM runtime=WASM3|WARP`.
Prefer it over the driver prefixes: it names the runtime the kernel actually
registered, so it distinguishes a mis-configured tree from a correctly
configured one that booted somebody else's kernel image.

`.config` describes what was CONFIGURED; the ESP is what was BOOTED. Compare
them directly when it matters:

```sh
nm <dir>/kernel.elf     | grep -c warp_   # the tree's own kernel
nm <dir>/esp/kernel.elf | grep -c warp_   # what actually boots
```
The run targets pass `WASMOS_ESP` and `WASMOS_KERNEL` for their own tree to the
harness, so a suite started through a target tests that tree; the two-`nm`
check is how you prove it.

## Which architecture is a build dir?

```sh
grep ARCH <dir>/.config     # CONFIG_WASMOS_ARCH_AARCH64=y → aarch64
```
The architecture is a Kconfig choice like the runtime. It selects the compiler
triples, `BOOTX64.EFI` vs `BOOTAA64.EFI`, and the QEMU machine every run target
and the harness boot (`qemu-system-aarch64 -M virt` + edk2 aarch64 firmware).
All of it comes from `ARCHES` in `scripts/buildgen/toolchains.py`.
`configs/aarch64_wasm3_single_defconfig` is the arm64 bring-up tree. Its kernel
schedules, preempts and runs wasm3 guests, then panics by name at the first
unimplemented arch interface (today PCI configuration access), so
its gate is `run-qemu-bringup-test` (boot to `[test] preempt ok`),
not `run-qemu-test`. An aarch64 guest runs under HVF on Apple Silicon, KVM on
an aarch64 Linux host that has it, TCG elsewhere. aarch64 has no COM2:
the kernel log is the virtio console.

## Editing a configuration

- `ninja -C <dir> menuconfig` — interactive editor on `<dir>/.config`.
- `./configure <dir> --set SYMBOL=VALUE` — set one symbol (`y`/`n` for bools),
  e.g. `--set WASMOS_SCHED_SMP_STRESS=y`.
- `./configure <dir> configs/<name>_defconfig --reseed` — back to a defconfig.
- Editing `<dir>/.config` by hand works too.

kconfiglib resolves `.config` against `Kconfig` on every regeneration, so a
symbol whose dependency is unmet (SMP without IOAPIC) is dropped rather than
half-applied. The next `ninja` regenerates and rebuilds what the change touches.

## Run / test targets

Host, no QEMU:
- `run-kernel-unit-tests` — host-native unit tests (`tests/unit/BUILD.py`).
  Runtime-independent; any tree works (CI uses `./configure out/unit`).
- `run-fat-image-tests` — fs_fat against formatter-built FAT images.

QEMU (need OVMF; **never run two in parallel** — they share `<dir>/esp/`):
- `run-qemu-test` — compile + boot + halt smoke (the default pre-commit check).
- `run-qemu-cli-test` — full CLI integration suite;
  `WASMOS_TEST_BATTERY=<name>` runs one battery of `tests/batteries.json`.
- `run-qemu-ui-test` — graphics UI smoke.
- `run-qemu` / `run-qemu-ui` — interactive serial / graphics boot.
- `run-qemu-debug` — paused QEMU + GDB stub on `QEMU_GDB_PORT` (default 1234).
- `run-qemu-sched-stress-test` — SMP scheduler stress; needs
  `WASMOS_SCHED_SMP_STRESS=y`.
- `run-qemu-ring3-test` / `-ring3-threading-test` / `-ring3-fault-storm-test` —
  ring-3 marker tests. Each configures its **own tree** under `<dir>/ring3*/`
  (wasm3, `WASMOS_RING3_SMOKE`, `WASMOS_PM_TEST_HOOKS`, the parent's
  `WASMOS_TRACE`) and runs its `run-qemu-ring3*-check` there.
- `strict-ring3` — `run-qemu-test` + `run-qemu-ring3-test` in sequence.

## Pre-commit checklist for a runtime-affecting change

Mirror the CI matrix — boot **both** runtimes, then the unit tests:
```sh
./configure out/wasm3-smp configs/wasm3_smp_defconfig && ninja -C out/wasm3-smp run-qemu-test
./configure out/warp-smp  configs/warp_smp_defconfig  && ninja -C out/warp-smp  run-qemu-test
ninja -C out/warp-smp run-kernel-unit-tests
```
A client-side or docs-only change that cannot differ per runtime can be gated on
one runtime; state which you ran.

## Gotchas

- WARP boot smokes are timing-sensitive and can **flake** (a near-complete boot
  that misses `halt`/`[calculator] ready` before the timeout). Re-run once on the
  same dir before treating a single failure as a regression; capture the **full**
  log (don't pipe through `tail`, which truncates the QEMU output).
- CI forces software emulation: `WASMOS_QEMU_ACCEL=tcg` (wasmos is only
  validated under TCG; KVM breaks the bootloader→kernel handoff), and
  `WASMOS_QEMU_TEST_TIMEOUT` sets `run-qemu-test`'s budget. Set these to
  reproduce CI locally.
- ESP staleness: the Python runners boot `<dir>/esp/kernel.elf`, staged by the
  build. Run `ninja -C <dir>` before running a Python test directly, and point
  it at the tree with `WASMOS_ESP` / `WASMOS_KERNEL` (the targets set both).
- Use LLVM `clang`, not AppleClang: `./configure <dir> ... --clang=/opt/homebrew/opt/llvm/bin/clang`.
