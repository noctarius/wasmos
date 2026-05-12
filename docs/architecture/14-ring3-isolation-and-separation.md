## Ring3 Isolation and Kernel/User Separation

### Purpose
This document describes the current WASMOS ring3 isolation model and how the
kernel/user-space separation is enforced across memory, entry points, IPC, and
fault handling.

### Threat Model
The baseline threat is a compromised or malformed user-space process (WASM or
native payload) attempting to:
- crash or hang the kernel
- read/write/execute kernel memory
- spoof privileged IPC/control-plane operations
- abuse shared-memory paths for cross-process access

The target property is containment: offending user-space is terminated while the
kernel and other processes continue.

### Separation Model
- Kernel executes in privileged mode and owns scheduler, memory manager,
  interrupt/trap handling, and core IPC transport.
- User workloads run in per-process contexts with dedicated user mappings.
- Kernel/user crossings are constrained to explicit syscall/hostcall/IPC
  boundaries.

### Memory Isolation
- Per-process address spaces are used with dedicated user slot mappings.
- User-root verification enforces low-slot stripping and constrained kernel
  transition mapping footprint before CPL3 entry.
- Low-slot sweep diagnostics run during boot to strip/verify eligible contexts
  and emit explicit failure markers on violation.
- Identity-map breadth used for transition support is fixed to strict baseline
  (`IDENTITY_PD_COUNT=0`).
- User-slot page mapping now requires explicit `MEM_REGION_FLAG_USER` in
  mapping calls; implicit user-slot compatibility flag bridging has been
  removed from the page-map path.

### Kernel Entry Safety
- Syscall argument width checks reject lossy `u64 -> u32` truncation.
- Signed status syscall arguments (`EXIT`, `THREAD_EXIT`) must be valid
  sign-extended 32-bit values; lossy 64-bit forms are rejected.
- Hostcall endpoint arguments that cross signed/unsigned boundaries are
  validated before conversion (for example `wasmos_serial_register` rejects
  negative endpoint IDs).
- Pointer-bearing hostcalls use explicit user-VA resolution and range checks.
- Copy semantics rely on validated copy helpers (`mm_copy_from_user`,
  `mm_copy_to_user`) rather than direct user-pointer dereference.
- Remaining coherence-bridge usage is treated as a bounded compatibility layer
  and tracked as follow-up hardening work.

### Privilege and Capability Enforcement
- Privileged operations are deny-by-default and capability-gated.
- Capability metadata is parsed fail-closed at app packaging/load boundaries.
- Sensitive operations (e.g. IRQ route/control paths) are validated by policy
  plus ownership/context checks.

### IPC and Control-Plane Isolation
- Endpoint ownership and sender-context checks are enforced for relevant IPC
  paths.
- Pending-reply queues preserve out-of-order replies until proper match/drop.
- Reply acceptance is correlated by request identity and authenticated source.
- Control-plane endpoints include explicit deny-path assertions in ring3 smoke.

### Fault Containment Policy
- User-origin fault classes covered by current strict smoke terminate only the
  faulting process with deterministic policy status (`-11`).
- Covered exception/fault probes currently include:
  - `#PF`, `#UD`, `#GP`, `#DE`, `#DB`
  - strict expected-vector probes for `ring3-fault-of`, `ring3-fault-nm`,
    `ring3-fault-ss`, and `ring3-fault-ac` under current payload behavior
- Fault-policy checks assert both reason markers and termination status markers.

### Scheduler and Trap Robustness
- Strict ring3 validation includes watchdog markers for forward progress and
  trap-frame integrity.
- Mixed stress runs combine syscall churn, IPC deny/allow probes, and repeated
  fault probes to validate kernel liveness under abuse.

### Shared Memory / Pager Isolation
- Shared regions are owner-context bound.
- Access requires owner, kernel supervisor context, or explicit grant.
- Grant/revoke semantics are explicit and verified by kernel and app-pair smoke.
- Misuse matrices check forged IDs, wrong-owner operations, stale revoke usage,
  and retain/release balance behavior.

### Validation Gates
Primary strict regression gates are:
- `cmake --build build --target run-qemu-test`
- `cmake --build build --target run-qemu-ring3-test`
- `cmake --build build --target run-qemu-cli-test`

These gates assert behavioral markers for memory isolation, IPC isolation,
shared-memory policy, fault containment, and scheduler liveness.

### Deferred Hardening Backlog
The following are intentionally deferred beyond initial strict closure:
- complete remaining hostcall split-view/coherence-bridge cleanup where
  practical
- extend specialized IPC request/reply adversarial coverage
- extend user-origin exception coverage beyond current set
- add dedicated multi-process mixed fault-storm liveness suite
- harden CLI fs-smoke harness image/reset isolation to eliminate known flake

### Design Status
The ring3 strict-mode/default isolation migration (Phases 0-8) is closed for
current scope. Ongoing work is incremental hardening and additional adversarial
coverage, not compatibility-path migration.
