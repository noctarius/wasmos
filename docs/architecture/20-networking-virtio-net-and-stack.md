## Networking via Virtio-Net and User-Space Stack

### Goal
Introduce a deterministic, minimal networking baseline for WASMOS using:
- a dedicated `virtio-net` driver process for device transport
- a separate user-space network stack service for L2/L3/L4 protocol state
- IPC-based socket-style APIs for apps/services

This preserves the microkernel split (mechanism in kernel, policy/protocol in
user space) and aligns with the existing device-manager and service-registry
startup model.

### Scope and Non-Goals
In scope:
- explicit QEMU NIC/network configuration for deterministic bring-up
- `virtio-net` PCI driver baseline (feature negotiation, queue init, RX/TX)
- network stack service baseline (ARP, IPv4, ICMP echo, UDP, then TCP)
- app-facing IPC contract for datagram and stream sockets
- boot-time integration through existing `device-manager` policy rules

Out of scope for initial rollout:
- IPv6
- TLS in kernel or first-pass stack service
- high-throughput offload features (TSO/GSO/GRO/LRO)
- multi-NIC routing and advanced firewall/NAT policy

### Current Baseline
- No explicit NIC model is configured in `run-qemu*`; QEMU defaults are used.
- `virtio-serial` already exists as a PCI-matched WASM service and proves the
  transport pattern for early virtio device bring-up.
- DMA capability plumbing and borrow-buffer DMA lifecycle exist and can be
  reused by `virtio-net` for packet buffers in later phases.

### Design Principles
- Deterministic first: always pin explicit QEMU NIC model + netdev backend.
- Strict split of responsibilities:
  - `virtio-net` driver owns transport mechanics (queues/interrupts/buffers).
  - net-stack service owns protocol state and socket semantics.
- Fail closed: when link/queue/stack is not ready, return explicit status
  errors; avoid silent drops where possible.
- Minimal ABI first: add only the smallest IPC surface needed for useful
  connectivity and debugging.
- Keep bootstrap stable: networking startup must not block non-network boot
  milestones.

### QEMU Bring-Up Contract (Normative)
Default developer/test profile should use explicit user-mode networking and
virtio NIC:

```sh
-netdev user,id=net0
-device virtio-net-pci,netdev=net0
```

Policy:
- Never rely on implicit NIC defaults for validation targets.
- Keep a compatibility toggle to boot with a legacy model (`e1000`) when
  bisecting regressions.

### Component Model

#### 1. `virtio-net` Driver Service
Responsibilities:
- discover and bind `virtio-net` PCI function
- negotiate required features (minimal baseline first)
- initialize RX/TX virtqueues and interrupt handling
- publish link/MAC info and packet ingress/egress IPC endpoints

Non-responsibilities:
- no TCP retransmission logic
- no ARP/IP routing tables
- no socket lifecycle semantics

#### 2. `net-stack` Service
Responsibilities:
- own protocol state machines and packet classification
- maintain ARP cache, IPv4 config, ICMP, UDP, and TCP state
- expose socket-style IPC to clients
- mediate packet flow to/from `virtio-net`

Non-responsibilities:
- no direct PCI/virtqueue access
- no privileged hardware config outside driver IPC contract

#### 3. Client Apps/Services
Responsibilities:
- use net-stack IPC APIs for open/bind/connect/send/recv/close
- handle explicit non-blocking/retry statuses

### IPC Contract (v0 Draft)

#### Driver <-> Stack IPC
- `NETDRV_IPC_LINK_GET`
  - returns link state, MTU, MAC
- `NETDRV_IPC_TX_FRAME`
  - payload: raw Ethernet frame bytes
  - result: accepted/queue-full/error
- `NETDRV_IPC_RX_POLL`
  - returns next received frame (or empty)
- `NETDRV_IPC_STATS_GET`
  - counters: rx/tx packets, drops, errors

#### Stack <-> Client IPC
- `NET_IPC_SOCKET_OPEN` (`UDP` or `TCP`)
- `NET_IPC_BIND`
- `NET_IPC_CONNECT` (TCP and connected UDP)
- `NET_IPC_SEND`
- `NET_IPC_RECV`
- `NET_IPC_CLOSE`
- `NET_IPC_POLL`

Conventions:
- request IDs must be preserved across async completion replies
- all ops return explicit status codes (`ok`, `would_block`, `invalid`,
  `not_ready`, `denied`, `io_error`)
- bounded payload sizes; caller retries with flow control

