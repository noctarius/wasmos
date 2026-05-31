## Process Model and IPC

This document covers the WASMOS process model, the IPC transport layer, the
`int 0x80` syscall ABI, the process manager (`proc` endpoint), and the
`libsys` event-loop helpers. The authoritative sources are
`src/kernel/ipc.c`, `src/kernel/syscall.c`, `src/kernel/process.c`,
`src/kernel/process_manager*.c`, and
`src/drivers/include/wasmos_driver_abi.h`.

---

### Process Lifecycle

Processes move through five states. Threads within a process have a
parallel state machine (see `05-scheduling-and-preemption.md`).

```
UNUSED ──spawn──► READY ──dispatch──► RUNNING
                  ▲                      │
                  │ wake                 ├──► BLOCKED  (IPC wait / process wait / thread join)
                  └──────────────────────┘
                                         │
                                         └──► ZOMBIE ──reap──► UNUSED
```

| `process_state_t`       | `process_block_reason_t` when BLOCKED     |
|-------------------------|-------------------------------------------|
| `PROCESS_STATE_UNUSED`  | —                                         |
| `PROCESS_STATE_READY`   | —                                         |
| `PROCESS_STATE_RUNNING` | —                                         |
| `PROCESS_STATE_BLOCKED` | `PROCESS_BLOCK_IPC`, `PROCESS_BLOCK_WAIT` |
| `PROCESS_STATE_ZOMBIE`  | —                                         |

A process becomes a zombie when its entry function returns
`PROCESS_RUN_EXITED`, when a `WASMOS_SYSCALL_EXIT` syscall fires, or when
`process_kill` is called from its parent. Zombie processes are either
explicitly reaped by a waiting parent (`process_wait`) or auto-reaped
(`auto_reap = 1`) when no waiter exists.

---

### Process Identity

Every process has three identifiers:

| Field        | Purpose                                         |
|--------------|-------------------------------------------------|
| `pid`        | Scheduler identity; used for wait, kill, status |
| `context_id` | Memory + capability + IPC ownership scope       |
| `main_tid`   | Main thread; additional threads can be spawned  |

`pid` and `context_id` are distinct: capability grants, endpoint ownership,
and MM regions are all keyed by `context_id`, not `pid`. Multiple threads
within one process share the same `context_id` and address space.

Process parentage is recorded in `parent_pid`. Kill and wait are restricted
to the parent process; a process cannot kill or wait on an unrelated process.

---

### Process Ownership and Trust

The kernel-owned `init` task (spawned by `kmain`) is the root parent for the
first generation of kernel-started processes. It spawns `device-manager` and
waits for FAT readiness before loading `sysinit`.

The process manager (`proc` endpoint) is the privileged mediator for all
further process lifecycle operations. It:

- Validates capability profiles at spawn time from WASMOS-APP metadata
- Assigns `io.port`, `irq.route`, `mmio.map`, `dma.buffer`, `system.control`
  grants per declared profile
- Maintains the service registry (name → endpoint)
- Enforces owner-context restrictions on wait, kill, and status

Processes spawned by the PM are children of the PM's kernel process. User
processes can request spawn via IPC; the PM validates and performs the actual
kernel `process_spawn_as` call, assigning the requester as the logical parent.

---

### `int 0x80` Syscall ABI

Ring3 user processes communicate with the kernel through `int 0x80` (DPL3
gate). The dispatcher is `x86_syscall_handler(syscall_frame_t *frame)`.

**Register convention:**

| Register | Role                                                                       |
|----------|----------------------------------------------------------------------------|
| `RAX`    | Syscall ID (see `wasmos_syscall_id_t`); primary return value on completion |
| `RDI`    | arg0                                                                       |
| `RSI`    | arg1                                                                       |
| `RDX`    | arg2 / secondary return (`IPC_CALL` reply `arg0` on success)               |
| `RCX`    | arg3                                                                       |
| `R8`     | arg4                                                                       |
| `R9`     | arg5                                                                       |

All 32-bit-field syscall arguments are validated as 32-bit-clean (high 32 bits
must be zero). `syscall_arg_u32(raw, out)` rejects any value with high bits
set. The frame handler copies the ring3 register state into the current
thread's `ctx` at entry so blocking syscalls resume at the correct post-syscall
RIP when the thread is rescheduled.

