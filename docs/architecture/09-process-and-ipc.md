## Process Model and IPC

This document covers the WASMOS process model, the IPC transport layer, the
select-set and poll-hub mechanisms, the futex primitive, the `int 0x80` syscall
ABI, the process manager (`proc` endpoint), and the `libsys` event-loop helpers.
The authoritative sources are `src/kernel/ipc.c`, `src/kernel/syscall.c`,
`src/kernel/process.c`, `src/kernel/sched_event.c`, `src/kernel/poll.c`,
`src/kernel/futex.c`, `src/kernel/process_manager*.c`, and
`src/drivers/include/wasmos_driver_abi.h`.

---

### Process Lifecycle

Processes move through four states.  Threads within a process have a parallel
state machine (see `07-scheduling-and-preemption.md`).

```
UNUSED ──spawn──► ALIVE
                    │
                    ├──► ZOMBIE ──reap──► UNUSED
                    │
                    └──► REAPING (transitional; slots still valid)
```

| `process_state_t`        | Meaning                                         |
|--------------------------|-------------------------------------------------|
| `PROCESS_STATE_UNUSED`   | Slot free                                       |
| `PROCESS_STATE_ALIVE`    | One or more live threads                        |
| `PROCESS_STATE_REAPING`  | Reap in progress; table entry still valid       |
| `PROCESS_STATE_ZOMBIE`   | All threads exited; awaiting reap               |

A process becomes zombie when its last thread exits, when `process_kill` is
called, or when a `WASMOS_SYSCALL_EXIT` syscall fires.  Zombie processes are
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

`pid` and `context_id` are distinct: capability grants, endpoint ownership, and
MM regions are keyed by `context_id`, not `pid`.  Multiple threads within one
process share the same `context_id` and address space.

---

### Process Ownership and Trust

The kernel-owned `init` task (spawned by `kmain`) is the root parent for the
first generation of kernel-started processes.  It spawns `device-manager` and
waits for FAT readiness before loading `sysinit`.

The process manager (`proc` endpoint) is the privileged mediator for all
further process lifecycle operations.  It:

- Validates capability profiles at spawn time from WASMOS-APP metadata
- Assigns `io.port`, `irq.route`, `mmio.map`, `dma.buffer`, `system.control`
  grants per declared profile
- Maintains the service registry (name → endpoint)
- Enforces owner-context restrictions on wait, kill, and status

---

### `int 0x80` Syscall ABI

Ring3 user processes communicate with the kernel through `int 0x80` (DPL3 gate).
The dispatcher is `x86_syscall_handler(syscall_frame_t *frame)`.

**Register convention:**

| Register | Role                                                                        |
|----------|-----------------------------------------------------------------------------|
| `RAX`    | Syscall ID (see `wasmos_syscall_id_t`); primary return value on completion  |
| `RDI`    | arg0                                                                        |
| `RSI`    | arg1                                                                        |
| `RDX`    | arg2 / secondary return (`IPC_CALL` reply `arg0` on success)                |
| `RCX`    | arg3                                                                        |
| `R8`     | arg4                                                                        |
| `R9`     | arg5                                                                        |

All 32-bit-field syscall arguments are validated as 32-bit-clean.

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

`WAIT` and `THREAD_JOIN` use `sched_event_wait` internally for blocking;
they are woken by `sched_event_wake_all` when the target exits or is killed.

`IPC_CALL` is a synchronous request/reply primitive.  It allocates a per-process
source endpoint, sends to the destination, then calls
`ipc_recv_blocking_for(source_endpoint)` with request_id matching.

---

### IPC Transport Layer

The kernel IPC layer (`src/kernel/ipc.c`) provides two endpoint types and five
operations.

#### Endpoint Types

| `ipc_endpoint_type_t`            | Purpose                                                 |
|----------------------------------|---------------------------------------------------------|
| `IPC_ENDPOINT_TYPE_MESSAGE`      | Bounded FIFO message queue; used for all service IPC    |
| `IPC_ENDPOINT_TYPE_NOTIFICATION` | Counter-based notification; used for lightweight signal |

