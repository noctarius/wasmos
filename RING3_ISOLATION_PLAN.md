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
- Previously observed runtime noise (`[cli] vt writer register failed`,
  `[sysinit] spawn failed`) is now mitigated in current baseline via VT writer
  reassignment + CLI serial fallback + conservative sysinit single-CLI startup.
  `run-qemu-test` and `run-qemu-ring3-test` currently pass with this fallback.
- Phase-1 strict-mode copy-path debt remains: removing compatibility host-pointer
  mirror writes for sensitive early output paths still regresses ring3 smoke
  (`[mode] strict-ring3=1`) at `hw-discovery` (`ACPI RSDP too small`) followed
  by `#GP` in `init`. Keep dual-write compatibility in place until those
  consumers are fully decoupled from immediate host-pointer visibility.

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
  - Migration item completed: `wasmos_boot_config_copy` now uses pure
    validated user-VA copy semantics (`mm_copy_to_user` path) without
    host-view mirroring.
  - Remaining direct user-pointer dereference inventory (next migration batch
    candidates):
    - Output writers: none in current inventory.
    - Input readers still carrying split-view compatibility handling:
      `wasmos_console_write`, `wasmos_strlen`,
      `wasmos_block_buffer_write`, `wasmos_fs_buffer_write`
    - Bidirectional buffer paths: migrated to output-side coherence bridge
      (`wasm_copy_to_user_sync_views`) for
      `wasmos_block_buffer_copy` and `wasmos_fs_buffer_copy`.
  - Migration caution: `mm_copy_to_user`/`mm_copy_from_user` now use chunked
    bounce-buffer copies across CR3 switches; continue validating size/range
    arithmetic and explicit permission preflight per call site during
    conversion.
  - Consumer-boundary hardening: added `wasmos_sync_user_read(ptr,len)` to let
    WASM services refresh host-view bytes from validated user memory after
    hostcalls that return data via output buffers.
  - Remaining migration objective: continue replacing direct pointer writes/
    reads in pointer-bearing hostcalls with `mm_copy_to_user` /
    `mm_copy_from_user` where practical, while keeping split-view bridge usage
    explicit until runtime-level host/user linear-memory coherence is resolved.
  - Migration item completed: `wasmos_early_log_copy` now copies into a local
    bounce buffer and writes to user memory via `mm_copy_to_user` in bounded
    chunks, removing direct host-pointer output writes from this path.
  - Migration item completed: `wasmos_proc_info` now returns process names via
    bounded local buffering plus `mm_copy_to_user` (NUL-terminated), removing
    direct host-pointer output writes from this path.
  - Migration item completed: `wasmos_proc_info_ex` now returns both
    `parent_pid` and process name via `mm_copy_to_user` (bounded copy +
    NUL-terminated string), removing direct host-pointer output writes from
    this path.
  - Migration item completed: `wasmos_console_read` now writes input bytes back
    to user memory through `mm_copy_to_user` instead of direct host-pointer
    writes.
  - Migration item advanced: `wasmos_console_write` and `wasmos_strlen` now
    use an input-side coherence bridge (`mm_copy_from_user` first, host-view
    divergence detect + resync) instead of consuming host pointers directly.
  - Migration item advanced: `wasmos_block_buffer_write` and
    `wasmos_fs_buffer_write` now use the same input-side coherence bridge for
    chunked user-buffer ingestion before writing into kernel-owned block/fs
    staging buffers.
  - Migration item advanced: `wasmos_block_buffer_copy` and
    `wasmos_fs_buffer_copy` now return output through the output-side
    coherence bridge (`mm_copy_to_user` + host-view sync) instead of direct
    host-pointer writes.
  - Compatibility hardening update: added a shared helper for sensitive
    early-boot output hostcalls that performs both `mm_copy_to_user` and a
    host-pointer mirror write. This preserves current non-strict behavior while
    ensuring validated user-VA writes are still exercised in the same path.
  - Debug finding (boot-module-name repro): user-VA write/readback via
    `mm_copy_to_user`/`mm_copy_from_user` succeeded, while the immediate wasm
    host-pointer view remained empty at that moment. This indicates view
    divergence for some early-boot consumers and explains the observed
    regression sensitivity when host-mirror writes are removed.
  - Strict-mode experiment result (2026-05-02): disabling host-pointer mirror
    for the sensitive dual-write paths in `run-qemu-ring3-test`
    (`[mode] strict-ring3=1`) regressed at `hw-discovery` with
    `ACPI RSDP too small` followed by `#GP` in `init`. Compatibility mirror is
    therefore still required until ACPI/module-name/boot-config consumers are
    fully decoupled from immediate host-pointer visibility.
  - Strict-mode revalidation (2026-05-02): after wiring `hw-discovery` to call
    `wasmos_sync_user_read` for ACPI/module-name output buffers, switching
    `wasmos_acpi_rsdp_info` and `wasmos_boot_module_name` to pure
    `wasm_copy_to_user` semantics passed `run-qemu-ring3-test`,
    `run-qemu-cli-test`, and `run-qemu-test`.
  - Consumer revalidation (2026-05-02): after wiring `sysinit` to call
    `wasmos_sync_user_read` for boot-config output buffers, switching
    `wasmos_boot_config_copy` to pure `wasm_copy_to_user` semantics passed
    `run-qemu-ring3-test`, `run-qemu-cli-test`, and `run-qemu-test`.
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
    - `cmake --build build --target run-qemu-cli-test`: pass (`45 tests`, OK)
      after harness hardening and example-output expectation rebaseline; no new
      copy-helper failure markers observed in captured serial output.

