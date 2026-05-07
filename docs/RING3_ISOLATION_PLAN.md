# Ring3 Isolation Plan (Archive)

## Status
Phases 0-8 are closed for the current strict-ring3 scope (as of 2026-05-07).

The active architecture description for ring3 isolation and kernel/user-space
separation now lives in:
- `docs/architecture/14-ring3-isolation-and-separation.md`

## Closed Scope Summary
The completed migration established:
- strict ring3 as default behavior for current scope
- deletion of strict compatibility toggles and transitional mode knobs
- strict behavioral gate assertions (fault policy, IPC isolation,
  shared-memory isolation, low-slot sweep diagnostics)
- closed stabilization cycle across:
  - `run-qemu-test`
  - `run-qemu-ring3-test`
  - `run-qemu-cli-test`

## Deferred Backlog (Post-Closure)
These items were noted during phase execution and remain intentional follow-up
work:

1. Hostcall boundary hardening tail
- Continue reducing remaining split-view/coherence-bridge input paths where
  practical, while preserving validated user copy semantics.

2. IPC adversarial coverage extension
- Extend specialized request/reply abuse coverage beyond current `ipc_call` and
  process-manager focused paths.

3. Exception coverage extension
- Add process-local handling and probes for additional user-origin exception
  vectors beyond current covered strict set.

4. Multi-process fault-storm suite
- Add sustained mixed fault-storm liveness assertions.

5. CLI fs-smoke harness flake follow-up
- Revisit image/reset isolation in CLI fs-smoke harness paths to remove
  mutable-artifact flake potential.

## Notes
This file is retained as a compact historical closure record and deferred-item
tracker. Architecture and behavior details belong in the architecture docs.
