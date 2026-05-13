# Temporary Ring-3 Hardening Checklist

Purpose: Track all deferred ring-3 hardening tasks and block merge until all are complete.

## Merge Gate Rule
- [ ] Do not merge `ring3-hardening-checklist` until every checkbox in this file is complete and validated.

## A) Hostcall Boundary Cleanup
- [x] Inventory remaining coherence-bridge / compatibility-layer hostcall paths.
- [x] Replace user-slot compatibility bridge callsites in hostcalls that relied on implicit USER mapping flags (`wasmos_map_framebuffer`, `wasmos_shmem_map` in `src/kernel/wasm3_link.c`).
- [x] Remove implicit user-slot map compatibility bridge in `paging_map_4k_in_root` (`src/kernel/paging.c`) so callers must pass explicit `MEM_REGION_FLAG_USER`.
- [x] Replace any remaining direct user-pointer dereferences with validated copy helpers (`mm_copy_from_user`, `mm_copy_to_user`) where applicable.
  - Pointer-bearing entry-path hostcalls now resolve/check user VAs before copy; remaining host-view reconciliation is isolated in `wasm_copy_*_sync_views` as explicit TODO-scoped compatibility bridge.
- [x] Ensure explicit user-VA resolution and range checks on pointer-bearing hostcalls.
  - Audited `m3ApiGetArgMem` hostcalls in `src/kernel/wasm3_link.c`; pointer-bearing paths perform `wasm_user_va_from_host_ptr` plus `mm_user_range_permitted` checks before user-memory copies/maps.
- [x] Audit syscall/hostcall argument width handling (`u64 -> u32` truncation deny).
  - Done so far: syscall-side signed status width checks for `EXIT` and `THREAD_EXIT` now enforce strict `i32` sign-extension validity.
  - Done in this slice: hostcall-side endpoint width check hardened for `wasmos_serial_register` (negative endpoints now rejected before `uint32_t` conversion).
  - Remaining: keep regressions out as new hostcall endpoint/ID surfaces are added.
- [x] Add/update tests for all changed hostcall boundary behaviors (validated via strict QEMU gates in section F for this slice).

## B) Adversarial IPC Coverage Expansion
- [x] Add spoofed-source request/reply abuse tests.
  - Added invalid-source spoof probe in strict ring3 IPC call path with marker: `[test] ring3 ipc call spoof invalid source deny ok`.
- [x] Add stale/cross-process reply-identity misuse tests.
  - Added stale/future request-id replay probe in ring3 IPC call correlation path with strict marker: `[test] ring3 ipc call stale id deny ok`.
  - Existing spoofed-source probe remains in place (`[test] ring3 ipc call source auth ok`) and continues to validate reply-source ownership/authentication.
- [x] Add stronger out-of-order reply queue retention/match/drop tests.
  - Added out-of-order reply retention check in strict ring3 IPC call path with marker: `[test] ring3 ipc call out-of-order retain ok`.
- [x] Extend control-plane endpoint deny-path assertions.
  - Added explicit control-endpoint deny marker in strict ring3 IPC call path: `[test] ring3 ipc call control endpoint deny ok`.
- [x] Verify endpoint ownership + sender-context checks under stress.
  - Added strict stress marker in IPC call source-auth path after multiple inauthentic reply drops: `[test] ring3 ipc owner+sender stress ok`.

## C) Fault-Policy Coverage Expansion
- [x] Add additional user-origin exception probes beyond current baseline set.
  - Added `ring3-fault-bp` (`int3`) probe with strict marker: `[test] ring3 fault bp exit status ok`.
- [x] Assert process-local containment (faulting process terminated; kernel/other processes remain live).
  - Added containment liveness marker in fault-policy runner: `[test] ring3 containment liveness ok`.
- [x] Assert deterministic policy markers/reason + termination status markers for each probe.
  - Strict status markers now include `#BP` alongside existing probes, all validating deterministic `-11` policy exits.
- [x] Add mixed abuse scenarios (fault + syscall + IPC churn) for liveness validation.
  - Mixed churn loop remains enforced with strict marker: `[test] ring3 mixed stress ok`.

## D) Dedicated Multi-Process Fault-Storm Liveness Suite
- [x] Add strict ring3 multi-process fault-storm suite target/profile.
  - Added dedicated target: `run-qemu-ring3-fault-storm-test` (with marker runner `run-qemu-ring3-fault-storm-check`).
- [x] Include watchdog/progress markers for forward progress.
  - Suite requires `[test] ring3 watchdog clean ok` and `[test] sched progress ok`.
- [x] Include trap-frame integrity assertions under repeated fault load.
  - Suite rejects trap-frame watchdog fault marker: `[watchdog] trap frame invalid cs=`.
- [x] Document expected markers and failure signatures in test output.
  - Marker expectations and fail markers are codified in `scripts/qemu_ring3_fault_storm_test.py`.

## E) CLI Smoke Flake Reduction
- [x] Harden test image/reset isolation in CLI smoke harness.
  - CLI suite now runs with isolated per-session ESP copies (`WASMOS_QEMU_ISOLATE_ESP=1`).
- [x] Remove known flaky ordering/reset hazards around shared mutable artifacts.
  - `QemuSession` now supports isolated runtime ESP trees to avoid cross-test mutable artifact contention.
- [x] Stabilize deterministic pass/fail signaling for CLI smoke.
  - CLI target now runs via `scripts/run_unittest_suite.py` and emits explicit marker (`[test] cli suite status ok` / failed).

## F) Required Validation Before Merge
- [x] `cmake --build build --target run-qemu-test`
- [x] `cmake --build build --target run-qemu-ring3-test`
- [x] `cmake --build build --target run-qemu-cli-test`
- [x] Any new/updated suite targets added by this branch.
  - `cmake --build build --target run-qemu-ring3-fault-storm-test`

## G) Documentation Alignment Before Merge
- [x] Update `README.md` highlights if behavior/test markers change.
- [x] Update `docs/ARCHITECTURE.md` checkpoint text for landed hardening work.
- [x] Update `docs/architecture/14-ring3-isolation-and-separation.md` with concrete closure notes.
- [x] Update `docs/TASKS.md` open items to reflect what is closed vs still deferred.

## H) Final Branch Exit Criteria
- [ ] All tasks above complete.
- [ ] No known ring-3 hardening TODO/FIXME left without explicit follow-up owner/plan.
- [ ] Branch is ready for review and merge.