**Message endpoint**: FIFO queue of `IPC_QUEUE_DEPTH = 32` messages.
**Notification endpoint**: `notify_count` saturating counter.

#### `ipc_endpoint_t` Structure

```c
typedef struct {
    uint32_t            id;
    uint32_t            in_use;
    ipc_endpoint_type_t type;
    uint32_t            owner_context_id;
    spinlock_t          lock;
    ipc_message_t       queue[IPC_QUEUE_DEPTH];
    uint32_t            head, tail, count;
    uint32_t            notify_count;
    sched_event_t       event;        /* wait_list of blocked receivers */
    poll_struct_t      *poll_struct;  /* push-model poll hub (lazy-allocated) */
} ipc_endpoint_t;
```

`waiter_tid` is removed.  The `event` field holds an embedded `sched_event_t`
whose `wait_list` can hold multiple blocked receiver threads.  `poll_struct`
is allocated lazily when the first select set targets this endpoint.

#### IPC Receive Variants

**`ipc_recv_for(ctx, ep, out)`** — non-blocking.  Returns `IPC_OK` if a message
is available, otherwise registers the calling thread in `ep->event.wait_list`
and returns `IPC_EMPTY`.  The YIELDED handler cleans up stale registrations.

**`ipc_recv_blocking_for(ctx, ep, out)`** — true blocking.  On `IPC_EMPTY`
calls `sched_event_wait(&ep->event, 0)` to park the thread.  Returns when a
sender wakes the thread.  Used by all WASM blocking receive host functions.

#### Send and Wake

`ipc_send_from` enqueues the message, calls `sched_event_wake_one(&ep->event, ...)`,
then calls `poll_notify(ep->poll_struct, POLL_EV_IN, ep->id)` to push a
readiness notification to any registered select sets.

#### IPC Result Codes

| Code              | Value | Meaning                                                |
|-------------------|-------|--------------------------------------------------------|
| `IPC_OK`          | 0     | Success                                                |
| `IPC_EMPTY`       | 1     | Queue empty / notification count zero                  |
| `IPC_ERR_INVALID` | -1    | Null pointer, invalid endpoint, type mismatch          |
| `IPC_ERR_PERM`    | -2    | Caller does not own the source or destination endpoint |
| `IPC_ERR_FULL`    | -3    | Queue at capacity (`IPC_QUEUE_DEPTH = 32`)             |

#### `ipc_message_t`

```c
typedef struct {
    uint32_t type;        // opcode
    uint32_t source;      // sender's reply endpoint
    uint32_t destination; // target endpoint
    uint32_t request_id;  // correlation token
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
} ipc_message_t;
```

---

### Poll-Hub: Push-Model Select

Source: `src/kernel/include/poll.h`, `src/kernel/poll.c`

The poll hub attaches a `poll_struct_t` to each IPC endpoint and pushes
readiness notifications directly to registered select sets — eliminating the
legacy O(N) scan-on-send.

```c
typedef struct poll_watcher {
    struct ipc_select  *sel;
    uint32_t            user_data;
    struct poll_watcher *next;
} poll_watcher_t;

typedef struct {
    poll_watcher_t *watchers[POLL_EV_MAX];  /* per event type: EV_IN, EV_OUT, EV_CLOSE, EV_KERNEL */
} poll_struct_t;
```

`poll_notify(ps, ev, ep_id)` walks `ps->watchers[ev]` and calls
`ipc_select_signal(sel, ep_id)` on each registered select set — O(watchers per
endpoint), typically O(1).

`poll_struct_t` is allocated lazily when the first `ipc_select_add` targets an
endpoint and stored in `ep->poll_struct`.

---

### Select-Set API

Source: `src/kernel/ipc.c`, `src/kernel/include/ipc.h`

Select sets allow a thread to block on any of up to `IPC_SELECT_EPS_MAX = 8`
endpoints simultaneously.

