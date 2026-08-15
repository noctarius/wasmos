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
`tests/unit/` run via `cmake --build build --target run-kernel-unit-tests` and
boot nothing.

## The battery system

The suite is partitioned by subsystem into **batteries**, one per CI runner:

| Battery | What it covers |
| --- | --- |
| `boot-and-init` | Boot chain, init, CLI, panic decoding, device manager |
| `scheduler-and-ipc` | Scheduler, preemption, threading, IPC wakeup, shmem grants |
| `networking` | virtio-net, net-stack, sockets, DNS, TLS |
| `graphics-and-vt` | Graphics, virtual terminals, hardware input |
| `filesystem` | Filesystem reads and writes |
| `language-runtimes` | One guest per supported source language |
| `host-tools` | Host tools only — boots nothing, needs no emulation |

`tests/batteries.json` is the single source. Two consumers read it and neither
enumerates batteries itself:

- `scripts/run_unittest_suite.py` selects which files to run.
- `.github/workflows/ci.yml` builds its job matrix from it.

Add a battery there and a CI runner appears; nothing else needs editing.

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
that installs nothing — no emulation toolchain, and no CMake configure either,
since a full configure validates the cross toolchain and fails on `llvm-objcopy`
long before it reaches a host tool. The `host-tools` job compiles what it needs
with `cc` directly, which works only because `scripts/make_wasmos_app.c` includes
nothing but libc. A host test whose tool needs the real build does not belong in
that battery.

Then verify, before running anything expensive:

```sh
python3 scripts/run_unittest_suite.py --verify-batteries
```

## Step 3: Run it

```sh
# one file, fastest iteration
python3 -m unittest tests.test_my_feature

# the battery it belongs to
python3 scripts/run_unittest_suite.py --battery filesystem

# everything (what CI runs, split across runners)
cmake --build build --target run-qemu-cli-test
```

The CMake target honours `WASMOS_TEST_BATTERY`, which is how CI runs one battery
per runner without duplicating the ESP staging:

```sh
WASMOS_TEST_BATTERY=filesystem cmake --build build --target run-qemu-cli-test
```

**Never run two QEMU integration targets at once.** They share a mutable
`build/esp`, and parallel runs produce flaky failures like `Error deleting` and
corrupted boot config. Batteries are safe in CI only because each runner has its
own filesystem — that is a cross-machine property, not a licence to parallelise
locally.

## Step 4: Check it before committing

```sh
python3 scripts/run_unittest_suite.py --verify-batteries
python3 scripts/run_unittest_suite.py --battery <its battery>
cmake --build build --target run-qemu-test   # boot gate, always
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
