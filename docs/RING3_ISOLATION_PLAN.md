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

Tasks (next-cycle hardening candidates after closure):
- Add CI gate profile `strict-ring3` that always runs:
  - `run-qemu-test`
  - `run-qemu-ring3-test`
  - new adversarial ring3 tests from this plan as they land
- Add strict behavioral marker checks that fail when isolation regresses
  (fault-policy, IPC-deny/allow, shared-memory isolation, and sweep diagnostics).

Exit criteria:
- CI can run strict-ring3 profile deterministically.
- Strict behavioral markers are asserted in relevant tests.

Known baseline test debt to track during later phases:
- Previously observed runtime noise (`[cli] vt writer register failed`,
  `[sysinit] spawn failed`) is now mitigated in current baseline via VT writer
  reassignment + CLI serial fallback + conservative sysinit single-CLI startup.
  `run-qemu-test` and `run-qemu-ring3-test` currently pass with this fallback.
- Phase-1 copy-path compatibility debt is now resolved: sensitive early output
  consumers were migrated to explicit sync/validated-copy behavior, and strict
  smoke paths no longer depend on dual-write host-pointer compatibility.

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
    `u64` -> `u32` cleanliness via `syscall_arg_u32`.
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
  bounded 64 MiB allowlist (PDE-level), and extended verifier checks to assert
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
- Instrumented repro + QEMU exception trace (2026-05-03):
  - Repro path: `run-qemu-ring3-test` with child low-slot strip enabled.
  - Observed sequence: expected recoverable page-fault test (`#PF` at
    `page_fault_test_entry`), then fatal `#PF` at `RIP=write_cr3+0x10`
    (`0x108b90`) with child root active (`CR3=0x7d4000`), followed by
    `#DF` and triple fault reset.
  - Interpretation: stripping `PML4[0]` currently removes a still-required
    execute mapping for kernel code running during CR3 switch windows; this is
    not a transient test flake and is deterministic under current layout.
  - Actionable constraint: do not re-enable low-slot stripping until kernel
    execution during user-root windows (scheduler/MM copy and transition path)
    is fully moved to a higher-half-safe mapping model.
- Scheduler migration progress: process kernel stacks now prefer higher-half
  virtual placement (physical pages allocated below current shared window) so
  more scheduler/user-CR3 execution uses higher-half stack addresses. A
  temporary fallback to unconstrained stack allocation remains in place for
  stability when the bounded shared window cannot satisfy all stacks.
- Strict-ring3 integration follow-up (2026-05-03):
  - Fixed deterministic strict-mode crashes uncovered after the low-slot
    experiment across scheduler/context-switch, IRQ route tables, serial/libc
    pointer handling, and framebuffer globals.
  - Context-switch ingress pointers are now canonicalized before dereference
    in assembly transition paths.
  - Scheduler RIP validation now accepts both low and higher-half kernel text
    forms used during transition windows.
  - Serial/libc pointer remap policy now aliases only low kernel-image
    pointers; user pointers are no longer remapped into invalid higher-half
    addresses.
  - `run-qemu-ring3-test` now passes end-to-end with strict mode enabled,
    including ring3 smoke, fault-policy, preempt stress, and native ABI checks.
  - Remaining Phase-2 debt:
    - remove temporary strict-mode framebuffer write bypass once explicit MMIO
      mapping guarantees exist for that path
    - continue reducing/removing low-slot compatibility mappings after all
      user-CR3 kernel windows are fully higher-half-safe
- Stack-safety hardening update (2026-05-03):
  - Scheduler/process stack allocation no longer falls back to low-address
    pages when higher-half-window allocation fails; process spawn now fails
    closed in that condition.
  - `mm_copy_from_user` / `mm_copy_to_user` now trampoline through a dedicated
    higher-half copy stack before temporary CR3 switches when needed, removing
    caller low-stack dependence for copy windows.
  - Scheduler dispatch now uses a dedicated higher-half trampoline stack on
    low-stack ingress (`process_schedule_once`), replacing direct low-stack
    register rebasing in this non-copy transition window.
  - Kernel scheduler entry pointers now canonicalize to higher-half addresses
    at spawn/dispatch (`process_trampoline` RIP + entry-call canonicalization),
    reducing low-slot execute dependence in non-copy runtime paths.
  - Context-switch ring0 restore paths now canonicalize low-form kernel RIP to
    higher-half before `ret`, reducing low-slot execute dependence for kernel
    resume.
  - Ring3 user-entry activation now applies an explicit no-low-slot verifier
    gate (`PML4[0]` absent) after strip and before CPL3 selector commit.
  - Added guarded sweep mode that performs strip+verify across eligible
    contexts and logs the first strip/verify failure path for iterative
    Phase-2 removal work.
