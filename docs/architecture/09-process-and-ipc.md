## Process Model and IPC

> **Documentation status: Mixed reference and proposal.** Process, endpoint,
> transfer-buffer, and service-registry sections describe implemented behavior.
> Future transport and asynchronous IPC sections are labelled as proposals.

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

### Vring Bulk Transport (planned)

The FIFO message queue above copies a fixed-size `ipc_message_t` and is the
right tool for small **control** messages. It is a poor fit for **bulk or
streaming** payloads (network packets, disk blocks, audio frames, compositor
damage) where copying bytes through a bounded 32-deep queue is the bottleneck.

For those, the planned transport is a **virtqueue-style ring (vring)**: a shared
ring of descriptors that reference buffers in a shared pool, so producer and
consumer pass *ownership of buffers* rather than copying data. This is a
**complement** to message IPC, not a replacement — control stays on message
endpoints; bulk data moves over a vring.

The same ring mechanics serve both device transport and permanent
service↔service channels, so the design factors into a transport-neutral core
plus two backends:

- **vring core** — descriptor table + avail/used index management, batching,
  and memory barriers. Pure logic over `(region, notify_fn)`, with no device or
  transport knowledge. Lives in a `libsys` library that both drivers and
  services link (keeps the kernel out of the data path — the point of
  zero-copy).
- **PCI/MMIO backend** — real virtio devices; ring memory is a pinned physical
  region (see [DMA Transfers → Driver-Owned DMA Regions](12-dma-transfers.md)),
  the doorbell is the device notify register, and completion arrives by IRQ.
- **shmem/IPC backend** — service↔service; the ring and buffer pool are a shmem
  region mapped into both peers, the doorbell is a `NOTIFICATION` endpoint (the
  existing block/wake + select path), and there is no device, IRQ, or physical
  address. Being CPU-coherent, this is strictly simpler than the device case.

Two properties differ from stock virtio, which assumes a *trusted* driver
talking to a device, and must be treated as first-class here:

- **Mutual distrust → consumer-side validation.** A buggy or malicious producer
  can write any offset/length into the ring, so the consumer must bounds-check
  every descriptor against the region size on each consume. The isolation trick:
  the kernel maps *exactly* the shared region and nothing else, so a bad
  descriptor can at worst fault within that region and only harms the two
  participants — it cannot reach either peer's private memory. For device
  vrings the same bounds are enforced by `capability_dma_range_allowed` plus the
  low-2GB clamp.
- **Teardown / revocation.** If a peer dies mid-stream the region must be
  reclaimed and the other side notified; this rides on the shmem grant/revoke
  lifecycle, which must be solid before bulk service channels are built on it.

The transport-neutral vring core is implemented as a header-only libsys library,
`src/libsys/wasm/include/wasmos/vring.h`: the legacy split-virtqueue layout
(descriptor table + avail/used rings), descriptor alloc/free, publish/kick, and
used-ring consumption with consumer-side bounds validation, all as pure logic
over a caller-provided region and a `notify` callback — no device/PCI/IPC
knowledge. It is covered by `tests/unit/test_vring.c` (host unit test). The PCI
backend is implemented by `virtio-net` for its RX/TX queues. The shmem/service
backend remains future work and requires proven grant/revoke teardown. See
[Networking](22-networking-virtio-net-and-stack.md).

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

##### Class-Based Discovery (planned)

Direct lookup by a concrete provider name (e.g. `virtio.net`) couples clients to
a specific driver and cannot express *"any interface of this kind"* or *"which
of several"*.  The registry is therefore generalized so a provider registers
under an optional **virtual class** plus an **instance** index, alongside its
concrete name:

```
svc_register(name, class, instance, flags)          // class/instance optional
svc_lookup(name)              -> endpoint            // unchanged
svc_lookup_class(class)       -> [ {instance, endpoint, pid} ]   // enumerate all
svc_subscribe_class(class, endpoint)                 // notify on add/remove/die
```

Multiple providers may share one class — `virtio-net` registers `class="net.ifc"
instance=0`; a second NIC (or an `e1000` driver) is just `instance=1`, with no
client change.  Consumers resolve the class, not the driver.

This lives in the PM/kernel — a `(class, instance) → endpoint + metadata` table
beside the existing name table — for three reasons that rule out a standalone
lookup *service*:

- **Bootstrap.** A separate registry would itself need a well-known endpoint to
  be found; the kernel-resident PM endpoint is already that root.
- **Death reaping.** Registry entries must vanish when a provider dies, or
  lookups hand out dead endpoints.  The PM already owns process lifecycle and
  the exit hook, so it purges entries (and fires remove-notifications) for free.
- **Anti-spoof.** "Who may register under class `net.ifc`?" must be
  capability-gated, or any app could impersonate an interface — and the kernel
  already enforces capabilities on IPC.

The registry emits only **existence** events (a provider registered / unregistered
/ died under a class) to class subscribers.  **Domain** events — link up/down,
media change — are provider-specific and travel over the provider's own protocol
(e.g. a `NETDRV_IPC_LINK_NOTIFY` from the NIC driver), not the registry.

Guardrail: the kernel piece stays limited to register / lookup / enumerate /
existence-notify / death-reap.  All *policy* — human-facing naming (`eth0`),
address assignment, routing, provider selection — lives in the consuming service
(for networking, the net-stack; see
[Networking](22-networking-virtio-net-and-stack.md)).  This keeps the registry a
small primitive rather than a framework in the kernel.

#### Startup (spawn-info)

