---
name: wasmos-integration-test
description: Add or change a QEMU integration test under tests/ and assign it to a test battery. Covers the QemuSession framework (boot, expect, send, timeouts), the tests/batteries.json manifest that partitions the suite across CI runners, running one battery locally, the --verify-batteries completeness gate, and how a battery becomes a CI job. Use when adding a file under tests/, when a test needs different QEMU options, or when the battery layout changes.
---

# WASMOS Integration Test

## Overview

An integration test boots the real system under QEMU and drives it through the
serial console. It lives in `tests/test_<name>.py`, uses `QemuSession` from
`scripts/qemu_test_framework.py`, and **must** be assigned to a battery in
`tests/batteries.json`.

Unit tests are a different thing and live elsewhere: C kernel tests under
`tests/unit/` run via `ninja -C out/<dir> run-kernel-unit-tests` and boot
nothing.

## The battery system

The suite is partitioned by subsystem into **batteries**, and every QEMU battery
runs once PER WASM RUNTIME — so a CI runner is a (battery, runtime) cell, not a
battery:

| Battery | What it covers |
| --- | --- |
| `boot-and-init` | Boot chain, init, CLI, panic decoding, device manager |
| `scheduler-and-ipc` | Scheduler, preemption, threading, IPC wakeup, buffer borrows |
| `networking` | virtio-net, net-stack, sockets, DNS, TLS |
| `graphics-and-vt` | Graphics, virtual terminals, hardware input |
| `filesystem` | Filesystem reads and writes |
| `language-runtimes` | One guest per supported source language |
| `host-tools` | Host tools only — boots nothing, needs no emulation |

`tests/batteries.json` is the single source for EVERY axis — the `batteries`
list and the `arches` list, where each architecture names the `runtimes`
(defconfigs) its QEMU batteries run under and the `build_only` defconfigs CI
builds without booting (an architecture under bring-up). Two consumers read it
and neither enumerates any axis itself:

- `scripts/run_unittest_suite.py` selects which files to run; its `--matrix`
  mode emits the cross product (one cell per QEMU battery per (arch, runtime),
  one per host battery), and `--build-matrix` the build-only cells.
- `.github/workflows/ci.yml` consumes exactly those outputs.

A third list, `hosts`, names the machines builds and host tests run ON
(x86_64-linux, aarch64-linux, aarch64-darwin) with the CI runner for each;
`--host-matrix` feeds CI's kernel unit-test job, which runs on every host.

Add a battery, a runtime, an architecture or a host there and the runners appear;
nothing else needs editing. The harness boots the architecture the tree was
configured for (`WASMOS_ARCH`, set by the run targets); `QemuConfig.arch`
overrides it, and `klog_port="com2"` is x86_64-only (QEMU virt has one UART).

**Why the runtime axis exists.** wasm3 interprets in-kernel at CPL 0; WARP JITs
to ring 3. They break independently, and the difference is not always visible
from one side: a guest mapping that aliases linear memory under WARP but not
under wasm3 passes a WARP-only run and fails nothing until someone boots the
other runtime by hand. CI ran WARP only for a long time, which is exactly how
that hid. A cell also ASSERTS the runtime it claims, by grepping the booted
kernel's `[subsystem] register request=WASM runtime=...` marker rather than
trusting the configure step — a build directory's `.config` is seeded once and
keeps whatever runtime it was seeded with (or later edited to), and an ESP can
serve a kernel older than the tree it was built from.

Batteries are **semantic, not duration-balanced** — `networking` is much the
largest and sets the critical path, deliberately. A red job named `networking`
says what broke without opening a log, and membership stays stable as tests are
added, so a battery's history stays comparable run to run. An i-of-N shard has
neither property: adding one test reshuffles every partition.

### The rule that matters

> Every file under `tests/` belongs to **exactly one** battery.

A named battery does not cover new files by construction the way a numeric shard
does, so a test assigned to nothing would simply never run and no one would
notice. `--verify-batteries` is what prevents that, and it runs in the quality
gate:

```sh
python3 scripts/run_unittest_suite.py --verify-batteries
```

It fails if a discovered file is in no battery, in two batteries, or if the
manifest lists a file that does not exist.

## Step 1: Write the test

```python
import unittest
from scripts.qemu_test_framework import QemuSession, default_config, default_kernel_path

class MyFeatureTest(unittest.TestCase):
    """One line saying what this proves — it is what a reader sees when it fails."""

    session: QemuSession | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cfg = default_config()
        cls.session = QemuSession(cfg, timeout_s=150)
        cls.session.start()
        if not cls.session.expect(b"wamos> ", timeout_s=120):
            cls.session.close()
            raise RuntimeError("CLI prompt not reached")

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.session:
            cls.session.force_stop()
            cls.session.close()
            cls.session = None

    def test_my_feature(self) -> None:
        assert self.session is not None
        self.session.send("spawn /apps/my_app")
        self.assertTrue(
            self.session.expect(b"[my-app] ready", timeout_s=30),
            "my_app did not reach ready — <what that implies is broken>",
        )
```

Points that repeatedly matter:

- **Assert on markers the guest prints**, never on timing. `expect()` returns
  False on timeout; give the assertion a message naming what the absence implies,
  because that message is the whole failure report in CI.
