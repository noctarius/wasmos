---
name: wasmos-build-and-run
description: Build and run WASMOS QEMU/test targets correctly with respect to runtime selection (wasm3 vs WARP) via Kconfig .config / configs/*_defconfig. Explains how the runtime is pinned at configure time, why a warm build dir or a leftover local .config silently overrides -D flags, the one-build-dir-per-config rule, how to tell what a build dir is configured as, and the full run-target list. Use before running run-qemu-test / run-qemu-cli-test / run-kernel-unit-tests or when a build boots the wrong runtime.
---

# WASMOS: Build & Run Targets (runtime selection)

## Why this exists

The WASM runtime (wasm3 interpreter vs WARP JIT/AOT) is chosen **at configure
time** and baked into a build directory. Three things override each other, which
causes "why did my build boot WARP when I asked for wasm3?" confusion:

1. **Code default** (fresh cache, no `.config`): wasm3. A one-time guard
   (`WASMOS_WASM_RUNTIME_WASM3_INITIALIZED`) sets `WASM3=ON`/`WARP=OFF` on the
   first configure of a new build dir.
2. **`WASMOS_DOTCONFIG`** (default `<repo>/.config`, which is **gitignored /
   local**): if that file exists it is imported every configure and **FORCE**s
   the runtime — so a leftover local `.config` silently overrides a
   `-DWASMOS_WASM_RUNTIME_WARP=...` on the command line.
3. **The tracked defconfigs** `configs/{wasm3,warp}_{single,smp}_defconfig`
   (plus `configs/wasmos_defconfig`): the source-of-truth configs. Select one
   with `-DWASMOS_DOTCONFIG=configs/<name>_defconfig`. This is what CI's matrix
   uses.

Consequence: **`-DWASMOS_WASM_RUNTIME_WARP=ON/OFF` alone is unreliable** across a
warm cache or a leftover `.config`. Drive the runtime with a **defconfig on a
fresh build dir** instead.

## The rule: one build dir per configuration

Do **not** reconfigure an existing build dir to flip runtimes — a warm cache and
any local `.config` fight the new `-D`, and the dir can stay on the old runtime.
Use a dedicated dir per config, mirroring CI:

```sh
# wasm3 (interpreter)
cmake -S . -B build-wasm3 -DWASMOS_DOTCONFIG=configs/wasm3_smp_defconfig
cmake --build build-wasm3 --target run-qemu-test

# WARP (JIT/AOT)
cmake -S . -B build-warp  -DWASMOS_DOTCONFIG=configs/warp_smp_defconfig
cmake --build build-warp  --target run-qemu-test
```

`*_single` vs `*_smp` selects UP vs SMP (SMP is where scheduler/IPC races
surface — prefer it for boot smokes). `build/` is just your personal working
dir; treat its runtime as "whatever it was last configured as", not a default.

## Which runtime is a build dir? 

```sh
grep WASMOS_WASM_RUNTIME_WARP: <dir>/CMakeCache.txt   # :BOOL=ON → WARP, OFF → wasm3
```
At boot the log also tells you: `[warp-driver] ... using AOT binary` ⇒ WARP; no
AOT/`warp-driver` lines ⇒ wasm3. If a dir is not the runtime you expect, look for
a stray `<repo>/.config` (it is local + gitignored) — that is usually the cause.

## Run / test targets

Host, no QEMU:
- `run-kernel-unit-tests` — host-native unit tests. Runtime-independent; CI runs
  it under a plain `cmake -S . -B build`.

QEMU (need OVMF; **never run two in parallel** — they share `build/esp/`):
- `run-qemu-test` — compile + boot + halt smoke (the default pre-commit check).
- `run-qemu-cli-test` — full CLI integration suite.
- `run-qemu-ui-test` — graphics UI smoke.
- `run-qemu` / `run-qemu-ui` — interactive serial / graphics boot.
- `run-qemu-debug` — paused QEMU + GDB stub on `QEMU_GDB_PORT` (default 1234).
- `run-qemu-ring3-test` / `-ring3-threading-test` / `-ring3-fault-storm-test` —
  ring-3 marker tests. These build their **own shadow tree** under
  `build/ring3*/` and **always use wasm3** regardless of the main runtime.
- `strict-ring3` — `run-qemu-test` + `run-qemu-ring3-test` in sequence.

## Kconfig frontend flow (optional)

For interactive editing rather than a defconfig (README):
- `cmake --build <dir> --target kconfig-defconfig` — seed `<dir>/.config` from
  `configs/wasmos_defconfig`.
- `cmake --build <dir> --target kconfiglib-menuconfig` — edit it (needs
  `python3 -m pip install kconfiglib`).

## Pre-commit checklist for a runtime-affecting change

Mirror the CI matrix — boot **both** runtimes, then the unit tests:
```sh
cmake -S . -B build-wasm3 -DWASMOS_DOTCONFIG=configs/wasm3_smp_defconfig && \
  cmake --build build-wasm3 --target run-qemu-test
cmake -S . -B build-warp  -DWASMOS_DOTCONFIG=configs/warp_smp_defconfig  && \
  cmake --build build-warp  --target run-qemu-test
cmake -S . -B build && cmake --build build --target run-kernel-unit-tests
```
A client-side or docs-only change that cannot differ per runtime can be gated on
one runtime; state which you ran.

## Gotchas

- WARP boot smokes are timing-sensitive and can **flake** (a near-complete boot
  that misses `halt`/`[calculator] ready` before the timeout). Re-run once on the
  same dir before treating a single failure as a regression; capture the **full**
  log (don't pipe through `tail`, which truncates the QEMU output).
- CI forces software emulation: `WASMOS_QEMU_ACCEL=tcg` and a long
  `WASMOS_QEMU_TEST_TIMEOUT=600` (wasmos is only validated under TCG; KVM breaks
  the bootloader→kernel handoff). Set these to reproduce CI locally.
- ESP staleness: the Python runners boot `build/esp/kernel.elf`, staged by the
  `run-*` targets. Rebuild via cmake (or restage) before running a Python test
  binary directly.
- Use LLVM `clang`, not AppleClang: `-DCLANG=/opt/homebrew/opt/llvm/bin/clang`.