The legacy per-app entry-arg binding mechanism has been retired (the four entry
args are always zero). The child now retrieves all startup values from its
**spawn-info buffer** — a child-owned xfer buffer holding a versioned
`wasmos_spawn_info_t` header (`proc_endpoint`, `tty`, `module_count`,
`module_index`, args blob). WASM apps read it via the `spawn_info_buffer`
hostcall (`wasmos_startup_proc_endpoint()` / `_tty()` / `_module_count()` /
`_arg()` accessors); native drivers/services via `api->spawn_info(&hdr, buf, cap)`.
Service endpoints are resolved dynamically via `svc_lookup()` rather than bound
statically at spawn. See [Runtime and Packaging](13-runtime-and-packaging.md).

---

### Buffer-Borrow Mechanism

Bulk data between processes uses the buffer-borrow model. The architectural
object is a **transfer buffer** owned by one context and lent to another
context with explicit read and/or write grants. The same model is reused for
file data, spawn payloads, broker plans, packet payloads, and other non-message
bulk transfers.

**Buffer kinds:**

| Kind                    | Value | Used for                 |
|-------------------------|-------|--------------------------|
| transfer/xfer buffer    | 1     | Generic transfer buffer  |
| framebuffer buffer      | 2     | Framebuffer pixel data   |

**Semantics:**

1. A context owns a transfer buffer object for a given kind.
2. A borrower is granted access to that same buffer object with explicit
   read/write flags.
3. The borrower reads or writes through the granted borrow.
4. Releasing the borrow drops access to that shared buffer object; it does not
   imply "switch to a second reply buffer" or "upgrade the same borrow."

**Code-facing contract matrix:**

| Area               | Intended rule                                                                                                                               |
|--------------------|---------------------------------------------------------------------------------------------------------------------------------------------|
| Ownership          | A context may own multiple distinct buffer objects (each a stable `buffer_id`), including several of the same kind — one per live operation |
| Owner access       | The owner always retains implicit read/write access to its own object                                                                       |
| Borrow grant       | A borrow creates one grant from one owner object to one borrower context with explicit flags                                                |
| Object identity    | Borrower access is to the same underlying owner object, not a copied or shadow reply buffer                                                 |
| Concurrent grants  | One owner may lend to multiple borrowers, and one borrower may hold multiple grants to distinct owners at once                              |
| Release            | Release is grant-specific; releasing one grant must not affect other live grants                                                            |
| Revocation         | Dropping one owner revokes only grants sourced from that owner                                                                              |
| Transfer policy    | Transfer borrows require a nonzero external owner distinct from the borrower                                                                |
| Framebuffer policy | Framebuffer borrows are local-only and do not name an external owner                                                                        |
| DMA attach point   | DMA mapping attaches to one grant, not to the borrower context as a whole                                                                   |
| DMA directions     | `TO_DEVICE` requires read access; `FROM_DEVICE` requires write access; bidirectional DMA requires both                                      |
| DMA teardown       | Release while mapped is invalid; unmap clears DMA state but preserves the grant                                                             |

**Borrow constraint:**

The registry allows at most one active borrow **per object** (to keep borrow
lifecycle sequencing clean), but a borrower may hold multiple simultaneous
borrows to *distinct* objects. A relay reading from one buffer and writing to
another simply holds two independent borrows (`borrow_id`s). `buffer_id` and
`borrow_id` are stateless integer handles passed on the IPC wire.

#### Broker Contract Clarification

Brokered executable-plan delegation is the canonical example of the intended
borrow contract:

1. The caller owns a transfer buffer containing the request payload.
2. The broker is granted read access to that request buffer.
3. The plan reply is written through a separate write-capable transfer-buffer
   borrow.
4. Releasing one borrow only removes access to that one borrowed buffer
   object; it does not "flip" the process onto some implicit alternate buffer.

If an implementation only tracks one active borrow per borrower context, that
implementation is narrower than the broker contract and must be treated as a
known limitation to remove, not as the architectural rule.

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

### Synchronous request/response IPC — deadlock hazard (planned direction: futures)

The prevailing service pattern issues a request and then **blocks** on a reply
(`wasmos_ipc_send` + `wasmos_ipc_select_one`, or `wasmos_ipc_call`). When two
services each block on the other, they deadlock. This is a structural hazard,
not an incidental bug:

- Concrete instance: `fs-manager`, while handling a class-discovery event for a
  newly discovered FS backend, synchronously queried `device-manager`
  (`DEVMGR_QUERY_MOUNT_REQ`) for boot metadata — while `device-manager` was
  itself blocked waiting for `fs-manager` to answer its `/boot` rules read.
  Mutual wait → boot hang.
- Even without a hard deadlock, blocking round-trips serialize otherwise
  independent work and make boot ordering timing-sensitive (a caller that
  blocks cannot service unrelated requests that arrive meanwhile).

**Planned direction: remove blocking request/response entirely.** A request
returns a future/promise; the caller keeps pumping its single generic
`wasmos_sys_event_loop` (one receiver per endpoint), and the reply is delivered
as an event that resolves the future and runs its continuation. No service ever
parks inside a nested receive, so cross-service call cycles cannot deadlock and
a service always keeps serving. Until that lands, avoid synchronous
service→service calls inside a handler for another service's request.

---

### Invariants

1. **One endpoint, one receiver.** A single receive pump owns each service
   endpoint.  The select-set mechanism (`sched_event_t.wait_list`) supports
   multiple concurrent waiters per endpoint, but the service model uses one.

2. **Bulk data through borrows, not messages.** File content, spawn payloads,
   executable plans, pixel buffers, and packet data flow through borrowed
   kernel-managed transfer buffers. IPC messages carry only metadata.

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