**Syscall table:**

| ID | Name            | Args                                                      | Returns                              |
|----|-----------------|-----------------------------------------------------------|--------------------------------------|
| 0  | `NOP`           | —                                                         | 0                                    |
| 1  | `GETPID`        | —                                                         | `pid`                                |
| 2  | `EXIT`          | `RDI=exit_status (i32)`                                   | does not return                      |
| 3  | `YIELD`         | —                                                         | 0                                    |
| 4  | `WAIT`          | `RDI=child_pid`                                           | child exit status or -1              |
| 5  | `IPC_NOTIFY`    | `RDI=endpoint`                                            | `ipc_result_t`                       |
| 6  | `IPC_CALL`      | `RDI=dst, RSI=type, RDX=arg0, RCX=arg1, R8=arg2, R9=arg3` | `RAX=ipc_result_t`, `RDX=reply.arg0` |
| 7  | `GETTID`        | —                                                         | current `tid`                        |
| 8  | `THREAD_YIELD`  | —                                                         | 0                                    |
| 9  | `THREAD_EXIT`   | `RDI=exit_status (i32)`                                   | does not return                      |
| 10 | `THREAD_CREATE` | `RDI=entry_rip, RSI=user_stack_top`                       | `tid` or -1                          |
| 11 | `THREAD_JOIN`   | `RDI=target_tid`                                          | target exit status or -1             |
| 12 | `THREAD_DETACH` | `RDI=target_tid`                                          | 0 or -1                              |
| 13 | `NOTIFY_READY`  | —                                                         | 0                                    |

`WAIT` and `THREAD_JOIN` loop internally: on `IPC_EMPTY` / pending result they
call `process_yield(PROCESS_RUN_BLOCKED)` until the target exits. The calling
thread is woken when `process_wake_waiters` or `process_wake_thread_joiner`
fires.

`IPC_CALL` is a synchronous request/reply primitive. It:
1. Allocates or reuses a per-process source endpoint (lazy, validated on reuse).
2. Assigns a monotonically-increasing `request_id`.
3. Sends the request to `destination` via `ipc_send_from`.
4. Loops on `ipc_recv_for(source_endpoint)`, yielding on `IPC_EMPTY`.
5. Checks `response.request_id == request_id` (out-of-order messages are
   retained in a per-process pending queue of depth `SYSCALL_IPC_PENDING_DEPTH = 8`
   for later matching).
6. Validates reply authenticity: `response.source` must match the destination
   endpoint's owner context (`syscall_ipc_reply_authentic`).
7. Returns `response.arg0` in `RDX` on success; `RDX = 0` on any error path.

`IPC_NOTIFY` uses `ipc_notify_from` on a notification-type endpoint. It cannot
send to a kernel-owned endpoint unless that endpoint is explicitly allowlisted
(`g_ipc_notify_control_deny_endpoint`).

`IPC_CALL` cannot reach kernel-owned endpoints unless the destination is the
echo test endpoint (`g_ipc_call_echo_endpoint`). All other kernel-context
endpoints return `IPC_ERR_PERM`.

---

### IPC Transport Layer

The kernel IPC layer (`src/kernel/ipc.c`) provides two endpoint types and five
operations. All transport state is kernel-owned; user processes access it only
through hostcalls (WASM) or the syscall gate (ring3).

#### Endpoint Types

| `ipc_endpoint_type_t`            | Purpose                                                 |
|----------------------------------|---------------------------------------------------------|
| `IPC_ENDPOINT_TYPE_MESSAGE`      | Bounded FIFO message queue; used for all service IPC    |
| `IPC_ENDPOINT_TYPE_NOTIFICATION` | Counter-based notification; used for lightweight signal |

**Message endpoint** (`ipc_endpoint_create`): FIFO queue of `IPC_QUEUE_DEPTH = 32`
messages. The message at position `head` is the oldest.

**Notification endpoint** (`ipc_notification_create`): maintains a `notify_count`
saturating counter (capped at `UINT32_MAX`). `ipc_notify_from` increments it;
`ipc_wait_for` decrements it or returns `IPC_EMPTY` if zero.

#### Endpoint Table

