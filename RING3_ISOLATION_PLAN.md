# Ring3 Isolation Plan

This document defines the remaining work to reach full ring-3 isolation between
processes and hard kernel survival guarantees under user-space faults.

## Goal

A user-space process/driver (WASM or native) must not be able to crash, hang,
or corrupt the kernel through malformed input, invalid memory access, or
intentional abuse. Faulting user-space must be contained to the offending
process (or explicitly scoped service domain).

## Security Invariants

1. User code cannot read, write, or execute kernel memory.
2. User code cannot access another process's private memory without an explicit
   kernel-mediated sharing primitive.
3. All user-kernel crossings (syscalls, hostcalls, IPC payload handling) are
   bounds-checked and overflow-safe.
4. Kernel faults are never triggered by direct user-pointer dereference or
   unchecked user-controlled addresses.
5. Privileged operations (PIO/MMIO/IRQ/DMA/kernel-owned IPC endpoints) are
   denied by default and granted explicitly.
6. A user fault exits only the offending process with deterministic policy
   status (`-11` today), while scheduler and other processes continue.

## Current Status Summary

- Ring3 syscall path is active and smoke-tested.
- Per-process address spaces exist with dedicated user slot mappings.
- User fault classification + process-local termination behavior exists.
- User copy helpers (`mm_copy_from_user`, `mm_copy_to_user`) and permission
  preflight APIs exist and are partially migrated.
- Ring3 smoke already asserts fault reason markers and policy exit status
  markers.

Remaining work is mostly policy tightening, mapping minimization, and adversarial
coverage depth.

## Ordering Principle

Implementation order is optimized for early deletion of non-ring3 code paths.
That means:

1. Make ring3 path complete and test-gated first.
2. Turn legacy/non-ring3 behavior into explicit compatibility toggles.
3. Flip defaults to strict ring3 as soon as tests are green.
4. Delete compatibility branches immediately after one stabilization cycle.

## Ordered Implementation Plan

Each phase below is intentionally dependency-ordered. Do not skip ahead unless
all phase exit criteria are met.

### Phase 0: Baseline Hard Gates (Do First)

Objective: prevent regressions while tightening isolation.

Tasks:
- Add CI gate profile `strict-ring3` that always runs:
  - `run-qemu-test`
  - `run-qemu-ring3-test`
  - new adversarial ring3 tests from this plan as they land
- Add a boot-time marker that declares active isolation mode:
  - `[mode] strict-ring3=0/1`
- Add test helper checks that fail if strict mode is expected but disabled.

Exit criteria:
- CI can run strict-ring3 profile deterministically.
- Mode marker is visible and asserted in relevant tests.

Known baseline test debt to track during later phases:
- `run-qemu-cli-test` currently has unrelated language-example regressions
  (`test_hello_go`, `test_hello_rust`, `test_hello_zig` missing expected hello
  output, with follow-on `BrokenPipeError` in `tearDownClass` after failed
  expectations). Treat these as pre-existing non-phase-0 blockers; keep them
  visible while progressing strict-ring3 isolation work.
  TODO: Re-baseline or fix language example output expectations so full CLI test
  suite can be restored to green alongside strict-ring3 gating.

Legacy-path impact:
- None removed yet; this phase only creates safety rails.

### Phase 1: Ring3-Only Syscall/Hostcall Boundary Completion

Objective: complete pointer/length safety so kernel no longer relies on
trusted caller assumptions.

Tasks:
- Build an explicit entry-point inventory (file/function list) for:
  - syscall handlers
  - hostcalls in `wasm3_link.c`
  - IPC marshal/unmarshal paths
  - native-driver ABI functions that consume user-owned arguments
- For each entry point, enforce:
  - `u64` -> expected-width validation (reject truncation)
  - overflow-safe address arithmetic (`base+offset+len`)
  - `mm_user_range_permitted` preflight where non-copy access is required
  - `mm_copy_from_user` / `mm_copy_to_user` for copy semantics
