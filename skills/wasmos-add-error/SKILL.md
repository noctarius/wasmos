---
name: wasmos-add-error
description: Add or change a WASMOS error/status code via the single-source IDL abi/errors.yaml + scripts/gen_abi_errors.py. Covers the two axes (the transport wasmos_status_t vs the namespaced packed (domain, code) operation errors), the stable domain registry, how to add a code vs a new domain, what is generated for every language (constants + packed accessors + decode lookups + the 40-byte error object + wrap/unwrap/is/as chain helpers), and the append-only stability rules. Use whenever adding/retiring an error code or domain.
---

# WASMOS: Add an Error / Status Code

## Overview

WASMOS has two error axes, both single-sourced in `abi/errors.yaml`:

- **transport** (`wasmos_status_t`): a small global, negative-on-error axis for
  when the *call mechanism* or service itself failed (`INVAL`, `DENIED`, `FULL`,
  `NOENT`, `TIMEOUT`, …).
- **domains**: the namespaced *operation-error* space. Each domain has a STABLE
  numeric id; a packed error is the **negative** of `(domain << 16) | local_code`.
  Domain `0` (`none`) is reserved for success / an empty chain frame.

`scripts/gen_abi_errors.py` generates the full value ABI for every language; you
never hand-write these constants. See
`docs/architecture/34-abi-idl-and-error-model.md`.

Generated per language (do **not** hand-edit — `GENERATED` banner):
`abi/generated/<lang>/wasmos_status.{h,rs,go,zig,ts}` — the transport enum, the
domain registry (stable ids), the packed `(domain, code)` constants, decode
lookups (`wasmos_status_str` / `wasmos_strerror`-style), the fixed 8-byte frame /
40-byte error object, and the chain helpers (`wrap`/`unwrap`/`root`/`is`/`as`).
Only the IPC framing that serializes the error block into a reply lives
hand-written in `src/libsys/{wasm,native}`.

## Workflow

1. Edit `abi/errors.yaml` (add a code, or a new domain, or a transport value).
2. Regenerate and verify.
3. Return/propagate it at the call site (broad migration is Phase 4).

## Step 1: Edit `abi/errors.yaml`

**Add a code to an existing domain** — append to that domain's `codes:` list.
Local code ids are assigned by position and start at 1 (0 reserved), so
**append only; never reorder or delete** (that renumbers the packed value and
breaks the ABI across every language):

```yaml
  proc_pm:
    id: 2
    description: "non-path process-manager IPC failures (was PROC_PM_ERR_*)"
    codes:
      # … existing, in order …
      - { name: QUOTA,  description: "per-user process quota exceeded" }   # appended
```

**Add a new domain** — give it the next free stable `id` (append to the registry;
ids are permanent):

```yaml
  net_dns:
    id: 12
    description: "DNS resolver failures"
    codes:
      - { name: NXDOMAIN, description: "name does not resolve" }
      - { name: SERVFAIL, description: "resolver returned SERVFAIL" }
```

**Add a transport value** — extend the `transport:` list with the next free
negative value (these are also append-only / stable).

## Step 2: Regenerate and verify

```sh
python3 scripts/gen_abi_errors.py             # regenerate every language file
python3 scripts/gen_abi_errors.py --check     # confirm generated files match the IDL
python3 scripts/gen_abi_errors.py --lang c    # (optional) restrict to one language
```

Requires PyYAML. `--check` is wired into the `quality` lint target, so a stale
checked-in file fails CI. Commit the regenerated `abi/generated/**` with the IDL.
Optionally re-verify the generated files compile with their toolchains (`clang`,
`rustc`, `zig ast-check`, `go vet`, `asc`).

## Step 3: Use it

Return the packed constant (e.g. `WASMOS_ERR_PROC_PM_QUOTA`) as the reply status
or host-call result — it is already negative, so it needs no cast or re-signing,
and `wasmos_strerror(rc)` decodes a return value directly. Use the chain helpers
to attach provenance at a deliberate abstraction seam: `wasmos_error_wrap(higher,
lower)` appends a frame; `unwrap`/`root`/`is`/`as` inspect the chain (max depth
4). The transport axis rides the IPC result; domain errors ride the reply payload
/ opcode status field.

## Guardrails

- **Append-only, stable ids.** Never reorder, renumber, or delete a domain id, a
  local code, or a transport value — the numbers are the cross-language ABI.
  Retire by leaving the slot and marking it deprecated in its `description`.
- Pick the right axis: transport = the call/mechanism failed; a domain code = a
  named *operation* failure. Do not add operation errors to `transport`.
- Migrating the legacy `PROC_*`/`FS_ERR_*`/`SHMEM_*` call sites and the bare
  `return -1;` backlog onto these packed codes is **Phase 4** — the vocabulary
  exists but is not yet wired through the OS, so a code-only change needs no boot
  test.
- Never hand-edit `abi/generated/**`; never return a bare `-1` for a distinct
  failure — use a named transport value or a packed `(domain, code)`.