The table is a linked list (`list_t`) growing in `IPC_ENDPOINT_TABLE_CHUNK = 16`
chunks. Endpoint IDs are assigned sequentially from `g_next_endpoint_id = 1`;
`IPC_ENDPOINT_NONE = 0xFFFFFFFF` is reserved as the null sentinel. IDs wrap
around (skipping `IPC_ENDPOINT_NONE`) when they reach `0xFFFFFFFF`.

`ipc_endpoints_release_owner(context_id)` walks the full table and frees all
endpoints owned by the given context. This is called during process reap to
prevent table exhaustion across repeated short-lived app runs.

#### `ipc_endpoint_t` Structure

```c
typedef struct {
    uint32_t           id;
    uint32_t           in_use;
    ipc_endpoint_type_t type;
    uint32_t           owner_context_id;
    spinlock_t         lock;
    ipc_message_t      queue[IPC_QUEUE_DEPTH];
    uint32_t           head, tail, count;
    uint32_t           notify_count;
    uint32_t           waiter_tid;
} ipc_endpoint_t;
```

`waiter_tid` stores the TID of a thread currently blocked on this endpoint
(set in `ipc_recv_for` / `ipc_wait_for` when the queue/count is empty). A
subsequent send/notify clears `waiter_tid` and calls `process_wake_thread(tid)`.
Only one waiter per endpoint is tracked; multiple concurrent receivers on the
same endpoint are an anti-pattern.

#### Locking Protocol

Lock order: `g_endpoint_table_lock` → `ep->lock`.

`ipc_send_from` performs the source permission check (ownership of
`message->source`) under `g_endpoint_table_lock` only, without acquiring
`ep->lock`, to avoid holding two endpoint locks simultaneously. After the
permission check it acquires `ep->lock` for queue mutation and wake.

#### `ipc_message_t`

```c
typedef struct {
    uint32_t type;        // opcode — see wasmos_driver_abi.h range table
    uint32_t source;      // sender's reply endpoint (or IPC_ENDPOINT_NONE)
    uint32_t destination; // target endpoint (overwritten to ep->id on enqueue)
    uint32_t request_id;  // correlation token for request/reply matching
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
} ipc_message_t;
```

All fields are 32-bit. Bulk data never flows through messages; it moves through
the buffer-borrow mechanism with IPC messages carrying only control metadata.

#### IPC Result Codes

| Code              | Value | Meaning                                                |
|-------------------|-------|--------------------------------------------------------|
| `IPC_OK`          | 0     | Success                                                |
| `IPC_EMPTY`       | 1     | Queue empty / notification count zero                  |
| `IPC_ERR_INVALID` | -1    | Null pointer, invalid endpoint, type mismatch          |
| `IPC_ERR_PERM`    | -2    | Caller does not own the source or destination endpoint |
| `IPC_ERR_FULL`    | -3    | Queue at capacity (`IPC_QUEUE_DEPTH = 32`)             |

#### Permission Model

- `ipc_send_from(sender_ctx, endpoint, msg)`: if `sender_ctx != IPC_CONTEXT_KERNEL`,
  `msg->source` must be owned by `sender_ctx`. The destination endpoint can
  belong to any context.
- `ipc_recv_for(receiver_ctx, endpoint, out)`: if `receiver_ctx != IPC_CONTEXT_KERNEL`,
  `ep->owner_context_id` must equal `receiver_ctx`.
- Kernel calls `ipc_send` / `ipc_recv` (which pass `IPC_CONTEXT_KERNEL = 0`)
  and bypass context checks entirely.
- `ipc_notify_from`: sender must own the notification endpoint
  (or be the kernel).
- `ipc_wait_for`: receiver must own the notification endpoint (or be the kernel).

---

### IPC Opcode Space

Opcodes are allocated in contiguous ranges. Each range belongs to one
subsystem; opcodes are never scattered across ranges.

| Range         | Subsystem                                                       |
|---------------|-----------------------------------------------------------------|
| `0x100–0x1FF` | chardev                                                         |
| `0x200–0x2FF` | proc / process manager (spawn, wait, kill, service, DMA borrow) |
| `0x300–0x3FF` | block device                                                    |
| `0x400–0x4FF` | filesystem                                                      |
| `0x600–0x6FF` | fbtext (framebuffer text)                                       |
| `0x700–0x7FF` | VT (virtual terminal)                                           |
| `0x800–0x8FF` | input / RTC / virtio-serial                                     |
| `0x900–0x9FF` | device-manager                                                  |
| `0xA00–0xBFF` | networking (virtio-net driver + net-stack service)              |

