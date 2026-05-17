# Architecture Notes

This document is the architecture index and entry point for the repository.
Detailed design and implementation status now live in focused documents under
`docs/architecture/`.

IMPORTANT: Keep this file and `README.md` up to date with every prompt execution
and code iteration, but do not use as a changelog.
IMPORTANT: Create a git commit after each prompt iteration.

## Current Status
- This file is now intentionally a concise index, not a release-changelog.
- Ring-3 strict isolation/hardening, threading phase rollout, DMA rollout,
  filesystem/PM service discovery, and CLI/runtime updates are tracked in the
  dedicated docs under `docs/architecture/`.
- Threading remains scope-complete through Phase D, but not production-complete:
  see the explicit closure checklist in
  `docs/architecture/15-threading-and-lifecycle.md` section 17.
- Recent threading runtime hardening (user-thread kernel-stack setup for
  `THREAD_CREATE` and syscall frame/context synchronization for yield/block
  paths) is documented in `docs/architecture/15-threading-and-lifecycle.md`.

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
- [DMA Transfers](architecture/16-dma-transfers.md)
- [Graphics, Framebuffer, and Compositor](architecture/17-graphics-framebuffer-and-compositor.md)

## Update Rules
- Update the relevant feature document(s) in `docs/architecture/` when behavior
  changes.
- Keep cross-document references consistent across `README.md`, `docs/TASKS.md`, and
  architecture docs.
- Prefer appending concrete implementation notes over vague roadmap text.