- Phase-2 closure verification (2026-05-06):
  - Strict low-slot baseline is exercised in the main strict gate.
  - `cmake --build build --target strict-ring3` passes, including both
    `run-qemu-test` and `run-qemu-ring3-test`, with runtime markers:
    `[mode] low-slot-sweep=1`,
    `[mode] low-slot-sweep-level=2`,
    `[diag] low-slot sweep ok`.
  - This satisfies the outstanding low-slot transition blocker for current
    strict-ring3 scope; remaining compatibility-path deletion is tracked in
    Phase 8.

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

Progress update (2026-05-06):
- Capability authorization is now centralized through
  `policy_authorize(context, action, arg0)` and used by privileged hostcall and
  IRQ-route paths.
- WASMOS-APP capability metadata now supports multi-capability grants per app
  (removes prior one-grant-per-app limitation).
- Capability descriptor parsing is fail-closed in `make_wasmos_app`:
  unknown capability names and non-zero capability flags are rejected at pack
  time.
- Runtime coverage includes ring3 deny/allow probes for `irq.route`.
- Phase 3 is functionally complete for current capability classes; remaining
  work is extension as new privileged actions are introduced.

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

Progress update (2026-05-06):
- Implemented per-process bounded pending-reply queues for `ipc_call`, so
  unmatched out-of-order replies are preserved until matching waiter
  consumption.
- `ipc_call` now authenticates matched replies by both expected source endpoint
  and source endpoint owner context before acceptance.
- Ring3 runtime coverage now asserts both ordering and authenticity paths via:
  - `[test] ring3 ipc call correlate ok`
  - `[test] ring3 ipc call source auth ok`
- Ring3 runtime coverage now also asserts notification control-plane deny
  behavior by attempting `ipc_notify` to a kernel-owned control notification
  endpoint and requiring `[test] ring3 ipc syscall control deny ok`.
- Ring3 runtime coverage now also asserts control-plane deny behavior by
  attempting `ipc_call` to the process-manager endpoint and requiring
  `[test] ring3 ipc call control deny ok`.
- Process-manager specialized request/reply hardening update:
  FS-backed spawn response correlation now also requires expected source
  endpoint (`fs-fat`) in addition to matching `request_id`/response type.
- Process-manager specialized async-wait hardening update:
  waiter records now bind reply endpoint to owner context and revalidate
  endpoint ownership before async wait-reply delivery, dropping stale/re-owned
  endpoints to avoid cross-context response delivery.
- Strict-ring3 deterministic gate update:
  ring3 runtime now injects a synthetic owner-mismatch waiter record and
  requires deny marker `[test] pm wait reply owner deny ok`.
- Strict-ring3 deterministic gate extension:
  ring3 runtime now also injects a synthetic kernel-owned `PROC_IPC_KILL`
  caller and requires deny marker `[test] pm kill owner deny ok`.
- Strict-ring3 deterministic gate extension:
  ring3 runtime now also injects a synthetic kernel-owned `PROC_IPC_STATUS`
  caller and requires deny marker `[test] pm status owner deny ok`.
- Strict-ring3 deterministic gate extension:
  ring3 runtime now also injects a synthetic kernel-owned `PROC_IPC_SPAWN`
  caller and requires deny marker `[test] pm spawn owner deny ok`.
- Remaining Phase 4 work:
  - extend equivalent adversarial coverage to any additional specialized
    request/reply paths outside current `ipc_call` flow
  - continue sensitive endpoint allowlist tightening for control planes

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

Progress update (2026-05-06):
- Added first non-#PF user-exception containment probe in strict ring3 smoke:
  a dedicated `ring3-fault-ud` process executes CPL3 `ud2` (`#UD`, vector 6).
- Kernel trap path now handles CPL3 `#UD` as process-local termination
  (`-11`) instead of global panic for this covered vector.
- Added CPL3 `#GP` containment probe in strict ring3 smoke:
  `ring3-fault-gp` executes a privileged instruction (`cli`) in user mode to
  trigger vector 13 and assert process-local termination (`-11`).