All opcodes are defined in `src/drivers/include/wasmos_driver_abi.h`.

---

### Process Manager (`proc` Endpoint)

The process manager (PM) runs as a kernel-native C++ service. It owns the
`proc` endpoint and is the exclusive entry point for process lifecycle
operations from user space.

#### Opcode Table

All `proc` endpoint opcodes are in the `0x200–0x2FF` range.

| Opcode                          | Value   | Direction    | Description                           |
|---------------------------------|---------|--------------|---------------------------------------|
| `PROC_IPC_SPAWN`                | `0x200` | request      | Spawn by module index                 |
| `PROC_IPC_WAIT`                 | `0x201` | request      | Wait for child exit                   |
| `PROC_IPC_KILL`                 | `0x202` | request      | Kill child                            |
| `PROC_IPC_STATUS`               | `0x203` | request      | Query child state                     |
| `PROC_IPC_SPAWN_NAME`           | `0x204` | request      | Spawn by name (initfs)                |
| `PROC_IPC_SPAWN_CAPS`           | `0x205` | request      | Spawn with capability profile         |
| `PROC_IPC_MODULE_META`          | `0x206` | request      | Query module metadata by index        |
| `PROC_IPC_MODULE_META_PATH`     | `0x207` | request      | Query module metadata by path         |
| `PROC_IPC_SPAWN_CAPS_V2`        | `0x208` | request      | Spawn with extended caps profile      |
| `PROC_IPC_SPAWN_PATH`           | `0x209` | request      | Spawn by filesystem path              |
| `PROC_IPC_SPAWN_PATH_CAPS`      | `0x20A` | request      | Spawn by path with caps               |
| `PROC_IPC_SPAWN_SYNC`           | `0x20B` | request      | Spawn; block until child NOTIFY_READY |
| `PROC_IPC_NOTIFY_READY`         | `0x20C` | notification | Child signals it is ready             |
| `PROC_IPC_SPAWN_CAPS_SYNC`      | `0x20D` | request      | SPAWN_CAPS with sync wait             |
| `PROC_IPC_SPAWN_PATH_SYNC`      | `0x20E` | request      | SPAWN_PATH with sync wait             |
| `PROC_IPC_SPAWN_PATH_CAPS_SYNC` | `0x20F` | request      | SPAWN_PATH_CAPS with sync wait        |
| `PROC_IPC_DMA_MAP_BORROW_REQ`   | `0x230` | request      | Map a borrow handle for DMA           |
| `PROC_IPC_DMA_SYNC_BORROW_REQ`  | `0x231` | request      | Sync a mapped borrow handle           |
| `PROC_IPC_DMA_UNMAP_BORROW_REQ` | `0x232` | request      | Unmap a borrow handle                 |
| `SVC_IPC_REGISTER_REQ`          | `0x220` | request      | Register a named service endpoint     |
| `SVC_IPC_LOOKUP_REQ`            | `0x221` | request      | Look up a named service endpoint      |
| `PROC_IPC_RESP`                 | `0x280` | response     | Success response                      |
| `SVC_IPC_REGISTER_RESP`         | `0x2A0` | response     | Register success                      |
| `SVC_IPC_LOOKUP_RESP`           | `0x2A1` | response     | Lookup success (arg0=endpoint)        |
| `PROC_IPC_DMA_BORROW_RESP`      | `0x2B0` | response     | DMA borrow success                    |
| `SVC_IPC_ERROR`                 | `0x2AF` | response     | Service registry error                |
| `PROC_IPC_DMA_BORROW_ERROR`     | `0x2BF` | response     | DMA borrow error                      |
| `PROC_IPC_ERROR`                | `0x2FF` | response     | General PM error                      |

#### Spawn Variants

**Async spawn** (`PROC_IPC_SPAWN`, `PROC_IPC_SPAWN_PATH`, etc.): the PM loads,
validates, and spawns the app, then immediately responds with `PROC_IPC_RESP,
arg0=child_pid`. For service/driver app kinds, `PROC_IPC_SPAWN_PATH` internally
waits for the child to call `PROC_IPC_NOTIFY_READY` (matching `SPAWN_PATH_SYNC`
behavior).

