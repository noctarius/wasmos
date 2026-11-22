---
name: wasmos-ide-targets
description: Keep CLion/clangd (and clang-tidy) coverage in sync when adding or moving C/C++ sources. Explains why a file shows "not in a project target", how the build's compile_commands.json is produced (real compile edges plus index-only entries), how to audit orphaned sources, and how to fix coverage in the owning BUILD.py. Use when a new file is not indexed or a "missing project target" appears.
---

# WASMOS: Update IDE Coverage

## Why this exists

CLion, clangd and clang-tidy index a file only if it has an entry in the build
directory's `compile_commands.json`. `./configure` writes that file next to
`build.ninja` and rewrites it whenever the build regenerates. A source with no
entry shows in CLion as **"not in a project target"**, is not indexed, and is
silently skipped by `scripts/quality.sh lint`.

Entries come from two places (`scripts/buildgen/buildgen.py`):

- **Real compile edges** — every `b.toolchain(...).compile(...)` call (kernel,
  loader, native modules, sysroot libc/libsys, host tools) records the exact
  command it runs.
- **Index-only entries** — `b.index(source, argv)` records a command without a
  build edge. The build uses it where the real compile is not a plain clang
  invocation a tool can replay:
  - guest C modules: `guest()` (`scripts/buildgen/wasmos_rules.py`) compiles
    through the SDK's `wasmos-clang` wrapper and indexes each source with the
    `wasm32` profile flags against the in-tree headers;
  - C sources a Zig/Rust/Go driver compiles internally (`guest(..., lang=...)`);
  - host unit tests: `tests/unit/BUILD.py` compiles each test in one
    multi-source command and indexes every source with that test's flags
    (`_index_sources`);
  - the other WASM runtime's kernel glue: `src/kernel/BUILD.py` indexes it with
    the flags a tree of that runtime would use (`b.toolchain(name,
    overrides=...)`), so one tree covers both runtimes;
  - libsys entry shims and Go glue (`src/libsys/BUILD.py`), the SDK hello app
    (`scripts/sdk/BUILD.py`), `warp_aot` in a wasm3 tree, the WARP host test,
    and the clang-tidy plugin.

## Step 1: Audit — what's orphaned?

From a configured build directory:

```sh
python3 - out/default <<'PY'
import json, os, subprocess, sys
root = os.getcwd()
cc = json.load(open(os.path.join(sys.argv[1], "compile_commands.json")))
cov = {os.path.realpath(e["file"]) for e in cc}
tus = [f for f in subprocess.check_output(
           ["git", "ls-files", "src", "tests", "examples", "tools"], text=True).splitlines()
       if f.endswith((".c", ".cc", ".cpp", ".cxx"))]
orph = [f for f in tus if os.path.realpath(os.path.join(root, f)) not in cov]
print(f"TUs {len(tus)}  covered {len(tus) - len(orph)}  ORPHANS {len(orph)}")
for f in orph:
    print(" ", f)
PY
```

Expected orphans: `src/kernel/wasm_chardev.c`, which nothing builds (see the
comment at its top). Only `.c/.cc/.cpp/.cxx` are listed; headers are indexed
transitively through a covering source and its include path.

## Step 2: Fix in the owning BUILD.py

Coverage lives with the build step that compiles the file, so a fix is almost
always "make the build compile it":

- **Guest module (driver/service/util/example):** list the file in that
  module's `guest(b, name, [sources...])` call. A multi-file module must list
  *all* its `.c` files, not just the entry.
- **Kernel (`src/kernel/`):** add it to `CORE` (or the runtime list it belongs
  to) in `src/kernel/BUILD.py` — see `skills/wasmos-kernel-internals`.
- **Native ring-3 module:** add it to that manifest's `tc.compile(...)` call.
- **Host unit test:** the test's entry in `TESTS` (`tests/unit/BUILD.py`) lists
  its sources; every source there is indexed with the test's flags.
- **A file the build legitimately does not compile in this configuration** (a
  source of a mode another tree builds, a file a non-clang driver compiles):
  add a `b.index(source, [compiler, *flags])` call in the owning manifest with
  the flags the real compile uses. Take the flags from a toolchain profile
  (`b.toolchain("wasm32").tc.cflags`, `b.toolchain("kernel", overrides={...})`)
  rather than restating them.

Get the include set right — the same `-I`/`-iquote` set the real compile uses —
or the file indexes with unresolved `#include`s.

## Host unit tests: one libc per translation unit

`tests/unit` is host-compiled, so both our libc headers and the host's are
within reach — but a TU that sees both does not parse: our headers redeclare
`strcpy`/`memcpy`/…, and the macOS SDK defines those names as `_FORTIFY_SOURCE`
macros. Each test picks one through its own flags, and its index entries carry
exactly those flags:

- Compiles against **our** libc: `-I src/libc/include`. Our
  `<string.h>`/`<stdio.h>`/`<stdlib.h>` win; the host's are never reached.
- Compiles against the **host** libc: no libc `-I`, or `-iquote
  src/libc/include`. The host serves `<angle>` includes — including
  `stderr`/`fprintf`/`abort`, which our libc does not declare — and ours serve
  `"quoted"` ones.
- Compiles **real kernel or vendored-library sources**: it sees those sources'
  own headers, not the deliberately minimal `tests/unit/include` shims, and
  carries the `-D` set its compile line passes, because the headers branch on
  them.

Because index entries are derived from the test's real command, a test that
builds also indexes; a header conflict under `lint` means the test's own flags
mix both libcs.

## Step 3: Regenerate and verify

`ninja -C out/default` regenerates `build.ninja` and `compile_commands.json`
after a `BUILD.py` edit. Re-run the Step 1 audit; the file's realpath should now
be covered. CLion opens the source root as a compilation-database project
through the `compile_commands.json` symlink `configure` maintains there
(`./configure <dir> --ide-link` re-points it; `docs/BUILD_SYSTEM.md` §10
*IDE Entry Point*); reload it after regenerating unless auto-reload is on. An
index-only entry never affects the real build or the boot, so no
`run-qemu-test` is required for an IDE-only change.

## Guardrails

- Do not add a `b.index` entry for a file a build edge already compiles:
  duplicate entries make clangd pick one arbitrarily.
- `b.index` entries are indexing-only; they never add a build edge.
- A file that is genuinely dead (compiled by nothing) should be **deleted**,
  not given an index entry.