- **The first `wamos> ` is handled for you.** It does not mean the CLI is usable
  -- services keep starting behind it -- so on the first prompt match `expect()`
  probes the console (sends an empty line, requires the prompt back promptly)
  and reports the prompt found only once it answers. Later matches do not probe,
  since those are a command completing. You do not need to call `settle()`
  yourself; it is public for a test with an unusual boot.
  Readiness is deliberately a probe and NOT console silence: a system running the
  gfx demos or a vt never goes quiet, so waiting for silence there burns the whole
  budget and makes every battery minutes slower.
- **Do not raise a timeout to fix a CI-only failure.** `WASMOS_TEST_TIMEOUT_SCALE`
  multiplies every deadline and CI sets it to 3, so a slow runner is already
  accounted for. A test that still times out is telling you something.
- **Boot once per class** (`setUpClass`), not per test. A boot is seconds.
- **`force_stop()` then `close()`** in teardown, or a stray QEMU outlives the run.
- Serial output interleaves from several processes, so ordering between two
  different components' lines is not guaranteed — match each independently.
- A test that needs no NIC should set `WASMOS_QEMU_NIC_MODEL=none`; one that
  needs it must NOT (see `test_virtio_net_notify_e2e`).

## Step 2: Assign it to a battery

Add the filename to the right battery's `files` list in `tests/batteries.json`,
keeping the list sorted. If it fits no battery, add one — `name`, `description`,
`needs_qemu`, `files` — and CI picks it up automatically.

Set `needs_qemu: false` only if the test boots nothing. Those run on a fast job
that installs nothing — no emulation toolchain, and no `./configure` either,
since `configure` resolves the cross toolchain and fails on `llvm-objcopy` long
before it reaches a host tool. The `host-tools` job compiles what it needs
with `cc` directly, which works only because `scripts/make_wasmos_app.c` includes
nothing but libc. A host test whose tool needs the real build does not belong in
that battery.

Then verify, before running anything expensive:

```sh
python3 scripts/run_unittest_suite.py --verify-batteries
```

## Step 3: Run it

```sh
# one file, fastest iteration (after `ninja -C out/<dir>` has staged the ESP)
WASMOS_ESP=out/<dir>/esp WASMOS_KERNEL=out/<dir>/kernel.elf \
  python3 -m unittest tests.test_my_feature

# the battery it belongs to
WASMOS_TEST_BATTERY=filesystem ninja -C out/<dir> run-qemu-cli-test

# everything (what CI runs, split across runners)
ninja -C out/<dir> run-qemu-cli-test
```

`run-qemu-cli-test` brings `out/<dir>/esp` and the disk images up to date,
exports the `WASMOS_ESP` / `WASMOS_KERNEL` / image / OVMF / SMP environment the
framework reads (`scripts/system/BUILD.py`), and sets
`WASMOS_QEMU_ISOLATE_ESP=1`. Run outside it, a test falls back to the
`build/...` defaults in `scripts/qemu_test_framework.py`, so a direct
`python3 -m unittest` or `python3 scripts/run_unittest_suite.py --battery <name>`
needs those variables pointed at `out/<dir>`. The target honours
`WASMOS_TEST_BATTERY`, which is how CI runs one battery per runner without
duplicating the ESP staging.

**Never run two QEMU integration targets at once.** They share a mutable
`out/<dir>/esp` and disk images, and parallel runs produce flaky failures like `Error deleting` and
corrupted boot config. Batteries are safe in CI only because each runner has its
own filesystem — that is a cross-machine property, not a licence to parallelise
locally.

## Step 4: Check it before committing

```sh
python3 scripts/run_unittest_suite.py --verify-batteries
WASMOS_TEST_BATTERY=<its battery> ninja -C out/<dir> run-qemu-cli-test
ninja -C out/<dir> run-qemu-test   # boot gate, always
```

## Diagnosing a failure

- Serial output is not always clean; `grep -a` rather than `grep`.
- **A test that mutates the filesystem must clean up first, not after.** The ESP
  persists between local runs, so a test that creates something and leaves it
  passes once and fails forever after, while CI's fresh ESP passes every time --
  which reads as flakiness. `fs_write_smoke` had exactly this: it removes its
  directory before creating it.
- A test that passes locally and fails in CI is usually timing: CI is Linux
  MTTCG with SMP, local macOS is often `thread=single`, which masks
  memory-ordering races (see `skills/wasmos-build-and-run`).
- Before concluding a failure is pre-existing, build the baseline commit in a
  **separate git worktree** and run the same test there. Do not use `git stash`
  in this repository.
- Pull the CI log directly rather than guessing:
  `gh api repos/noctarius/wasmos/actions/jobs/<id>/logs`

## Files

- `tests/batteries.json` — the manifest; single source for runner and CI
- `scripts/run_unittest_suite.py` — `--battery`, `--list-batteries`, `--matrix`,
  `--verify-batteries`
- `scripts/qemu_test_framework.py` — `QemuSession`, `default_config`
- `.github/workflows/ci.yml` — `batteries` job resolves the matrix; `host-tests`
  and `integration-tests` consume it
