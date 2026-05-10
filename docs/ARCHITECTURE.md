# Architecture Notes

This document is the architecture index and entry point for the repository.
Detailed design and implementation status now live in focused documents under
`docs/architecture/`.

IMPORTANT: Keep this file and `README.md` up to date with every prompt execution
and code iteration.
IMPORTANT: Create a git commit after each prompt iteration.
Threading design details are maintained in
`docs/architecture/15-threading-and-lifecycle.md`.
Ring-3 isolation architecture and separation model are documented in
`docs/architecture/14-ring3-isolation-and-separation.md`.
Latest checkpoint: Phase 2 mapping minimization is closed for current strict
scope; Phase 5 fault-policy expansion coverage is in place for `#PF`, `#UD`,
`#GP`, `#DE`, `#DB`, `#OF`, `#NM`, `#SS`, and `#AC` process-local termination
probes; Phase 6 scheduler/trap robustness is closed for current scope
(watchdog + mixed-stress + trap-integrity rollout complete, with one noted
transient CLI fs-smoke flake); Phase 7 memory-service/shared-mapping isolation
is closed for current strict scope with owner-bound checks, explicit grants/
revoke, strict ring3 cross-process deny/allow markers, app-pair forged/stale
negative checks, kernel misuse-matrix gate markers, and shared-map state
ordering hardening; Phase 8 strict-mode/default compatibility-path deletion is
closed for current scope with `WASMOS_RING3_STRICT` removed, plus removal of
low-slot strict-mode configuration
knobs (`WASMOS_LOW_SLOT_SWEEP`, `WASMOS_LOW_SLOT_SWEEP_LEVEL`,
`WASMOS_IDENTITY_PD_COUNT`) in favor of fixed strict baseline behavior and a
green strict stabilization cycle (`run-qemu-test`, `run-qemu-ring3-test`,
`run-qemu-cli-test`).
Kernel boot smoke now also runs a shared-memory misuse matrix (forged IDs,
wrong-owner grant/revoke attempts, pre/post-grant map deny/allow, idempotent
revoke, and release-balance checks) in the strict-ring3 gate.
Threading rollout (`docs/architecture/15-threading-and-lifecycle.md`) is
closed through Phase D for current scope: scheduler-active internal worker
threads (dedicated kernel stacks + worker entrypoints), targeted multi-thread
IPC stress, and native ring3 syscall coverage (`gettid`, `thread_yield`,
`thread_exit`, `thread_create`, `thread_join`, `thread_detach`, including
deny-path markers) are validated in smoke. A user-facing continuation-style
native thread wrapper API (`wasmos/thread_x86_64.h`) is available for native
ring3 callers. A separate opt-in strict ring3
thread-lifecycle profile is now available via `run-qemu-ring3-threading-test`
to validate strict ring3 threading signals (ring3-threading spawn plus thread
create/join/detach syscall markers). The lifecycle profile now also checks
kill-while-blocked wait wakeup behavior via `[test] threading wait kill wake ok`
plus join-after-kill ordering and kill-during-join wakeup markers
(`[test] threading join after kill order ok`,
`[test] threading join kill wake ok`) without altering baseline strict startup
behavior. Stack teardown now restores guard-page mappings before allocator free
so recycled pages remain reachable through the shared higher-half alias window
under strict threading stress.
Forward note: future deterministic kernel race/integration tests should use a
centralized hook/instrumentation layer around kernel transition points (for
example scheduler/process/thread lifecycle events) so orchestration logic does
not spread as ad-hoc test fragments across runtime code paths.
Build configuration now has a Kconfig-compatible entry point (`Kconfig`) plus
`configs/wasmos_defconfig`, with CMake importing `build/.config` through
`scripts/kconfig_to_cmake.py` when present. The imported scope is currently
intentionally narrow (existing CMake toggles and a few key scalar values) to
preserve minimalism and keep behavior deterministic.

## Architecture Document Map
- [Goals](architecture/01-goals.md)
- [Current System Summary](architecture/02-current-system-summary.md)
- [Architectural Direction](architecture/03-architectural-direction.md)
- [Boot and Handoff](architecture/04-boot-and-handoff.md)
- [Scheduling and Preemption](architecture/05-scheduling-and-preemption.md)
- [Process and IPC](architecture/06-process-and-ipc.md)
- [Memory Management](architecture/07-memory-management.md)
- [Runtime and Packaging](architecture/08-runtime-and-packaging.md)
- [Drivers and Services](architecture/09-drivers-and-services.md)
- [CLI and User-Space Baseline](architecture/10-cli-and-user-space.md)
- [Diagnostics and Status](architecture/11-diagnostics-status.md)
- [Repository Map and Validation Baseline](architecture/12-repo-map-and-validation.md)
- [Virtual Terminal](architecture/13-virtual-terminal.md)
- [Ring3 Isolation and Separation](architecture/14-ring3-isolation-and-separation.md)
- [Threading and Lifecycle](architecture/15-threading-and-lifecycle.md)

## Update Rules
- Update the relevant feature document(s) in `docs/architecture/` when behavior
  changes.
- Keep cross-document references consistent across `README.md`, `docs/TASKS.md`, and
  architecture docs.
- Prefer appending concrete implementation notes over vague roadmap text.
