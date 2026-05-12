# Temporary Ring-3 Hardening Checklist

Purpose: Track all deferred ring-3 hardening tasks and block merge until all are complete.

## Merge Gate Rule
- [ ] Do not merge `ring3-hardening-checklist` until every checkbox in this file is complete and validated.

## A) Hostcall Boundary Cleanup
- [x] Inventory remaining coherence-bridge / compatibility-layer hostcall paths.
- [x] Replace user-slot compatibility bridge callsites in hostcalls that relied on implicit USER mapping flags (`wasmos_map_framebuffer`, `wasmos_shmem_map` in `src/kernel/wasm3_link.c`).
- [x] Remove implicit user-slot map compatibility bridge in `paging_map_4k_in_root` (`src/kernel/paging.c`) so callers must pass explicit `MEM_REGION_FLAG_USER`.
- [ ] Replace any remaining direct user-pointer dereferences with validated copy helpers (`mm_copy_from_user`, `mm_copy_to_user`) where applicable.
- [ ] Ensure explicit user-VA resolution and range checks on pointer-bearing hostcalls.
- [ ] Audit syscall/hostcall argument width handling (`u64 -> u32` truncation deny).
  - Done so far: syscall-side signed status width checks for `EXIT` and `THREAD_EXIT` now enforce strict `i32` sign-extension validity.
  - Done in this slice: hostcall-side endpoint width check hardened for `wasmos_serial_register` (negative endpoints now rejected before `uint32_t` conversion).
  - Remaining: broader hostcall-width sweep as additional endpoint/ID surfaces evolve.
- [x] Add/update tests for all changed hostcall boundary behaviors (validated via strict QEMU gates in section F for this slice).

## B) Adversarial IPC Coverage Expansion
- [ ] Add spoofed-source request/reply abuse tests.
- [x] Add stale/cross-process reply-identity misuse tests.
  - Added stale/future request-id replay probe in ring3 IPC call correlation path with strict marker: `[test] ring3 ipc call stale id deny ok`.
  - Existing spoofed-source probe remains in place (`[test] ring3 ipc call source auth ok`) and continues to validate reply-source ownership/authentication.
- [x] Add stronger out-of-order reply queue retention/match/drop tests.
  - Added out-of-order reply retention check in strict ring3 IPC call path with marker: `[test] ring3 ipc call out-of-order retain ok`.
- [ ] Extend control-plane endpoint deny-path assertions.
- [ ] Verify endpoint ownership + sender-context checks under stress.

## C) Fault-Policy Coverage Expansion
- [ ] Add additional user-origin exception probes beyond current baseline set.
- [ ] Assert process-local containment (faulting process terminated; kernel/other processes remain live).
- [ ] Assert deterministic policy markers/reason + termination status markers for each probe.
- [ ] Add mixed abuse scenarios (fault + syscall + IPC churn) for liveness validation.

## D) Dedicated Multi-Process Fault-Storm Liveness Suite
- [ ] Add strict ring3 multi-process fault-storm suite target/profile.
- [ ] Include watchdog/progress markers for forward progress.
- [ ] Include trap-frame integrity assertions under repeated fault load.
- [ ] Document expected markers and failure signatures in test output.

## E) CLI Smoke Flake Reduction
- [ ] Harden test image/reset isolation in CLI smoke harness.
- [ ] Remove known flaky ordering/reset hazards around shared mutable artifacts.
- [ ] Stabilize deterministic pass/fail signaling for CLI smoke.

## F) Required Validation Before Merge
- [x] `cmake --build build --target run-qemu-test`
- [x] `cmake --build build --target run-qemu-ring3-test`
- [x] `cmake --build build --target run-qemu-cli-test`
- [ ] Any new/updated suite targets added by this branch.

## G) Documentation Alignment Before Merge
- [ ] Update `README.md` highlights if behavior/test markers change.
- [ ] Update `docs/ARCHITECTURE.md` checkpoint text for landed hardening work.
- [ ] Update `docs/architecture/14-ring3-isolation-and-separation.md` with concrete closure notes.
- [ ] Update `docs/TASKS.md` open items to reflect what is closed vs still deferred.

## H) Final Branch Exit Criteria
- [ ] All tasks above complete.
- [ ] No known ring-3 hardening TODO/FIXME left without explicit follow-up owner/plan.
- [ ] Branch is ready for review and merge.