**Sync spawn** (`PROC_IPC_SPAWN_SYNC`, `PROC_IPC_SPAWN_PATH_SYNC`, etc.):
the PM defers `PROC_IPC_RESP` until the child process calls
`PROC_IPC_NOTIFY_READY` or the timeout expires. On timeout or child death
before ready: `PROC_IPC_ERROR, arg1=error_code`.

`PROC_IPC_NOTIFY_READY` (opcode `0x20C`) is sent by the child to the `proc`
endpoint when its initialization is complete. This also unblocks callers of the
`WASMOS_SYSCALL_NOTIFY_READY` syscall pattern.

#### Entry Bindings

The PM resolves spawn argument bindings from WASMOS-APP metadata:

| Binding name        | Resolved value                   |
|---------------------|----------------------------------|
| `none`              | 0                                |
| `proc.endpoint`     | PM's `proc` endpoint ID          |
| `module.count`      | number of initfs modules         |
| `init.module.index` | initfs module index of `fs-init` |
| `block.endpoint`    | ATA block endpoint               |
| `cli.tty.alloc`     | next available VT tty slot       |
| `chardev.endpoint`  | chardev endpoint                 |
| `const.neg1`        | `0xFFFFFFFF`                     |

These are resolved at spawn time and passed as `arg0..arg3` to the spawned
process entry function (`wasmos_main`).

#### Service Registry

The PM maintains a flat list of named services (`pm_service_entry_t`). Names
are capped at 16 characters.

`SVC_IPC_REGISTER_REQ`: caller sends name (packed into arg0–arg3 as 16 bytes)
and their reply endpoint. PM stores the name → endpoint mapping with the
caller's `owner_context_id`. Responds `SVC_IPC_REGISTER_RESP`.

`SVC_IPC_LOOKUP_REQ`: caller sends name. PM responds `SVC_IPC_LOOKUP_RESP,
arg0=endpoint` or `SVC_IPC_ERROR` if not found.

The PM updates well-known internal endpoint references (fs, block, fb, vt)
when the corresponding services register. This allows late-binding of service
endpoints without hardcoded values.

#### Wait and Kill

`PROC_IPC_WAIT`: the PM records a `pm_wait_state_t` with the caller's reply
endpoint and `request_id`. When `process_manager_on_child_ready` fires (child
exits and wakes waiters), the PM sends `PROC_IPC_RESP` to the saved reply
endpoint with the child exit status.

`PROC_IPC_KILL`: parent-restricted. The PM validates caller ownership before
calling `process_kill`.

`PROC_IPC_STATUS`: returns process state, block reason, and `is_wasm` flag.
Owner-restricted.

---

### Buffer-Borrow Mechanism

Bulk data between processes uses the buffer-borrow model rather than IPC
message fields. A borrow grants one process read/write access to a
kernel-managed shared buffer owned by another process.

**Buffer kinds** (defined in `process_manager.h`):

| Kind                         | Value | Used for                                |
|------------------------------|-------|-----------------------------------------|
| `PM_BUFFER_KIND_FILESYSTEM`  | 1     | FS read buffers (file content delivery) |
| `PM_BUFFER_KIND_FRAMEBUFFER` | 2     | Framebuffer pixel data                  |

**Borrow flags:**

| Flag                     | Value |
|--------------------------|-------|
| `PM_BUFFER_BORROW_READ`  | `0x1` |
| `PM_BUFFER_BORROW_WRITE` | `0x2` |

A typical FS read flow:
1. PM allocates a 2 MB FS buffer (`PM_FS_BUFFER_SIZE`) per spawn state.
2. PM borrows the buffer to the FS driver's context (`process_manager_buffer_borrow_context`).
3. FS driver reads file content into the borrowed buffer.
4. PM reads back the content and forwards it to the requesting process.
5. PM releases the borrow (`process_manager_buffer_release_context`).

#### DMA Buffer Borrow

For device drivers that need DMA access to a buffer, the PM provides a
three-phase DMA lifecycle through the `proc` endpoint:

| Opcode | Operation |
|---|---|
| `PROC_IPC_DMA_MAP_BORROW_REQ` | Map borrow for DMA; returns device address |
| `PROC_IPC_DMA_SYNC_BORROW_REQ` | Sync CPU/device coherency for a mapped borrow |
| `PROC_IPC_DMA_UNMAP_BORROW_REQ` | Unmap and release DMA mapping |