### Buffer and DMA Model
Phase A/B (initial):
- start with copy-based frame exchange for correctness and simpler debugging
- use fixed-size packet buffers and conservative queue depth

Phase C+ (optimization):
- adopt borrow-buffer + DMA mapping path for zero/low-copy data movement
- enforce existing DMA capability windows and direction checks
- keep deterministic fallback to copy path when DMA is denied/unavailable

### Stack Implementation Options

#### Option A: `lwIP` (Recommended Initial Path)
Why:
- C-first integration fits current service/driver codebase
- mature IPv4/UDP/TCP behavior with clear raw/netif integration seams
- lower FFI friction for initial WASMOS service integration

Tradeoffs:
- requires disciplined memory/pbuf configuration
- weaker compile-time safety than Rust-first approach

#### Option B: `smoltcp` (Rust-First Alternative)
Why:
- memory-safe stack logic and explicit state-machine style
- good for constrained embedded networking experiments

Tradeoffs:
- Rust service integration and C ABI boundary increase initial complexity
- feature scope differs from full legacy stacks and may require protocol-policy
  adaptations

Decision:
- start with `lwIP` for first end-to-end baseline, then re-evaluate `smoltcp`
  after the driver/IPC contracts are stable.

### Rollout Plan

Phase 0: Deterministic platform wiring
- Add explicit QEMU netdev + NIC model in `run-qemu*` targets.
- Add build toggle for `virtio-net` vs `e1000` model selection.
- Add boot markers for NIC/device visibility.

Done gate:
- `run-qemu-test` remains green with explicit NIC config.

Phase 1: `virtio-net` transport baseline
- Add `virtio-net` driver skeleton package and devmgr match rule.
- Implement PCI probe, basic feature negotiation, queue init, RX/TX loop.
- Register driver endpoint (for example `virtio.net`).

Done gate:
- driver reports MAC/link and can TX/RX raw frames in loopback/smoke path.

Phase 2: net-stack service baseline (L2/L3/ICMP/UDP)
- Add `net-stack` service package and startup policy.
- Integrate chosen stack core (`lwIP` first path).
- Implement ARP + IPv4 + ICMP echo + UDP send/recv.
- Add simple UDP echo sample app for validation.

Done gate:
- guest ping/UDP echo works on QEMU user-mode network.

Phase 3: TCP baseline
- Add minimal TCP connect/listen/accept/send/recv/close behavior.
- Add timeout/retry/close-path handling and explicit error mapping.
- Add TCP echo client/server smoke tests.

Done gate:
- stable TCP echo in `run-qemu` validation without boot regressions.

Phase 4: hardening + performance
- Add negative-path tests (queue full, malformed frames, link down, stack
  restart).
- Add counters/diagnostics (`netstat`-style endpoint later).
- Optional DMA-backed fast path rollout with fallback guarantees.

Done gate:
- regression matrix passes and no startup chain liveness regressions.

### Validation Matrix
- Baseline boot regression:
  - `cmake --build build --target run-qemu-test`
- Networking smoke (new target, sequential with existing QEMU tests):
  - boot + NIC detect + net-stack register + ICMP echo + UDP echo
- TCP smoke:
  - TCP connect + echo + close
- Negative behavior:
  - queue saturation returns `would_block`
  - link-down path returns `not_ready`
  - malformed frame path rejected with explicit status

### Risks and Mitigations
- Risk: large first integration scope causes boot instability.
  - Mitigation: strict phased rollout and isolated networking smoke target.
- Risk: ambiguous ownership between driver and stack.
  - Mitigation: lock clear transport-vs-protocol boundary and keep socket API
    stack-owned only.
- Risk: DMA path introduces hard-to-debug faults early.
  - Mitigation: copy-first correctness path, DMA only after baseline tests are
    stable.

### Open Decisions
- Final endpoint naming for driver/stack services (`virtio.net`, `net.stack`,
  etc.).
- Static IP first vs DHCP-first in initial user-mode networking profile.
- Whether TCP accept/listen should be in phase 3 baseline or phase 4 hardening.

### Task Checklist (Execution Order)
1. Make QEMU NIC settings explicit in all run/test targets.
2. Add `virtio-net` driver package skeleton and devmgr rule.
3. Land driver RX/TX baseline and driver diagnostics markers.
4. Add `net-stack` service with ARP/IPv4/ICMP/UDP.
5. Add TCP baseline and protocol-level regression tests.
6. Evaluate DMA optimization path behind guarded rollout.