- Add targeted negative tests for each migrated boundary.

Phase 1 inventory tracker (initial pass):
- Syscall handlers (`src/kernel/syscall.c`):
  - `x86_syscall_handler` (`wait`, `ipc_notify`, `ipc_call`) currently enforces
    `u64` -> `u32` cleanliness via `syscall_arg_u32`; `ipc_call` still has a
    deferred unmatched-reply correlation TODO for Phase 4.
- WASM hostcalls (`src/kernel/wasm3_link.c`):
  - Pointer-bearing hostcalls are largely guarded by
    `wasm_user_va_from_host_ptr` + `mm_user_range_permitted`.
  - Migration item completed: `wasmos_framebuffer_info` now performs
    `mm_copy_to_user` using the validated user VA (`out_user`) rather than a
    raw host pointer reinterpretation.
  - Migration item in progress: `wasmos_boot_config_copy` now attempts
    `mm_copy_to_user` using validated user VA (`ptr_user`) first, with a
    temporary compatibility fallback to host-pointer copy pending cleanup.
  - Remaining direct user-pointer dereference inventory (next migration batch
    candidates):
    - Output writers: `wasmos_boot_config_copy` (fallback path only),
      `wasmos_acpi_rsdp_info`, `wasmos_boot_module_name`,
      `wasmos_proc_info`, `wasmos_proc_info_ex`,
      `wasmos_console_read`
    - Input readers: `wasmos_console_write`, `wasmos_strlen`,
      `wasmos_block_buffer_write`, `wasmos_fs_buffer_write`
    - Bidirectional buffer paths: `wasmos_block_buffer_copy`,
      `wasmos_fs_buffer_copy`
  - Migration caution: `mm_copy_to_user`/`mm_copy_from_user` now use chunked
    bounce-buffer copies across CR3 switches; continue validating size/range
    arithmetic and explicit permission preflight per call site during
    conversion.
  - Additional caution: a direct `wasmos_acpi_rsdp_info` -> `mm_copy_to_user`
    swap regressed hw-discovery in non-strict mode (`ACPI RSDP too small`);
    keep this callsite deferred until ACPI consumer assumptions are aligned.
  - Remaining migration objective: continue replacing direct pointer writes/
    reads in pointer-bearing hostcalls with `mm_copy_to_user` /
    `mm_copy_from_user` where practical.
  - Migration item completed: `wasmos_early_log_copy` now copies into a local
    bounce buffer and writes to user memory via `mm_copy_to_user` in bounded
    chunks, removing direct host-pointer output writes from this path.
- IPC marshal/unmarshal (`src/kernel/ipc.c`, call sites in `syscall.c` and
  `wasm3_link.c`):
  - Endpoint/context ownership checks are in place; Phase 4 will complete
    unmatched-reply preservation and correlation hardening.
- Native-driver ABI (`src/kernel/native_driver.c`):
  - Current ABI entry points (`nd_shmem_create/map/unmap`) are scalar-only at
    the kernel boundary; no direct user-pointer ingestion in these entry points
    today.
- Phase 1 diagnostics update (2026-05-02):
  - Added trace-gated (`WASMOS_TRACE`) failure-stage instrumentation in
    `mm_copy_from_user` / `mm_copy_to_user` to report operation, stage,
    context id, user address/size, expected vs current CR3 root, and failing
    chunk address/size on copy-path failures.
  - Validation on baseline behavior:
    - `python3 -m unittest tests.test_memory_privilege_foundation_spec`: pass.
    - `cmake --build build --target run-qemu-test`: pass.
    - `cmake --build build --target run-qemu-cli-test`: same known baseline
      failures (`test_hello_go`, `test_hello_rust`, `test_hello_zig`) plus
      follow-on teardown `BrokenPipeError`; no new copy-helper failure markers
      observed in captured serial output.

