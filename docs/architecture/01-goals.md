## Goals
- Boot an x86_64 kernel through UEFI with a deterministic, auditable handoff.
- Keep the kernel small: scheduling, IPC, memory, interrupts, and runtime
  hosting are kernel responsibilities; policy lives in WASM services.
- Treat services and applications as isolated WASM programs and allow selected
  drivers to run as native ELF payloads when needed, all behind explicit IPC
  contracts instead of implicit in-kernel calls.
- Preserve a stable boot and process ABI while still allowing the system to
  evolve incrementally.