```c
typedef struct {
    uint32_t      id;
    uint32_t      in_use;
    spinlock_t    lock;
    sched_event_t event;         /* blocked waiter */
    uint32_t      ready_ep;      /* endpoint that signalled first */
    uint32_t      ep_ids[IPC_SELECT_EPS_MAX];
    uint32_t      ep_count;
} ipc_select_t;
```

**Kernel API:**

| Function                              | Description                                              |
|---------------------------------------|----------------------------------------------------------|
| `ipc_select_create()`                 | Allocate a select set; returns select_id                 |
| `ipc_select_add(sel_id, ep_id)`       | Register endpoint; lazily creates `ep->poll_struct`      |
| `ipc_select_wait(sel_id, out_ep_id)`  | Block until any registered endpoint has a message        |
| `ipc_select_destroy(sel_id)`          | Unregister from all endpoint `poll_struct` lists; free   |
| `ipc_select_signal(sel, ep_id)`       | Called by `poll_notify`; wakes waiter, sets `ready_ep`   |

`ipc_select_wait` calls `sched_event_wait(&sel->event, 0)`.  When a sender
fires `poll_notify`, `ipc_select_signal` calls `sched_event_wake_one(&sel->event)`.

**WASM host functions (`wasmos/api.h`):**

```c
extern int32_t wasmos_ipc_select_create(void)
    WASMOS_WASM_IMPORT("wasmos", "ipc_select_create");
extern int32_t wasmos_ipc_select_add(int32_t select_id, int32_t endpoint_id)
    WASMOS_WASM_IMPORT("wasmos", "ipc_select_add");
extern int32_t wasmos_ipc_select_wait(int32_t select_id)
    WASMOS_WASM_IMPORT("wasmos", "ipc_select_wait");
extern int32_t wasmos_ipc_select_destroy(int32_t select_id)
    WASMOS_WASM_IMPORT("wasmos", "ipc_select_destroy");
```

**Single-endpoint blocking receive:**

```c
extern int32_t wasmos_ipc_select_one(int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "ipc_select_one");   /* replaces ipc_recv */
```

**Non-blocking drain (returns 0 on empty):**

```c
extern int32_t wasmos_ipc_drain(int32_t endpoint)
    WASMOS_WASM_IMPORT("wasmos", "ipc_drain");        /* replaces ipc_try_recv */
```

`wasmos_ipc_select_one` blocks the calling WASM thread via
`ipc_recv_blocking_for`.  `wasmos_ipc_drain` calls `ipc_recv_for` and returns
0 on `IPC_EMPTY` — never blocking.

---

### Futex Primitive

Source: `src/kernel/futex.c`, `src/kernel/include/futex.h`

A futex provides a kernel parking lot for WASM userspace synchronization
primitives.  The kernel-side hash table has 16 buckets keyed by physical
address:

```c
#define FUTEX_TABLE_SIZE 16
static struct { spinlock_t lock; list_head_t head; } g_futex_table[FUTEX_TABLE_SIZE];
```

**`futex_wait(uaddr, expected, timeout_ms, context_id)`**:
1. Translate `uaddr` → physical address via `mm_uva_to_paddr`.
2. Find or allocate a `futex_t` in the hash bucket.
3. If `*kaddr != expected`: return immediately (caller retries).
4. `sched_event_wait(&ft->event, timeout_ms)` — parks the thread.
5. On wakeup: return `0` (ok) or `-ETIMEDOUT`.

**`futex_wake(uaddr, count, context_id)`**:
- Calls `sched_event_wake_one(&ft->event, ...)` up to `count` times.

**WASM host functions:**

```c
extern int32_t wasmos_futex_wait(int32_t addr, int32_t expected, int32_t timeout_ms)
    WASMOS_WASM_IMPORT("wasmos", "futex_wait");
extern int32_t wasmos_futex_wake(int32_t addr, int32_t count)
    WASMOS_WASM_IMPORT("wasmos", "futex_wake");
```

`addr` is a WASM linear-memory offset; the host function translates via
`mm_uva_to_paddr(proc->context_id, wasm_linear_base + addr)`.

These two primitives allow WASM to implement any synchronization object
(mutex, semaphore, condvar) without kernel-side abstractions for each.