The driver sends these as IPC requests and receives `PROC_IPC_DMA_BORROW_RESP`
(device address in arg0) or `PROC_IPC_DMA_BORROW_ERROR` on failure. This is
the driver-facing API; the backing implementation calls
`process_manager_buffer_dma_map` / `_dma_sync` / `_dma_unmap` in the PM.

---

### `libsys` Event Loop

`libsys` provides single-endpoint event loops for WASM and native services.
The pattern prevents response stealing and avoids duplicated receive loops.

#### `wasmos_sys_event_loop_t`

```c
typedef struct {
    int32_t receiver_endpoint;
    int32_t next_request_id;
    void (*default_on_message)(void *user, const wasmos_ipc_message_t *msg);
    void *default_user;
    wasmos_sys_intent_t  intents[WASMOS_SYS_INTENT_MAX];   // 16 slots
    wasmos_sys_handler_t handlers[WASMOS_SYS_HANDLER_MAX];  // 16 slots
} wasmos_sys_event_loop_t;
```

**Intent** (`wasmos_sys_intent_t`): a pending outgoing request tracked by
`request_id`. When a reply arrives with a matching `request_id`, the intent's
`on_resolve` callback fires and the slot is freed. Up to 16 concurrent intents.

**Handler** (`wasmos_sys_handler_t`): a registered callback for a specific
incoming `msg_type`. Unsolicited messages that match a handler's type are
dispatched to it. Up to 16 type-specific handlers.

**Default handler**: catches all messages that don't match any intent or type
handler.

#### Dispatch Priority

`wasmos_sys_event_loop_poll(loop, budget)` processes up to `budget` messages
per call:

1. Try `wasmos_ipc_try_recv` on `receiver_endpoint`. Stop if empty.
2. Check intents by `request_id` first (reply correlation).
3. If no intent matches, check handlers by `msg_type`.
4. If no handler matches, call the default handler.

This gives replies priority over unsolicited traffic, preventing a flood of
notifications from starving pending request replies.

#### `wasmos_sys_intent_send`

Sends a message and registers a completion callback atomically:

```c
wasmos_sys_intent_send(
    loop, destination, source, type, arg0, arg1, arg2, arg3,
    on_resolve, user, &out_request_id);
```

The loop auto-increments `next_request_id` and stores the intent. If all 16
intent slots are occupied, the send is rejected.

`wasmos_sys_intent_send_with_request_id` allows a caller to specify a fixed
`request_id` (used for re-use of a pre-allocated ID, e.g. when restarting a
request after a timeout).

#### Usage Pattern

Services should use exactly one event loop per endpoint:

```c
wasmos_sys_event_loop_t loop;
wasmos_sys_event_loop_init(&loop, my_endpoint, 1);
wasmos_sys_event_register(&loop, FS_IPC_READ_REQ, handle_read, NULL);
wasmos_sys_event_set_default(&loop, handle_unknown, NULL);

for (;;) {
    while (wasmos_sys_event_loop_poll(&loop, 16) > 0) {}
    wasmos_ipc_yield();
}
```

Multiple ad-hoc `recv-until-matching` loops on the same endpoint are an
anti-pattern: they cause response stealing where one loop consumes replies
intended for another.

---

### Invariants

1. **One endpoint, one receiver.** A single receive pump owns each service
   endpoint. Concurrent receivers on the same endpoint are not supported.

2. **Bulk data through borrows, not messages.** File content, pixel buffers,
   and packet data flow through kernel-managed shared memory. IPC messages carry
   only metadata and completion notifications.

3. **Source endpoint ownership is verified at send.** Non-kernel senders must
   own `message->source`. This prevents a process from spoofing another
   process's endpoint as its reply address.

4. **Reply authenticity is checked at `IPC_CALL`.** The syscall layer verifies
   that the reply source endpoint is owned by the expected context, preventing
   reply injection from a third party.

5. **Capability grants are declared at spawn.** No hardware capability can be
   acquired through IPC after process spawn. `device-manager` grants capabilities
   at spawn time; the PM validates them against the WASMOS-APP metadata.

6. **Endpoints are released on process reap.** `ipc_endpoints_release_owner`
   is called during process reap, preventing stale endpoint IDs from accumulating
   in the table.