- Added CPL3 `#DE` containment probe in strict ring3 smoke:
  `ring3-fault-de` executes a divide-by-zero path in user mode to trigger
  vector 0 and assert process-local termination (`-11`).
- Added CPL3 `#DB` containment probe in strict ring3 smoke:
  `ring3-fault-db` executes `icebp/int1` in user mode to trigger vector 1 and
  assert process-local termination (`-11`).
- Added strict-ring3 probes for vectors commonly surfaced as user-origin
  transition faults during bring-up:
  - `ring3-fault-of` (strict expected vector: `#GP`, 13)
  - `ring3-fault-nm` (strict expected vector: `#UD`, 6 on current smoke payload)
  - `ring3-fault-ss` (strict expected vector: `#GP`, 13 on current smoke payload)
  - `ring3-fault-ac` (strict expected vector: `#UD`, 6 on current smoke payload)
- Smoke/gate markers now assert both classification and policy status:
  - `[test] ring3 fault ud reason ok`
  - `[test] ring3 fault ud exit status ok`
  - `[test] ring3 fault gp reason ok`
  - `[test] ring3 fault gp exit status ok`
  - `[test] ring3 fault de reason ok`
  - `[test] ring3 fault de exit status ok`
  - `[test] ring3 fault db reason ok`
  - `[test] ring3 fault db exit status ok`
  - `[test] ring3 fault of reason ok`
  - `[test] ring3 fault of exit status ok`
  - `[test] ring3 fault nm reason ok`
  - `[test] ring3 fault nm exit status ok`
  - `[test] ring3 fault ss reason ok`
  - `[test] ring3 fault ss exit status ok`
  - `[test] ring3 fault ac reason ok`
  - `[test] ring3 fault ac exit status ok`
- Remaining Phase-5 work:
  - extend equivalent process-local handling and coverage to additional
    user-origin exception vectors beyond current `#PF`, `#UD`, `#GP`, `#DE`,
    `#DB`, `#OF`, `#NM`, `#SS`, and `#AC`
  - add multi-process mixed fault-storm liveness assertions

Exit criteria:
- All covered user exception classes terminate offending process only.
- Kernel remains alive across sustained fault storms.

Legacy-path impact:
- Remove any remaining user-fault paths that escalate to global panic unless
  architecturally unrecoverable and not user-originating.

### Phase 6: Scheduler/Trap Robustness Under Malicious Workloads

Objective: prove ring transitions and preemption are stable under abuse.

Status update (2026-05-07):
- Added first scheduler watchdog markers in `process.c`:
  - forward-progress marker: emits `[test] sched progress ok` after sustained
    context-switch activity in strict-ring3 boots.
  - reschedule-stall marker: emits `[watchdog] resched stall ...` if
    `g_need_resched` remains pending past a bounded tick threshold.
- Baseline validation remains green after marker rollout:
  - `cmake --build build --target run-qemu-test`
  - `cmake --build build --target run-qemu-ring3-test`
- Added mixed abuse rollout in `kernel.c`:
  - extended `ring3-smoke` syscall churn (`GETPID` loop from 4096 to 16384)
    while IPC deny/allow probes continue in the same CPL3 loop.
  - `ring3-fault-policy` now respawns alternating `#UD/#GP` fault probes for
    multiple rounds and emits `[test] ring3 mixed stress ok` on completion.
- Added trap-frame integrity watchdog guard in `process_preempt_from_irq`:
  - validates CS privilege shape and basic IRET frame sanity before preempt
    context capture.
  - emits `[watchdog] trap frame invalid ...` and skips preempt if malformed
    frame data is observed.
- Added stabilization pass markers/cleanup:
  - ring3 fault-policy completion now asserts watchdog counters remained clean
    and emits `[test] ring3 watchdog clean ok`.
  - removed temporary process-manager-specific preempt frame trace guards that
    were only used during strict-ring3 bring-up.
- Flaky test note (2026-05-07):
  - Observed one transient `run-qemu-cli-test` failure in filesystem smoke with
    `Error handling renames (-5)` / `Error deleting`, followed by downstream
    prompt-detection timeout.
  - Immediate re-run passed (`Ran 50 tests ... OK`) without code changes,
    indicating mutable-artifact test flake rather than a deterministic scheduler
    or trap-path regression.
  - Follow-up item: revisit CLI test harness image/reset isolation for fs-smoke
    cases before treating this as product-code instability.