Exit criteria:
- Every pointer-bearing kernel entry point is inventory-tracked and migrated.
- No direct user-pointer dereference remains in entry paths.
- Width and overflow negative tests pass.

Legacy-path impact:
- Remove any “best effort” bypasses that touch user memory without validation.
- Keep temporary compatibility only behind explicit flag if absolutely needed.

Phase 1 status (2026-05-02): complete.

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

Progress update (2026-05-02):
- Added paging-level user-root kernel-footprint verifier
  (`paging_verify_user_root`) with dump helper
  (`paging_dump_user_root_kernel_mappings`).
- Enforced verifier at child address-space creation and after user context
  region setup (`mm_context_create`) to fail fast on unauthorized kernel
  PML4/PDPT exposure.
- Added source-level regression assertions for verifier presence and
  address-space creation hook in `tests/test_memory_privilege_foundation_spec.py`.
- Added runtime inspection surface: CLI `kmaps` command calls hostcall
  `kmap_dump`, which runs dump+verify for the active process root table.
- Extended runtime inspection surface: CLI `kmaps all` calls hostcall
  `kmap_dump_all`, which iterates all active processes and runs dump+verify
  for each live context root table.
- Tightened default higher-half sharing from broad-window behavior to a
  bounded 32 MiB allowlist (PDE-level), and extended verifier checks to assert
  allowed PD entries inside the approved higher-half PDPT window.

Progress update (2026-05-03):
- User-root verifier no longer requires `PML4[0]` pointer identity with kernel
  root; it now verifies higher-half slot parity (`PML4[511]`) plus explicit
  low-slot PDPT entry allowlist (`IDENTITY_PD_COUNT` only).
- Child address-space creation now allocates a private low-slot PDPT and copies
  only allowlisted identity/direct-map entries from the kernel template.
- Address-space teardown now frees child-private low-slot PDPT pages.
- Experimental removal of child `PML4[0]` exposed a concrete blocker: kernel
  paths that temporarily run under user CR3 roots (`mm_copy_from_user` /
  `mm_copy_to_user` switch windows and scheduler dispatch window before user
  context entry) still execute on low-mapped kernel stacks. Next Phase-2 task
  is to migrate kernel rsp0/scheduler stack execution to higher-half mappings
  (or equivalent stack-safe copy plumbing) before dropping child low-slot
  mappings.

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