Exit criteria:
- Every pointer-bearing kernel entry point is inventory-tracked and migrated.
- No direct user-pointer dereference remains in entry paths.
- Width and overflow negative tests pass.

Legacy-path impact:
- Remove any “best effort” bypasses that touch user memory without validation.
- Keep temporary compatibility only behind explicit flag if absolutely needed.

### Phase 2: Kernel Mapping Minimization in User CR3

Objective: remove broad kernel exposure from user-owned page tables.

Tasks:
- Replace shared “window copy” model with explicit allowlisted kernel mappings
  required for ring transitions only (trampoline/syscall/interrupt essentials).
- Add a runtime verifier:
  - enumerate PML4/PDPT/PD entries for user roots
  - assert only approved kernel ranges are present
- Add a debug command or trace dump to print user-root kernel mapping footprint.
- Add tests that fail when unauthorized kernel ranges appear.

Exit criteria:
- User roots map only approved kernel transition/support ranges.
- No general higher-half bulk alias remains in child roots.

Legacy-path impact:
- Delete old bulk higher-half sharing behavior.

### Phase 3: Strict Capability Default-Deny Flip

Objective: remove implicit privilege in userspace drivers/services.

Tasks:
- Replace compatibility fallback (`allow when unconfigured`) with strict deny.
- Require capability declarations for:
  - IO port
  - IRQ route/register
  - MMIO map
  - DMA buffer
  - privileged kernel/service endpoints
- Harden WASMOS-APP capability parsing:
  - malformed/unknown capability descriptors fail closed
- Add deny/allow tests per capability class.

Exit criteria:
- Unconfigured privileged actions are denied by default.
- Capability tests pass in strict mode.

Legacy-path impact:
- Remove compatibility-mode privilege grants.

### Phase 4: IPC Isolation + Correlation Hardening

Objective: prevent endpoint spoofing, confused replies, and cross-context abuse.

Tasks:
- Enforce endpoint ownership and sender context checks on all relevant paths.
- Resolve unmatched reply TODO:
  - implement per-process pending-reply queue
  - preserve unmatched messages until matching waiter or explicit drop policy
- Add sensitive endpoint allowlists (process manager, memory service, control
  planes) with explicit deny markers.
- Add replay/spoof/out-of-order stress tests.

Exit criteria:
- No request/reply confusion under adversarial ordering.
- Cross-context spoofing attempts deterministically fail.

Legacy-path impact:
- Remove message-drop behaviors that silently hide ordering bugs.

### Phase 5: Fault Policy Completion Across User Exception Space

Objective: guarantee user-origin faults are always process-local failures.

Tasks:
- Expand fault-policy assertions beyond current probes to all relevant user
  exception vectors handled by kernel trap path.
- Keep dual assertions for each probe:
  - classified reason marker
  - expected termination status marker
- Add fault-storm test:
  - multiple ring3 processes repeatedly triggering distinct fault classes
  - scheduler liveness assertion

Exit criteria:
- All covered user exception classes terminate offending process only.
- Kernel remains alive across sustained fault storms.

Legacy-path impact:
- Remove any remaining user-fault paths that escalate to global panic unless
  architecturally unrecoverable and not user-originating.

### Phase 6: Scheduler/Trap Robustness Under Malicious Workloads

Objective: prove ring transitions and preemption are stable under abuse.

Tasks:
- Add long-duration mixed stress:
  - high-rate syscalls
  - IPC call/reply churn
  - periodic forced user faults
- Add watchdog markers for:
  - scheduler forward progress
  - stuck reschedule loops
  - trap frame integrity violations
- Validate TSS `rsp0`, trampoline return, and context save/restore invariants
  under stress.

Exit criteria:
- No deadlocks or context corruption in sustained stress runs.
- Kernel continues serving unaffected processes.

Legacy-path impact:
- Remove temporary trap/preempt compatibility guards introduced during bring-up.

### Phase 7: Memory Service / Shared Mapping Isolation Closure

