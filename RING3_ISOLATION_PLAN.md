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

## Workstreams

### 1) Kernel Mapping Minimization in User CR3

Objective: remove broad kernel aliasing from user-owned address spaces.

Tasks:
- Replace fixed shared higher-half window copy with explicit required mapping
  set for user-root operation.
- Isolate trampoline/syscall/interrupt-required mappings from general kernel
  text/data exposure.
- Add compile/runtime assertions for "user roots contain only approved kernel
  ranges".

Acceptance:
- No generic higher-half bulk window mapped in user roots.
- Ring3 smoke + baseline boot tests pass.
- New test fails if unauthorized kernel range appears in a user root.

### 2) Complete User-Pointer Boundary Migration

Objective: eliminate direct/implicit user-memory dereference in kernel paths.

Tasks:
- Audit all syscalls, hostcalls, IPC marshal/unmarshal paths, and native-driver
  ABI entry points for pointer usage.
- Route all user memory access through `mm_copy_*` or a validated bridge with
  `mm_user_range_permitted`.
- Enforce strict overflow checks for every `(ptr, len, offset)` tuple.
- Add TODO/FIXME markers where migration is intentionally staged.

Acceptance:
- No kernel path dereferences user pointers directly.
- All pointer-bearing entry points have permission + overflow checks.
- Negative tests for invalid pointers and overflow cases pass.

### 3) Capability Enforcement: Strict Default-Deny

Objective: prevent user-space drivers/services from touching privileged kernel
resources without explicit capability grants.

Tasks:
- Remove compatibility "allow if unconfigured" fallbacks.
- Enforce explicit capability checks for IO port, IRQ routing, MMIO map, DMA,
  and privileged IPC control paths.
- Validate capability metadata at load time; fail closed on malformed policy.

Acceptance:
- Uncaped privileged operation attempts are denied deterministically.
- Capability-based allow/deny regression tests exist for each resource class.

### 4) IPC and Endpoint Hardening

Objective: ensure message transport cannot be abused to escalate privileges or
break isolation.

Tasks:
- Harden request/reply ownership checks and request-id correlation handling.
- Add per-service endpoint allowlists for sensitive control-plane endpoints.
- Resolve known TODO on unmatched IPC replies (preserve vs drop policy).

Acceptance:
- Cross-context spoofing/replay attempts fail.
- Out-of-order reply stress test passes without message confusion.

### 5) Fault-Containment Policy Completion

Objective: guarantee user-triggered exceptions never collapse kernel progress.

Tasks:
- Ensure all user exception vectors map to process-local termination policy
  where architecturally valid.
- Keep deterministic error markers and exit-policy assertions for each probe.
- Add repeated-fault storm test across many ring3 processes.

Acceptance:
- Kernel scheduler remains live after adversarial multi-process fault storms.
- Fault-policy tests assert both reason and exit semantics.

### 6) Scheduler / Trap Robustness Under Abuse

Objective: ensure trap/interrupt/preempt path cannot be destabilized by ring3
behavior.

Tasks:
- Verify TSS stack, trampoline return, and saved context integrity under rapid
  syscall/fault/preempt interleavings.
- Add stress harness for high-rate syscalls + IPC + forced faults.
- Add watchdog assertions for forward progress and no stuck resched loops.

Acceptance:
- Long-running abuse tests do not deadlock kernel scheduling.
- No context corruption markers under stress.

### 7) Memory Service / Pager Isolation Guarantees

Objective: ensure demand-mapping and shared-memory operations stay scoped.

Tasks:
- Verify fault mapping only within owning region definitions.
- Ensure shared-memory map/unmap cannot rebind arbitrary physical ranges into
  user contexts.
- Add negative tests for cross-context mapping attempts.

Acceptance:
- Cross-process/map-forgery attempts are denied.
- Shared memory remains explicit, auditable, and revocable.

## Test Plan Additions

Add ring3-focused adversarial tests (QEMU-driven):

1. Invalid pointer matrix per syscall/hostcall category.
2. Width/overflow matrix (`u64` truncation, `offset+len` wrap).
3. Cross-context memory and endpoint abuse attempts.
4. Capability deny matrix (PIO/MMIO/IRQ/DMA/privileged IPC).
5. Multi-process fault storm with scheduler liveness checks.
6. Long preempt+syscall stress with watchdog markers.

All above should be part of CI gating for strict isolation mode.

## Rollout Strategy

1. Keep staged compatibility only behind explicit build/runtime flags.
2. Introduce strict-isolation mode that is CI-default.
3. Flip production/default policy to strict after tests are stable.
4. Remove legacy compatibility paths after one full stabilization cycle.

## Definition of Done

The project reaches "full ring-3 isolation" when:

- Kernel mapping in user roots is minimal and explicitly allowlisted.
- All kernel entry points are safe against malformed user memory arguments.
- Capability policy is default-deny with no implicit allow fallbacks.
- User faults are always process-local and policy-asserted.
- Adversarial ring3 test suite passes consistently in CI.
- A compromised user-space process/driver cannot crash the kernel.