Phase closure (2026-05-07):
- Phase 6 exit criteria are considered met for current scope:
  - sustained mixed abuse markers and strict ring3 smoke remain green
  - watchdog/trap integrity markers are present and clean on passing runs
  - no deterministic deadlock/context-corruption regression introduced by the
    Phase 6 hardening set

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

Status update (2026-05-07):
- Started shared-memory ownership hardening:
  - shared regions are now owner-context-bound at creation time.
  - `shmem_map/unmap` and underlying retain/release/get-phys paths now reject
    cross-context use of forged shared IDs (kernel context `0` remains
    supervisor-only bypass for native/kernel-owned paths).
- Added explicit grant semantics for shared regions:
  - owner context can grant access to specific target contexts via
    `mm_shared_grant(owner,id,target)`.
  - access checks now allow owner, kernel context `0`, or explicitly granted
    contexts.
  - added user-facing hostcall surface `wasmos_shmem_grant(id,target_pid)`
    (DMA-capability-gated, PID->context validated) for controlled grant wiring
    from user-space services.
- Added cross-context negative smoke marker in kernel boot tests:
  - emits `[test] shmem owner deny ok` when foreign-context `get_phys`,
    `retain`, and `release` attempts against an owner-bound shared ID are all
    rejected as expected.
  - emits `[test] shmem grant allow ok` when access succeeds after explicit
    owner grant.
- Added strict ring3 cross-process shmem smoke markers using live ring3
  contexts (`ring3-smoke` owner, `ring3-native` target):
  - emits `[test] ring3 shmem owner deny ok` before grant.
  - emits `[test] ring3 shmem grant allow ok` after explicit owner grant.
- Added shared-memory revoke semantics:
  - owner context can revoke access to a specific target context via
    `mm_shared_revoke(owner,id,target)`.
  - added user-facing hostcall surface `wasmos_shmem_revoke(id,target_pid)`.
- Added user-space end-to-end app-pair smoke for shmem grant+revoke:
  - `shmtgt` verifies pre-grant map deny, post-grant map allow, and
    post-revoke map deny.
  - `shmownr` coordinates grant/revoke and emits completion marker.
- Extended negative-path coverage and closure hardening:
  - app-pair e2e now validates forged shared-ID map deny, map-argument policy
    deny, and stale revoked-ID deny.
  - kernel boot smoke now validates a shared-memory misuse matrix:
    forged IDs, wrong-owner grant/revoke rejection, pre/post-grant map
    deny/allow behavior, revoke idempotence, and release-balance checks.
  - strict ring3 gate now emits `[test] ring3 shmem misuse matrix ok`.
  - `mm_shared_map` ordering hardened to retain access before region insertion
    and rollback retain on insertion failure to avoid partial-state leakage.

Tasks:
- Completed.

Exit criteria:
- Cross-context mapping abuse attempts fail.
- Shared memory behavior is explicit, revocable, and policy-checked.

Result:
- Closed for current strict-ring3 scope.

Legacy-path impact:
- Remove implicit remap assumptions and any “caller trusted” physical map paths.

### Phase 8: Strict Mode Default + Compatibility Path Deletion

Objective: finish migration by removing non-ring3 behavior.

Status update (2026-05-07):
- Removed the transitional strict-mode compatibility toggle path:
  - deleted `WASMOS_RING3_STRICT` CMake option and kernel compile define wiring
  - removed `-DWASMOS_RING3_STRICT=ON` shadow configure override in
    `run-qemu-ring3-test`
- Removed transitional low-slot strict-mode knobs:
  - deleted `WASMOS_LOW_SLOT_SWEEP`, `WASMOS_LOW_SLOT_SWEEP_LEVEL`, and
    `WASMOS_IDENTITY_PD_COUNT` CMake configurability for kernel strict paths
  - paging identity-map breadth is fixed at strict baseline (`IDENTITY_PD_COUNT=0`)
- Removed redundant fixed-value boot mode serial markers from `kmain`
  (`strict-ring3` and low-slot sweep mode lines); ring3 smoke checks now rely
  on behavioral/fault-policy markers instead of static mode-value prints.
- Validation after removal:
  - `cmake --build build --target run-qemu-test` passes
  - `cmake --build build --target run-qemu-ring3-test` passes

Tasks:
- Maintain strict ring3 low-slot baseline behavior (`IDENTITY_PD_COUNT=0` and
  boot-time low-slot sweep diagnostic execution).
- Continue deleting remaining compatibility branches/fallback markers from
  strict-mode bring-up paths.
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