Objective: ensure pager/shared-memory operations cannot be abused for cross-
process or kernel mapping escalation.

Tasks:
- Assert fault-driven mapping only occurs inside owning region policy.
- Harden shared-memory map/unmap:
  - explicit ownership checks
  - explicit permissions for remap into target context
  - prevent arbitrary physical rebind through forged parameters
- Add map-forgery/cross-context negative tests.

Exit criteria:
- Cross-context mapping abuse attempts fail.
- Shared memory behavior is explicit, revocable, and policy-checked.

Legacy-path impact:
- Remove implicit remap assumptions and any “caller trusted” physical map paths.

### Phase 8: Strict Mode Default + Compatibility Path Deletion

Objective: finish migration by removing non-ring3 behavior.

Tasks:
- Flip build/runtime defaults to strict ring3 mode.
- Keep compatibility mode only as short-lived opt-in fallback.
- Run one stabilization cycle with strict as default in CI.
- Delete compatibility code paths and flags after cycle is green.
- Update docs and test expectations to strict-only.

Exit criteria:
- Strict ring3 is default everywhere.
- Compatibility branches are removed from mainline.

Legacy-path impact:
- This is the explicit deletion phase for all non-ring3 behavior.

## Workstreams (Cross-Cutting Execution Tracks)

The following tracks run across phases but should not violate ordered
dependencies above.

### A) Kernel Mapping Minimization in User CR3

Objective: remove broad kernel aliasing from user-owned address spaces.

Primary phase: 2.

### B) Complete User-Pointer Boundary Migration

Objective: eliminate direct/implicit user-memory dereference in kernel paths.

Primary phase: 1.

### C) Capability Enforcement: Strict Default-Deny

Objective: prevent user-space drivers/services from touching privileged kernel
resources without explicit capability grants.

Primary phase: 3.

### D) IPC and Endpoint Hardening

Objective: ensure message transport cannot be abused to escalate privileges or
break isolation.

Primary phase: 4.

### E) Fault-Containment Policy Completion

Objective: guarantee user-triggered exceptions never collapse kernel progress.

Primary phase: 5.

### F) Scheduler / Trap Robustness Under Abuse

Objective: ensure trap/interrupt/preempt path cannot be destabilized by ring3
behavior.

Primary phase: 6.

### G) Memory Service / Pager Isolation Guarantees

Objective: ensure demand-mapping and shared-memory operations stay scoped.

Primary phase: 7.

## Test Plan Additions

Add ring3-focused adversarial tests (QEMU-driven):

1. Invalid pointer matrix per syscall/hostcall category.
2. Width/overflow matrix (`u64` truncation, `offset+len` wrap).
3. Cross-context memory and endpoint abuse attempts.
4. Capability deny matrix (PIO/MMIO/IRQ/DMA/privileged IPC).
5. Multi-process fault storm with scheduler liveness checks.
6. Long preempt+syscall stress with watchdog markers.
7. Unauthorized kernel-range mapping detector in user roots.
8. Capability malformed-policy rejection at app load.
9. IPC unmatched-reply preservation/correlation stress.

All above should be part of CI gating for strict isolation mode.

## Rollout Strategy

1. Keep staged compatibility only behind explicit build/runtime flags.
2. Introduce strict-isolation mode that is CI-default.
3. Finish phases 1-7 and hold strict-mode green for one stabilization cycle.
4. Flip production/default policy to strict.
5. Delete compatibility paths and compatibility flags.

## Definition of Done

The project reaches "full ring-3 isolation" when:

- Kernel mapping in user roots is minimal and explicitly allowlisted.
- All kernel entry points are safe against malformed user memory arguments.
- Capability policy is default-deny with no implicit allow fallbacks.
- User faults are always process-local and policy-asserted.
- Adversarial ring3 test suite passes consistently in CI.
- A compromised user-space process/driver cannot crash the kernel.
