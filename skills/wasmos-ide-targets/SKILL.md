---
name: wasmos-ide-targets
description: Keep CLion/clangd IDE coverage in sync when adding or moving C/C++ sources. Explains why a file shows "not in a project target", how the EXCLUDE_FROM_ALL IDE OBJECT targets + compile_commands.json work, how to audit orphaned sources, and how to fix coverage (app helper SOURCES, KERNEL_SOURCES, the wasmos_ide_index globs, or a bespoke wasmos_add_ide_c_target). Use when a new file is not indexed or a "missing project target" appears.
---

# WASMOS: Update IDE Targets

## Why this exists

Most first-party sources are built via `add_custom_command` (raw `clang`), which
CMake does **not** export to `compile_commands.json`. CLion/clangd + clang-tidy
index a file only if it has an entry there — so coverage is provided by parallel
`EXCLUDE_FROM_ALL` **OBJECT** targets (never linked; indexing only). A file with
no such target shows in CLion as **"not in a project target"** and isn't indexed.

Coverage sources (in the current tree):
- **App helpers** — `wasmos_add_wasm_c_app_target` / `wasmos_add_native_c_app_target`
  (root `CMakeLists.txt`) auto-emit a `<name>_ide` OBJECT target from their
  `SOURCES`. Covers apps/drivers/services/utils.
- **Kernel** — `kernel_ide` is derived from `KERNEL_SOURCES` (`src/kernel/CMakeLists.txt`).
- **Shared areas** — `wasmos_ide_index(...)` at the end of the root `CMakeLists.txt`
  globs `libc`, `libsys`, `libui`, `tests/unit`, `src/tools`
  (`file(GLOB … CONFIGURE_DEPENDS)`), so new files there need no CMake edit.
- **Bespoke** — `net_stack` declares its own `wasmos_add_ide_c_target` (lwIP/mbedTLS
  includes).

## Step 1: Audit — what's orphaned?

`compile_commands.json` is the oracle. From a configured build:

```sh
python3 - <<'PY'
import json, os, subprocess
root=os.getcwd(); cc=json.load(open("build/compile_commands.json"))
cov={os.path.realpath(e['file'] if os.path.isabs(e['file']) else os.path.join(e['directory'],e['file'])) for e in cc}
tus=[f for f in subprocess.check_output(["git","ls-files","src","tests","examples"],text=True).splitlines()
     if f.endswith(('.c','.cc','.cpp','.cxx')) and '/generated/' not in f]
orph=[f for f in tus if os.path.realpath(os.path.join(root,f)) not in cov]
print(f"TUs {len(tus)}  covered {len(tus)-len(orph)}  ORPHANS {len(orph)}")
for f in orph: print(" ", f)
PY
```

Note: `compile_commands.json` reflects the **current** configure. A source only
compiled under another config (e.g. WARP-only `src/kernel/warp/*`, or ring3-smoke
probes) legitimately appears orphaned in the default (wasm3) build — reconfigure
that backend to see it. Only `.c/.cc/.cpp/.cxx` are listed; headers are indexed
transitively through a covering TU + its include path.

## Step 2: Fix by owner

Pick the mechanism that owns the file's area:

- **App (util/driver/service via a helper):** ensure the file is in the helper's
  `SOURCES`. A multi-file app must pass *all* its `.c` in `SOURCES`, not just the
  entry — a stray extra `.c` is the common miss.
- **Kernel (`src/kernel/`):** add it to `KERNEL_SOURCES` (the three-parallel-lists
  rule — see `skills/wasmos-kernel-internals`). `kernel_ide` follows automatically.
- **Shared area already globbed** (`libc`, `libsys`, `libui`, `tests/unit`,
  `src/tools`): nothing to edit — just reconfigure (the glob re-scans).
- **New shared/bespoke area** (a new top-level source dir): add it to an existing
  `wasmos_ide_index(...)` `DIRS`, or add a new `wasmos_ide_index(<name> DIRS <dir>
  INCLUDES <the dir's -I set>)` call at the end of the root `CMakeLists.txt`.
- **Complex bespoke build** (lwIP/mbedTLS-style, like `net_stack`): extend that
  target's `wasmos_add_ide_c_target(... SOURCES ... INCLUDES ...)`.

Get the `INCLUDES` right (the same `-I` set the real build uses) or the file
indexes with unresolved `#include`s.

## Step 3: Reconfigure and verify

```sh
cmake -S . -B build
```
Re-run the Step 1 audit — the file's realpath should now be covered. In CLion,
reload the CMake project. IDE targets are `EXCLUDE_FROM_ALL`, so this never
affects the real build or the boot; no `run-qemu-test` is required for an
IDE-only change.

## Guardrails

- Do **not** glob a directory that already has per-app `_ide` coverage (e.g.
  `src/utils`, `src/drivers`, most of `examples`): duplicate `compile_commands`
  entries make clangd pick one arbitrarily and can regress good indexing.
- IDE targets are indexing-only (`EXCLUDE_FROM_ALL`); they are never linked and
  must never be added to the real build graph.
- A file that is genuinely dead (compiled by nothing) should be **deleted**, not
  given an IDE target.
