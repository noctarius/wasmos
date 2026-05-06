## Process Model

### Process Lifecycle
The implemented process states are:
- `READY`
- `RUNNING`
- `BLOCKED`
- `ZOMBIE`

Typical transitions:
- Spawn: `READY`
- Dispatch: `READY -> RUNNING`
- Time slice expiration: `RUNNING -> READY`
- IPC wait or explicit block: `RUNNING -> BLOCKED`
- Wakeup: `BLOCKED -> READY`
- Exit: `RUNNING -> ZOMBIE`
- Reap: `ZOMBIE -> UNUSED`

### Process Ownership
- The kernel-owned `init` task is the root parent for kernel-spawned processes.
- The process manager owns the `proc` IPC endpoint and mediates spawn/wait/kill/status.
- PM-created processes get their own runtime context and stack/heap sizing from
  WASMOS-APP metadata.

### Runtime Contexts
Each process is associated with a runtime context that tracks:
- linear memory
- stack
- heap
- IPC region placeholders
- device region placeholders

This is the structural precursor to full address-space separation.

## IPC Model

### Core Message Format
All IPC messages share the same fixed register-sized layout:

```c
type
source
destination
request_id
arg0
arg1
arg2
arg3
```

Small control traffic stays in-message. Bulk payloads are expected to move to
shared buffers plus synchronization messages.

### Implemented Rules
- Endpoints have an owning context.
- `ipc_send_from` requires a non-kernel sender to own its source endpoint.
- `ipc_recv_for` requires a non-kernel receiver to own the destination endpoint.
- Enqueueing a message can wake a process blocked on the destination endpoint.
- Message queues are bounded and protected by spinlocks.
- Endpoint table capacity is currently 128 and endpoints owned by a process
  context are released when that process is reaped, preventing table exhaustion
  across repeated short-lived app runs.

### Error Model
Current IPC status codes:
- `IPC_OK`
- `IPC_EMPTY`
- `IPC_ERR_INVALID`
- `IPC_ERR_PERM`
- `IPC_ERR_FULL`

### Direction of Future Growth
The current transport is intentionally small. The architecture still needs:
- notification objects distinct from synchronous request/reply IPC
- shared-memory bulk transfer paths
- service-level allowlists / badges
- async server helpers for multi-hop service stacks
- richer endpoint naming / registry rules

## Interrupts and Timer Integration
- The kernel remaps the legacy PIC and installs exception plus IRQ stubs.
- PIT IRQ0 is the active scheduler clock.
- The timer code emits a one-time visible initialization message
  (`[timer] pit init`).
- Periodic timer tick progress markers are now trace-only and hidden when
  `WASMOS_TRACE=OFF`.

The current interrupt model is still PIC-based. APIC/IOAPIC support remains open.

