---
name: wasmos-regression-test
description: Write and run a regression test for a bug, red-to-green. Covers the mandatory order (failing test first, fix second), the Regression marker every such test carries, what makes a test behavioral rather than a source-text assertion, how to build and run the host unit suites (run-kernel-unit-tests, individual binaries, seed replay, the ThreadSanitizer arms), how to register a new C/C++ suite in CMakeLists.txt, and what to do when a bug genuinely cannot be reproduced on the host. Use whenever fixing a bug, before writing the fix.
---

# WASMOS Regression Test

## Overview

A regression test is the test that would have caught a specific bug. This
repository requires it to exist, to be **red before the fix**, and to say which
bug it belongs to.

The order is not a style preference. A test written after the fix proves only
that the code passes its own author's expectations; it never demonstrated it can
fail, so it may assert nothing. A test that goes red first, for the reason the
bug describes, is the only kind that pins the behavior.

## The four rules

1. **Behavioral, never source-text.** A test asserts runtime behavior, output,
   state transitions, or an API contract. It must never grep the repository for
   a line, a word, or a comment. See *What behavioral means* below.
2. **Carries a regression marker.** Every regression case names the bug it
   belongs to in its comment. See *The marker* below.
3. **Written before the fix.** Commit order may put them together, but the test
   is authored and run first.
4. **Red for the right reason, then green.** Run it against the unfixed tree and
   confirm both that it fails and that the failure message describes the bug. A
   test that fails because it does not compile, or because a stub is missing, has
   demonstrated nothing. Only then write the fix.

Do not skip 4 by reasoning that the test obviously fails. Two cases in this tree
would have passed unfixed because they asserted the wrong field.

## The marker

Put a `Regression:` line in the case's comment block, with an identifier that
outlives this conversation:

```c
/* Regression: #142 -- an enqueue refused because the thread was executing left
 * only a READY mark, which a holder past its own check never acted on, so the
 * thread sat runnable on no run queue and every FS request queued behind it. */
static void test_enqueue_current_leaves_a_consumable_claim(void) {
```

Identifier, in order of preference:

- A GitHub issue number: `Regression: #142`.
- A date-based id when there is no issue: `Regression: 2026-08-17-readdir-terminator`
  — the date the bug was diagnosed plus a short slug. Unique, sortable, and it
  survives being copied into a commit message.
- Add the fixing commit once it exists, if it helps a reader:
  `Regression: 2026-08-17-readdir-terminator (fixed 46bd8b5d26)`.

State the **failure**, not the fix: what went wrong and what it cost. A reader
hitting this test in three years needs to know what breaks if they delete it.

Markers are greppable on purpose:

```bash
rg -n "Regression:" tests/
```

## What behavioral means

Valid — the code runs and the test asserts what happened:

```c
cpu_sched_enqueue(&g_cpus[0].sched, t);
CHECK(t->on_rq == 1, "queued");
CHECK(t->enqueue_owed == 0, "owes nothing");
```

Valid — an API contract, asserted at runtime by which function got called:

```c
/* Tripwires must report through the LOCKING writer: without the lock a
 * concurrent CPU's line interleaves mid-string and corrupts both. */
CHECK(saw("enqueue non-ready"), "the tripwire reported");
CHECK(g_log_unlocked_count == 0, "and not through the unlocked writer");
```

**Invalid** — asserting the source text still contains something:

```python
# NEVER. Brittle, and it verifies nothing about behavior.
self.assertRegex(read_file("src/kernel/sched_thread.c"), r"serial_printf\(")
```

If the only test you can think of is a source-text assertion, the behavior is
not observable yet. Make it observable — a counter, a return code, a log line
the harness captures — and assert that. Existing source-text tests in this tree
are legacy and should be replaced, not imitated.

## Choosing the level

| The bug lives in | Test it as | Skill |
| --- | --- | --- |
| Kernel data structures, scheduler, IPC, libc, parsers | Host unit test under `tests/unit/` | this one |
| Service/driver protocol, boot flow, CLI, anything needing a live system | QEMU integration test under `tests/` | `skills/wasmos-integration-test` |

Prefer the host unit test: it runs in milliseconds, needs no emulation, and can
drive states a live system reaches only by luck. Reach for the integration test
when the bug is in the interaction between processes.

## Running the host unit suites

```bash
cmake --build build --target run-kernel-unit-tests
```

Every suite builds and runs in one target; the first failure stops it. Individual
binaries land in the build directory and can be rerun directly, which is what you
want while iterating:

```bash
./build/test_sched_runqueue_bias_on
./build/test_ipc_concurrency
```

### Order randomization and seed replay