---

### IPC Opcode Space

Opcodes are allocated in contiguous ranges.

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

The process manager (PM) runs as a kernel-native C++ service.

#### Opcode Table

| Opcode                          | Value   | Direction    | Description                           |
|---------------------------------|---------|--------------|---------------------------------------|
| `PROC_IPC_SPAWN`                | `0x200` | request      | Spawn by module index                 |
| `PROC_IPC_WAIT`                 | `0x201` | request      | Wait for child exit                   |
| `PROC_IPC_KILL`                 | `0x202` | request      | Kill child                            |
| `PROC_IPC_STATUS`               | `0x203` | request      | Query child state                     |
| `PROC_IPC_SPAWN_PATH`           | `0x209` | request      | Spawn by filesystem path              |
| `PROC_IPC_SPAWN_PATH_SYNC`      | `0x20E` | request      | SPAWN_PATH; wait for NOTIFY_READY     |
| `PROC_IPC_NOTIFY_READY`         | `0x20C` | notification | Child signals it is ready             |
| `PROC_IPC_DMA_MAP_BORROW_REQ`   | `0x230` | request      | Map a borrow handle for DMA           |
| `PROC_IPC_DMA_SYNC_BORROW_REQ`  | `0x231` | request      | Sync a mapped borrow handle           |
| `PROC_IPC_DMA_UNMAP_BORROW_REQ` | `0x232` | request      | Unmap a borrow handle                 |
| `SVC_IPC_REGISTER_REQ`          | `0x220` | request      | Register a named service endpoint     |
| `SVC_IPC_LOOKUP_REQ`            | `0x221` | request      | Look up a named service endpoint      |
| `PROC_IPC_RESP`                 | `0x280` | response     | Success response                      |
| `PROC_IPC_ERROR`                | `0x2FF` | response     | General PM error                      |

#### Spawn Variants

**Async spawn** (`PROC_IPC_SPAWN_PATH` for regular apps): the PM loads and
spawns the app, then immediately responds with `PROC_IPC_RESP, arg0=child_pid,
arg1=app_flags`.  The CLI sends `PROC_IPC_WAIT` for regular apps; for apps with
`FLAG_SERVICE` or `FLAG_DRIVER`, the PM internally defers the response until
`PROC_IPC_NOTIFY_READY` is received from the child (`pm_poll_sync_spawn` polls
`child->ready`), then responds with the service flag set so the CLI shows the
prompt without waiting for process exit.

`wasmos_proc_notify_ready()` is a direct kernel hostcall that sets
`proc->ready = 1` without sending an IPC.  `wasmos_sys_notify_ready()` (libsys)
sends an IPC to PM's `proc` endpoint.

#### Service Registry

The PM maintains a flat list of named services.  Names are capped at 16
characters.  `SVC_IPC_REGISTER_REQ` / `SVC_IPC_LOOKUP_REQ` (opcodes
`0x220`/`0x221`) register and resolve service endpoint IDs.

#### Entry Bindings

The PM resolves spawn argument bindings from WASMOS-APP metadata:

| Binding name        | Resolved value                   |
|---------------------|----------------------------------|
| `none`              | 0                                |
| `proc.endpoint`     | PM's `proc` endpoint ID          |
| `chardev.endpoint`  | chardev endpoint                 |
| `block.endpoint`    | ATA block endpoint               |
| `cli.tty.alloc`     | next available VT tty slot       |

---

### Buffer-Borrow Mechanism

Bulk data between processes uses the buffer-borrow model.

**Buffer kinds:**

| Kind                         | Value | Used for                                |
|------------------------------|-------|-----------------------------------------|
| `PM_BUFFER_KIND_FILESYSTEM`  | 1     | FS read buffers (file content delivery) |
| `PM_BUFFER_KIND_FRAMEBUFFER` | 2     | Framebuffer pixel data                  |

**DMA Buffer Borrow** — three-phase lifecycle:

