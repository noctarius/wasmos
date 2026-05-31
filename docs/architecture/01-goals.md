## Goals

### Purpose

WASMOS is a minimal x86_64 UEFI OS built as an experimentation platform, not a
production system. Its purpose is to explore what a WASM-first microkernel OS
looks like in practice: a small, auditable kernel core with user-space services
and drivers that are isolated WASM programs communicating over explicit IPC
contracts, running on real (virtualized) hardware.

The system is deliberately kept minimal. Features are introduced because they
serve a clear architectural goal or prove a design hypothesis, not to reach
feature parity with existing OSes. Complexity that does not contribute to
understanding the design is a liability.

---

### Kernel Responsibilities

The kernel does exactly five things and no more:

1. **Scheduling** — preemptive, timer-driven, single-core. Provides deterministic
   round-robin with ring3-safe preemption trampolines.
2. **Memory management** — physical frame allocation, per-process page tables,
   higher-half kernel alias, user-pointer copy with permission checks, shared-
   memory primitives.
3. **IPC transport** — fixed-layout message passing with bounded queues, endpoint
   ownership, and borrow-buffer bulk transfer. The kernel routes messages; it
   does not interpret them.
4. **Interrupts and exceptions** — legacy PIC, PIT timer, trap dispatch. User-mode
   faults terminate only the faulting process. Kernel-mode faults remain fatal.
5. **Capability enforcement** — per-context grants for I/O ports, IRQ routing,
   MMIO, DMA, and system control. The kernel checks grants at every privileged
   entry point; it does not decide policy.

Everything else — device discovery, driver matching, filesystem routing, display
policy, network protocol state — lives in user space.

---

### User-Space Model

**Services and apps are WASM programs.** WASM provides a portable, sandboxed
execution environment that can be compiled from multiple languages (C, Zig, Go,
Rust, AssemblyScript). The kernel hosts WASM execution via `wasm3` and exposes
kernel primitives as hostcalls rather than syscalls where WASM can call them
directly. WASM isolation means a driver or service crash cannot corrupt kernel
state without a kernel bug.

**Native ELF drivers are the escape hatch.** When a component genuinely needs
direct hardware register access, DMA, or capabilities that WASM cannot express
efficiently, it can be packaged as a native ELF payload inside the WASMOS-APP
format with `FLAG_NATIVE`. Native drivers run through a kernel function-table
ABI and are subject to the same capability grants as WASM services. Native is
not the default; it is the exception.

**All components communicate over IPC.** There are no in-kernel calls between
subsystems at the service level. A driver publishes an endpoint and a contract;
clients call it. This makes component boundaries explicit, restart semantics
tractable, and the dependency graph visible. IPC message types are statically
defined in shared headers; undocumented side channels are not a supported design
pattern.

**Capability grants are explicit and deny-by-default.** A driver that needs to
access I/O ports must be granted that right at spawn time by `device-manager`.
A service that needs to route IRQs must hold `irq.route` plus a per-app IRQ-line
allowlist entry. Nothing is ambient. The goal is that reading a driver's
capability profile fully describes its hardware access footprint.

**The WASMOS-APP package format (.wap) is the unit of deployment.** Apps,
services, and drivers are packaged identically and loaded by the process
manager. Metadata inside the package declares entry point, capabilities, kind
flags, and runtime requirements. The process manager does not make assumptions
beyond what the package declares.

---

### Design Principles

These principles should be used to resolve design ambiguities. When two
approaches seem equivalent, prefer the one that better satisfies these:

**Determinism over cleverness.**
Prefer explicit, auditable behavior over implicit or emergent behavior. Boot
sequences, IPC flows, and driver startup should be predictable from reading the
rules, not from empirical observation. If the order of events matters, make it
explicit.

**Mechanism in kernel, policy in user space.**
The kernel provides primitives (send a message, map a page, grant a capability).
Policy (which service handles which device, which mount path gets which
filesystem, which process gets which privilege) lives in user-space services
(`device-manager`, `fs-manager`, the capability policy layer). When a kernel
subsystem starts making policy decisions, it is drifting.

**Fail closed.**
Default state is denied. Capabilities are not inherited. Undeclared behavior
returns an explicit error. A service that crashes should not take unrelated work
down with it. When in doubt, return an error rather than silently succeed with
reduced behavior.

**Explicit contracts at every boundary.**
IPC message types are defined in shared headers. Borrow handles are typed.
Capability grants are named and enumerated. Services register and look up
endpoints by name. There are no private channels, ambient state, or undocumented
coupling between components. Every interface that a component depends on is
something it must acquire explicitly.

**Small, focused changes.**
Each change should do one thing and leave the boot chain working. This is a
practice discipline, not a backwards-compatibility commitment: WASMOS has no
external users and no deployed installed base, so there is no cost to breaking
ABIs, renaming IPC message types, restructuring package layouts, or renumbering
hostcalls when doing so improves the design. Break things freely; just keep each
individual change reviewable and keep `run-qemu-test` green.

**Minimal surface area.**
Every line of kernel code is a line that must be correct. Keep the kernel small.
Do not add kernel mechanisms for things that can be done in user space. Do not
add user-space abstractions for things that can be done with existing primitives.
Three similar lines in service code is better than a premature kernel
abstraction.

**Observability over silence.**
Failures at component boundaries should emit structured diagnostic markers
(`[component] event detail`). Bring-up paths that succeed silently are harder
to debug than those that log a single-line confirmation. Temporary debug markers
are fine during development but should be removed from final commits unless they
carry permanent diagnostic value.

---

### Non-Goals

The following are explicitly outside the scope of WASMOS:

- **Production readiness.** WASMOS is a research/experimentation platform. It is
  not hardened for untrusted workloads, does not provide exploit mitigations
  beyond what the microkernel model naturally gives, and makes no reliability
  guarantees.
- **POSIX compatibility.** The libc shims provide just enough for WASM apps to
  compile; they are not a complete POSIX implementation. System call numbers,
  signal semantics, and POSIX process model details are not faithfully reproduced.
- **High-performance networking or storage.** The first networking stack is
  correctness-first (copy path, no TSO/GRO/LRO offload). The ATA driver uses
  PIO. These choices are intentional: correctness and auditability come before
  throughput optimization.
- **SMP.** The scheduler is single-core. SMP requires kernel-wide locking
  discipline and significant scheduler changes; it is deferred until the single-
  core model is fully solid.
- **Full IOMMU.** DMA capability windows provide a capability-level policy fence;
  hardware IOMMU (VT-d/AMD-Vi) is a future hardening step, not a current
  requirement.
- **Backwards compatibility.** WASMOS has no real users and no deployed installed
  base. Nothing is ever "in production." This means breaking changes to any
  interface — boot ABI, WASMOS-APP format, hostcall numbering, IPC message
  layouts, capability names, driver spawn contracts — are always acceptable if
  they improve the design. Do not preserve old interfaces out of theoretical
  compatibility concerns. There is nobody to break.
- **General-purpose OS workloads.** WASMOS does not aim to run Linux binaries,
  support arbitrary file systems, or provide a fully featured userland. It aims
  to prove the WASM-first microkernel model and serve as a base for further
  experiments.