Suites shuffle their case order (`tests/unit/test_shuffle.h`) so a case that
leaks state cannot make its neighbour pass. The seed is printed **before** the
first case runs:

```
test_shuffle: WASMOS_TEST_SEED=0x9e3779b97f4a7c15
```

Replay an exact order with `WASMOS_TEST_SEED=0x…`. The generator is a fixed
splitmix64, not the host's `rand`, so a seed from a Linux CI failure reproduces
the same order on macOS.

`WASMOS_TEST_MAX_CASES` (256) bounds a suite's case count. Exceeding it aborts
with a message rather than overflowing the order array.

### The ThreadSanitizer arms

`test_sched_concurrency` and `test_ipc_concurrency` also build with
`-fsanitize=thread`. They are the gate for anything touching cross-CPU state: a
data race there fails the target with `Error 66` and 20+ warnings. If you add a
field written by one CPU and read by another — including a diagnostic counter —
these are the suites that will catch you.

### Adding a new C/C++ suite

Register it in `CMakeLists.txt` next to the others: a `COMMAND ${CLANG} …` that
compiles the test plus exactly the kernel translation units it needs, then a
`COMMAND ${BUILD_DIR}/test_<name>` that runs it. Keep the source list minimal —
each file you add drags in its dependencies and its stubs.

Stubs available under `tests/unit/`:

| Stub | Provides |
| --- | --- |
| `stubs_spinlock.c` | Host spinlocks, per-CPU state, `g_host_cpu_local` |
| `stubs_slab.c` | Slab allocator over host `malloc` |
| `stubs_kpanic.c` | `kpanic` (aborts), weak `serial_printf` / `serial_printf_unlocked` |
| `stubs_xfer_buffer_platform.c` | Transfer-buffer platform hooks |
| `stubs_native_libsys.c` | Native-service libsys surface |

The weak writers in `stubs_kpanic.c` let a suite define its own capturing
versions — that is how a test asserts on what the kernel reported.

After adding a suite, keep IDE/lint coverage in sync
(`skills/wasmos-ide-targets`): `scripts/quality.sh lint` reads a rewritten
compile database from the `*_ide` targets, and a file that is in no target is
silently unlinted.

Other languages: AssemblyScript suites under `tests/unit/as/` (run by
`run_as_test.mjs`), Go under `tests/unit/go/` (TinyGo + node, skipped when
TinyGo is absent), Rust as a cargo test. They hang off the same target.

## The workflow

1. **Reproduce first.** Get the bug to fail in front of you — a CI capture, a
   boot log, a thread dump. Understand the mechanism before writing an assertion;
   a test written against a guess pins the guess.
2. **Write the failing test**, with its `Regression:` marker.
3. **Run it against the unfixed tree.** Confirm red, and read the message: it
   must describe the bug, not a missing symbol.
4. **Write the fix.**
5. **Run it again.** Green.
6. **Run the whole suite**, not just your case — `run-kernel-unit-tests` end to
   end, plus `run-qemu-test`, plus the affected battery if the bug had one.
   Randomized order means your new case can expose an old leak; that is the
   feature working, and it is your commit's problem.
7. **Gates before committing**: `cmake --build build --target fmt-check` and
   `lint` (through the targets — they build the clang-tidy plugin the `wasmos-*`
   checks live in).

## When the host cannot reproduce it

Some bugs are not reachable from a host process: real SMP memory ordering, a
context switch, UART timing, anything whose mechanism is the hardware. Do not
fake a test for those, and do not weaken a real one until it passes.

Do this instead:

- Test the **nearest observable contract**. The serial-interleaving bug could not
  be reproduced on the host — there is no serial line — but "a tripwire reports
  through the locking writer" is a contract, assertable at runtime by which
  symbol the linker resolved, and it is what keeps the fix from being undone.
- If even that is impossible, say so plainly in the commit message and in
  `docs/TASKS.md`, with what you did verify instead (boots, sweeps, sample
  sizes). An honest "not covered by a test, verified by 40 boots" is worth more
  than a test that cannot fail.
- Record the reproduction recipe where the next person will find it — the task
  entry, or a handover doc for a host that can see the bug.

## Anti-patterns

- **Test after fix.** It never demonstrated it can fail.
- **Asserting the fix's shape instead of the bug's absence.** Assert the thread
  is queued; do not assert that a particular helper was called, unless the call
  itself *is* the contract (see the writer example above).
- **Loosening an assertion to get green.** If the test is wrong, fix the test and
  say why. If the fix is wrong, fix the fix.
- **A test that passes on the unfixed tree.** Delete it and start again; it is
  worse than nothing, because it looks like coverage.
- **Grepping source text.** Covered above, and forbidden by `AGENTS.md`.