| Opcode                          | Operation                                  |
|---------------------------------|--------------------------------------------|
| `PROC_IPC_DMA_MAP_BORROW_REQ`   | Map borrow for DMA; returns device address |
| `PROC_IPC_DMA_SYNC_BORROW_REQ`  | Sync CPU/device coherency                  |
| `PROC_IPC_DMA_UNMAP_BORROW_REQ` | Unmap and release DMA mapping              |

---

### `libsys` Event Loop

`libsys` provides select-based event loops for WASM and native services.

#### `wasmos_sys_event_loop_t`

```c
typedef struct {
    int32_t receiver_endpoint;
    int32_t select_id;           /* select set for blocking wait */
    int32_t next_request_id;
    void (*default_on_message)(void *user, const wasmos_ipc_message_t *msg);
    void *default_user;
    wasmos_sys_intent_t  intents[WASMOS_SYS_INTENT_MAX];   // 16 slots
    wasmos_sys_handler_t handlers[WASMOS_SYS_HANDLER_MAX];  // 16 slots
} wasmos_sys_event_loop_t;
```

`wasmos_sys_event_loop_init` creates a select set via `wasmos_ipc_select_create`
and calls `wasmos_ipc_select_add(select_id, receiver_endpoint)`.

**Intent** (`wasmos_sys_intent_t`): a pending outgoing request tracked by
`request_id`.  When a reply arrives with a matching `request_id`, the intent's
`on_resolve` callback fires.

**Handler** (`wasmos_sys_handler_t`): a registered callback for a specific
incoming `msg_type`.

#### Dispatch

`wasmos_sys_event_loop_poll(loop, budget)` processes up to `budget` messages:

1. `wasmos_ipc_drain(receiver_endpoint)` — non-blocking drain attempt.
2. If empty and `budget > 0`: `wasmos_ipc_select_wait(select_id)` — blocks
   until the endpoint has a message (no busy-polling).
3. Check intents by `request_id` first (reply correlation).
4. If no intent matches, check handlers by `msg_type`.
5. If no handler matches, call the default handler.

This gives replies priority over unsolicited traffic.  `wasmos_ipc_select_wait`
replaces the legacy `wasmos_ipc_yield()` spin loop.

#### Usage Pattern

Services use exactly one event loop per endpoint:

```c
wasmos_sys_event_loop_t loop;
wasmos_sys_event_loop_init(&loop, my_endpoint, 1);
wasmos_sys_event_register(&loop, FS_IPC_READ_REQ, handle_read, NULL);
wasmos_sys_event_set_default(&loop, handle_unknown, NULL);

for (;;) {
    wasmos_sys_event_loop_poll(&loop, 16);
}
```

Multiple ad-hoc receive loops on the same endpoint are an anti-pattern; they
cause response stealing.

---

### Invariants

1. **One endpoint, one receiver.** A single receive pump owns each service
   endpoint.  The select-set mechanism (`sched_event_t.wait_list`) supports
   multiple concurrent waiters per endpoint, but the service model uses one.

2. **Bulk data through borrows, not messages.** File content, pixel buffers,
   and packet data flow through kernel-managed shared memory.  IPC messages
   carry only metadata.

3. **Source endpoint ownership verified at send.** Non-kernel senders must own
   `message->source`.

4. **Reply authenticity checked at `IPC_CALL`.** The syscall layer verifies
   the reply source endpoint is owned by the expected context.

5. **Capability grants are declared at spawn.** No hardware capability can be
   acquired through IPC after process spawn.

6. **Endpoints are released on process reap.** `ipc_endpoints_release_owner`
   is called during process reap.

7. **No busy-polling in services.** Services must use `wasmos_ipc_select_one`
   (blocking single-endpoint) or `wasmos_ipc_select_wait` (blocking multi-endpoint)
   rather than polling `wasmos_ipc_drain` in a spin loop.  The scheduler does
   not yield on spin loops; only blocking waits release the CPU.

8. **Poll-hub registration is exclusive.** Once `ep->poll_struct` is created
   and a watcher is registered, the watcher persists until `ipc_select_destroy`
   removes it.  Endpoints and select sets must be destroyed in a consistent
   order to avoid dangling watcher pointers.
