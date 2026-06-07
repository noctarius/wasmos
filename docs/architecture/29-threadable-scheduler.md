## Threadable Scheduler

This document specifies the design for WASMOS's next-generation scheduler,
codenamed "Threadable Scheduler" (`WASMOS_SCHED_THREADABLE`).  It replaces the
current process-centric FIFO scheduler with a thread-centric priority scheduler
that draws concrete inspiration from Minos2's kernel (`others/minos2-main/`).

The design is intended to be implemented behind a compile-time flag so both
schedulers coexist during the transition.  The authoritative source for the
current scheduler is `src/kernel/process.c`; the new implementation will live
alongside it in `src/kernel/sched_thread.c` plus modifications to shared
headers.

---

### Motivation

The current scheduler has four structural defects:

| Defect | Consequence |
|--------|-------------|
| `proc->state == READY` dequeue gate | On SMP, workers of a RUNNING process are silently orphaned; fixed today with an inline-pass hack |
| Shared `proc->ctx` for all workers | Only one kernel worker per process may be blocked at a time (saves to one slot) |
| No priority model | Native IRQ-dispatch workers contend with idle WASM processes for the same queue |
| Ad-hoc per-endpoint `waiter_tid` | No general multi-object wait; poll-hub required a new scan-on-send mechanism |

The Threadable Scheduler eliminates all four.

---

### Design Pillars

1. **Priority** — 7 priority levels; O(1) highest-ready find via per-CPU bitmap.
2. **Thread-centric dispatch** — dequeue guard is `thread->state == READY`, not
   `proc->state == READY`.  Multiple threads of one process may run on different
   CPUs simultaneously.
3. **Per-thread context** — every thread (including kernel workers) saves its
   blocked state in `thread->ctx`, not a shared `proc->ctx`.
4. **Unified event system** — a single `sched_event_t` struct unifies IPC-wait,
   process-wait, thread-join, futex, and select-wait into one blocking primitive.
5. **Futex** — two new host functions let WASM userspace build any
   synchronization primitive on top of `shmem_grant`-shared memory.
6. **Full poll-hub** — kernel objects register a `poll_struct *` and call
   `poll_notify(ps, ev)` on readiness; select sets become the consumer side.

---

### Priority Model

```c
/* src/kernel/include/sched.h */
#define SCHED_PRIO_MAX     7   /* number of priority levels */
#define SCHED_PRIO_BITS    7   /* bitmask width; one bit per level */

typedef enum {
    SCHED_PRIO_REALTIME   = 0, /* IRQ handler workers, timer callbacks */
    SCHED_PRIO_DRIVER     = 1, /* native drivers (fbpci, serial, ata, kbd) */
    SCHED_PRIO_SERVICE    = 2, /* system services (device-manager, fs-manager) */
    SCHED_PRIO_SYSTEM     = 3, /* kernel services (process-manager, chardev) */
    SCHED_PRIO_WASM       = 4, /* default WASM processes */
    SCHED_PRIO_BACKGROUND = 5, /* batch / background jobs */
    SCHED_PRIO_IDLE       = 6, /* idle process only */
} sched_prio_t;
```

Priority is per-thread.  Default assignment at spawn:

| Process type | Default priority |
|---|---|
| `is_idle == 1` | `SCHED_PRIO_IDLE` |
| Native kernel worker | `SCHED_PRIO_SYSTEM` |
| WASM native driver (`FLAG_DRIVER`) | `SCHED_PRIO_DRIVER` |
| WASM native service (`FLAG_NATIVE`) | `SCHED_PRIO_SERVICE` |
| WASM application | `SCHED_PRIO_WASM` |

`thread_t` gains one new field: `uint8_t sched_prio`.  Exposed via
`process_thread_set_priority(pid, tid, prio)` for future policy enforcement.

---

### Per-CPU Scheduler State

Replace the current global `g_ready_queue[]` FIFO with per-CPU priority queues:

```c
/* src/kernel/include/sched.h */
#define SCHED_PRIO_MAX 7

typedef struct {
    spinlock_t   lock;                        /* protects all fields below */
    uint8_t      ready_bitmap;                /* bit i = 1 → ready_list[i] non-empty */
    list_head_t  ready_list[SCHED_PRIO_MAX];  /* one FIFO per priority */
    uint32_t     thread_count[SCHED_PRIO_MAX];/* threads in each list */
    thread_t    *running;                     /* currently executing thread */
    thread_t    *idle;                        /* this CPU's idle thread */
    uint32_t     nr_threads;                  /* total threads on this CPU */
    struct timer sched_timer;                 /* round-robin quantum timer */
    list_head_t  new_list;                    /* cross-CPU wakeup arrivals */
} cpu_sched_t;
```

One `cpu_sched_t` lives in the existing `cpu_local_t` struct (in
`src/kernel/include/arch/x86_64/smp.h`), co-located with the fields already
there (`current_process`, `current_thread`, etc.).

**O(1) find-highest-ready:**

```c
/* ffs_table[bitmap] = index of lowest set bit, or 0xFF if bitmap==0 */
static const uint8_t ffs_table[128] = { 0xFF,0,1,0,2,0,1,0,... };

static inline int cpu_sched_highest_prio(cpu_sched_t *cs) {
    return ffs_table[cs->ready_bitmap & 0x7F];  /* 7 levels */
}
```

Equivalent to Minos2's `ffs_one_table[pcpu->local_rdy_grp]`.

**Enqueue (`cpu_sched_enqueue`):**

```c
void cpu_sched_enqueue(uint32_t cpu_id, thread_t *t) {
    cpu_sched_t *cs = &cpu_local_for(cpu_id)->sched;
    spinlock_lock(&cs->lock);
    list_add_tail(&cs->ready_list[t->sched_prio], &t->sched_node);
    cs->thread_count[t->sched_prio]++;
    cs->ready_bitmap |= (1u << t->sched_prio);
    if (t->sched_prio < cs->running->sched_prio)
        set_need_resched_on(cpu_id);     /* IPI if remote CPU */
    spinlock_unlock(&cs->lock);
}
```

**Dequeue (`cpu_sched_pick_next`):**

```c
thread_t *cpu_sched_pick_next(cpu_sched_t *cs) {
    /* Caller holds cs->lock */
    int prio = cpu_sched_highest_prio(cs);
    if (prio == 0xFF) return cs->idle;
    thread_t *t = list_first_entry(&cs->ready_list[prio], thread_t, sched_node);
    list_del(&t->sched_node);
    if (--cs->thread_count[prio] == 0)
        cs->ready_bitmap &= ~(1u << prio);
    return t;
}
```

**Round-robin timer:** when two or more threads share the highest ready priority,
`sched_timer` fires at the end of the current thread's quantum and calls
`set_need_resched()`.  The timer is armed/disarmed based on
`cs->thread_count[cs->running->sched_prio] > 1` — identical to Minos2's
`sched_update_sched_timer()`.

**Thread affinity:** `thread_t.cpu_affinity` (new field, `uint32_t`) is a CPU
mask.  Default `~0u` = any CPU.  `cpu_sched_enqueue` picks the least-loaded
eligible CPU; cross-CPU enqueue appends to `cpu_sched_t.new_list` and sends a
rescheduling IPI to the target.

---

### Thread-Centric Dispatch

#### 1. Remove proc->state gate from dequeue

Current `ready_queue_dequeue`:
```c
if (thread->state == THREAD_STATE_READY &&
    proc->state == PROCESS_STATE_READY) {   /* ← THIS IS THE BUG */
    return thread;
}
```

New `cpu_sched_pick_next` picks any `THREAD_STATE_READY` thread whose owning
process is not `PROCESS_STATE_ZOMBIE` or `PROCESS_STATE_UNUSED`.  No
`PROCESS_STATE_RUNNING` exclusion.

Multiple threads of the same process may run on different CPUs simultaneously.
This requires:

#### 2. Atomic modifications to shared process state

Fields that are now written by potentially multiple CPUs:

| Field | Protection |
|---|---|
| `proc->live_thread_count` | `__atomic_fetch_sub(..., __ATOMIC_ACQ_REL)` |
| `proc->state` ALIVE→ZOMBIE | `__atomic_compare_exchange` (CAS) |
| `proc->exit_status` | set once before CAS to ZOMBIE |
| `proc->exiting` | `__atomic_store_n(..., __ATOMIC_RELEASE)` |

`proc->state` simplifies to:

```c
typedef enum {
    PROCESS_STATE_UNUSED  = 0,
    PROCESS_STATE_ALIVE   = 1,   /* replaces READY/RUNNING/BLOCKED */
    PROCESS_STATE_REAPING = 2,   /* being reaped, slots still valid */
    PROCESS_STATE_ZOMBIE  = 3,
} process_state_t;
```

The READY/RUNNING/BLOCKED distinction is eliminated at the process level.
Per-thread `thread_state_t` carries the full READY/RUNNING/BLOCKED/ZOMBIE
lifecycle.

#### 3. Per-thread context for all threads

`process_sched_ctx_for_thread` is simplified:

```c
/* Before */
if (thread->is_kernel_worker) return &proc->ctx;
return &thread->ctx;

/* After */
return &thread->ctx;   /* always — workers use their own ctx */
```

`proc->ctx` (the shared `process_context_t` embedded in `process_t`) is
**removed**.  The `ctx_canary_pre` and `ctx_canary_post` fields move to bracket
`thread->ctx` per thread.

Each `thread_t` therefore has:

```c
typedef struct thread {
    /* ... existing fields ... */
    uint64_t            ctx_canary_pre;   /* NEW: moved from process_t */
    process_context_t   ctx;             /* save/restore for this thread */
    uint64_t            ctx_canary_post;  /* NEW: moved from process_t */
    uint8_t             sched_prio;       /* NEW: scheduler priority */
    uint32_t            cpu_affinity;     /* NEW: allowed CPU mask */
    list_head_t         sched_node;       /* NEW: linkage in cpu_sched_t.ready_list */
    list_head_t         event_node;       /* NEW: linkage in sched_event_t.wait_list */
    sched_event_t      *wait_event;       /* NEW: event this thread is blocked on */
    uint32_t            pend_state;       /* NEW: PEND_OK / PEND_TIMEOUT / PEND_ABORT */
    uint64_t            pend_data;        /* NEW: value from waker (futex retcode etc.) */
    /* ... existing fields ... */
} thread_t;
```

#### 4. Worker scheduling in process_schedule_once_impl

```c
if (thread->is_kernel_worker) {
    if (thread->ctx.rsp != 0) {
        /* Thread previously blocked; resume from saved context */
        context_switch_high(&cpu_local()->sched_ctx, &thread->ctx);
    } else {
        /* Fresh thread; call entry on its kstack */
        cpu_local()->last_run_result =
            process_run_worker_on_stack(proc, thread);
    }
}
```

`proc->ctx.rsp` is now gone.  The fresh/resume decision uses `thread->ctx.rsp`
which is zero-initialized at thread spawn (via `list_alloc` `memset`) and
non-zero only after `process_yield(BLOCKED)` has saved the context.

The **inline worker pass** (`if (!thread->is_kernel_worker && result == PROCESS_RUN_YIELDED)`)
introduced as a temporary SMP workaround is **removed entirely**.  Workers are
directly dequeued when ready; no proc->state gate blocks them.

---

### Unified Event System

Minos2's `struct event` is adapted to WASMOS:

```c
/* src/kernel/include/sched_event.h */

typedef enum {
    SCHED_EVENT_TYPE_IPC      = 0,  /* IPC endpoint message or notification */
    SCHED_EVENT_TYPE_JOIN     = 1,  /* thread join */
    SCHED_EVENT_TYPE_PROCESS  = 2,  /* process-level wait */
    SCHED_EVENT_TYPE_SELECT   = 3,  /* select-set multi-endpoint wait */
    SCHED_EVENT_TYPE_FUTEX    = 4,  /* futex wait */
    SCHED_EVENT_TYPE_TIMER    = 5,  /* deadline timer */
} sched_event_type_t;

typedef enum {
    SCHED_PEND_NONE    = 0,
    SCHED_PEND_OK      = 1,  /* woken with data */
    SCHED_PEND_TIMEOUT = 2,  /* woken by timeout */
    SCHED_PEND_ABORT   = 3,  /* woken by process kill / endpoint destroy */
} sched_pend_state_t;

typedef struct {
    spinlock_t    lock;
    list_head_t   wait_list;    /* thread_t.event_node members */
    uint32_t      cnt;          /* semaphore count (TYPE_IPC / TYPE_SELECT) */
    sched_event_type_t type;
} sched_event_t;
```

**Blocking a thread on an event:**

```c
/* Called with event->lock held; releases it on return */
void sched_event_wait(sched_event_t *ev, uint32_t timeout_ms) {
    thread_t *t = current_thread();
    t->wait_event = ev;
    t->pend_state = SCHED_PEND_NONE;
    t->pend_data  = 0;
    __atomic_store_n(&t->blocking_transition, 1, __ATOMIC_RELEASE);
    list_add_tail(&ev->wait_list, &t->event_node);
    thread_set_state(t->tid, THREAD_STATE_BLOCKED, THREAD_BLOCK_EVENT);
    /* start optional timeout timer here (timeout_ms != 0) */
    spinlock_unlock(&ev->lock);
    process_yield(PROCESS_RUN_BLOCKED);
    /* blocking_transition cleared by PROCESS_RUN_BLOCKED handler */
}
```

**Waking one waiter:**

```c
/* Lock ordering: caller must hold event->lock */
thread_t *sched_event_wake_one(sched_event_t *ev,
                               uint64_t data,
                               sched_pend_state_t pend) {
    if (list_empty(&ev->wait_list)) return NULL;
    thread_t *t = list_first_entry(&ev->wait_list, thread_t, event_node);
    list_del(&t->event_node);
    t->wait_event = NULL;
    t->pend_state = pend;
    t->pend_data  = data;
    /* sched_wake_thread sets t->state = WAKING, then enqueues */
    sched_wake_thread(t);
    return t;
}

/* Broadcast: wake all waiters */
int sched_event_wake_all(sched_event_t *ev,
                         uint64_t data,
                         sched_pend_state_t pend);
```

**`sched_wake_thread`** (replaces `process_wake_thread`):

```c
void sched_wake_thread(thread_t *t) {
    /* Wait for the blocking_transition flag to clear (SMP: context save
     * must complete on the sleeping CPU before we enqueue elsewhere).
     * Mirrors ipc_recv_blocking_for's blocking_transition guard. */
    while (__atomic_load_n(&t->blocking_transition, __ATOMIC_ACQUIRE))
        cpu_relax();

    uint32_t target_cpu = t->last_cpu;   /* affinity hint */
    thread_set_state(t->tid, THREAD_STATE_READY, THREAD_BLOCK_NONE);
    cpu_sched_enqueue(target_cpu, t);    /* may IPI if remote CPU */
}
```

#### Migrating existing blocking sites

| Current mechanism | New mechanism |
|---|---|
| `ep->waiter_tid` in `ipc_endpoint_t` | `sched_event_t ep->event` embedded in `ipc_endpoint_t`; `waiter_tid` removed |
| `select_set.ready / waiter_tid` | `sched_event_t sel->event` |
| `thread->join_waiter_tid` | `sched_event_t thread->join_event` |
| `process_wait` PID-wait | `sched_event_t proc->wait_event` |
| `process_block_on_ipc` | `sched_event_wait(&ep->event, timeout)` |

The existing `ep->waiter_tid` / `process_wake_thread` path stays as a fast
inner path inside `ipc_endpoint_t` wakeup: `sched_event_wake_one(&ep->event, 0, SCHED_PEND_OK)`.
The difference is that `sched_event_t.wait_list` can hold more than one waiter,
enabling the thread-pool pattern (multiple threads competing on one endpoint).

---

### Futex Primitive

Modelled directly on Minos2's `kernel/userspace/futex.c`.

#### Kernel structures

```c
/* src/kernel/futex.c */

typedef struct {
    uintptr_t     paddr;   /* physical address of the futex word */
    sched_event_t event;   /* embedded event for waiters */
    list_head_t   list;    /* linkage in g_futex_table bucket */
} futex_t;

#define FUTEX_TABLE_BITS  4                       /* 16 buckets */
#define FUTEX_TABLE_SIZE  (1u << FUTEX_TABLE_BITS)

static struct {
    spinlock_t   lock;
    list_head_t  head;
} g_futex_table[FUTEX_TABLE_SIZE];

static inline int futex_bucket(uintptr_t paddr) {
    return (paddr >> PAGE_SHIFT) & (FUTEX_TABLE_SIZE - 1);
}
```

#### futex_wait

```c
int futex_wait(uint32_t __user *uaddr, uint32_t expected,
               uint32_t timeout_ms, uint32_t context_id) {
    uintptr_t paddr = mm_uva_to_paddr(context_id, (uintptr_t)uaddr);
    if (!paddr) return -IPC_ERR_INVALID;

    int bucket = futex_bucket(paddr);
    spinlock_lock(&g_futex_table[bucket].lock);

    /* Find or create futex entry */
    futex_t *ft = futex_find(paddr, bucket);   /* linear scan; ≤ bucket entries */
    if (!ft) {
        ft = futex_alloc(paddr, bucket);        /* allocate from slab or kmalloc */
        if (!ft) { spinlock_unlock(...); return -IPC_ERR_FULL; }
    }

    spinlock_lock(&ft->event.lock);
    spinlock_unlock(&g_futex_table[bucket].lock);

    uint32_t *kaddr = mm_paddr_to_kva(paddr);
    if (*kaddr != expected) {
        spinlock_unlock(&ft->event.lock);
        return 0;     /* word changed; caller retries */
    }

    sched_event_wait(&ft->event, timeout_ms);   /* blocks; releases ft->event.lock */

    /* On wakeup check pend_state */
    thread_t *t = current_thread();
    return (t->pend_state == SCHED_PEND_TIMEOUT) ? -ETIMEDOUT : 0;
}
```

#### futex_wake

```c
int futex_wake(uint32_t __user *uaddr, uint32_t count,
               uint32_t context_id) {
    uintptr_t paddr = mm_uva_to_paddr(context_id, (uintptr_t)uaddr);
    if (!paddr) return 0;

    int bucket = futex_bucket(paddr);
    spinlock_lock(&g_futex_table[bucket].lock);

    futex_t *ft = futex_find(paddr, bucket);
    if (!ft) { spinlock_unlock(...); return 0; }

    spinlock_lock(&ft->event.lock);
    spinlock_unlock(&g_futex_table[bucket].lock);

    int woken = 0;
    for (uint32_t i = 0; i < count; i++) {
        thread_t *t = sched_event_wake_one(&ft->event, 0, SCHED_PEND_OK);
        if (!t) break;
        woken++;
    }

    spinlock_unlock(&ft->event.lock);
    return woken;
}
```

#### WASM host functions

```c
/* wasm3_link.c */
m3ApiRawFunction(wasmos_futex_wait)  /* "i(iii)" — addr, expected, timeout_ms */
m3ApiRawFunction(wasmos_futex_wake)  /* "i(ii)"  — addr, count             */
```

```c
/* api.h */
extern int32_t wasmos_futex_wait(int32_t addr, int32_t expected, int32_t timeout_ms)
    WASMOS_WASM_IMPORT("wasmos", "futex_wait");
extern int32_t wasmos_futex_wake(int32_t addr, int32_t count)
    WASMOS_WASM_IMPORT("wasmos", "futex_wake");
```

`addr` is a WASM linear-memory offset.  The host function translates it through
`mm_uva_to_paddr(proc->context_id, wasm_linear_base + addr)`.

With these two primitives, a WASM process can implement:

```c
/* libsys mutex — userspace, no kernel involvement after first contend */
typedef struct { uint32_t state; } wasmos_mutex_t;   /* 0=free, 1=locked */

static void mutex_lock(wasmos_mutex_t *m) {
    while (__sync_val_compare_and_swap(&m->state, 0, 1) != 0)
        wasmos_futex_wait((int32_t)(uintptr_t)&m->state, 1, 0);
}
static void mutex_unlock(wasmos_mutex_t *m) {
    __atomic_store_n(&m->state, 0, __ATOMIC_RELEASE);
    wasmos_futex_wake((int32_t)(uintptr_t)&m->state, 1);
}
```

---

### Full Poll-Hub (Event Multiplexing)

The current select-set implementation (`ipc_select_t`) scans the table on every
send.  The full design attaches a `poll_struct` to each kernel object and pushes
notifications to registered hubs.

#### poll_struct: per-object registration table

```c
/* src/kernel/include/poll.h */

#define POLL_EV_MAX 4   /* EV_IN, EV_OUT, EV_CLOSE, EV_KERNEL */

typedef enum {
    POLL_EV_IN     = 0,   /* data / message ready to read */
    POLL_EV_OUT    = 1,   /* space available to send */
    POLL_EV_CLOSE  = 2,   /* far end closed / endpoint destroyed */
    POLL_EV_KERNEL = 3,   /* kernel-internal (timer, IRQ, etc.) */
} poll_ev_t;

typedef struct poll_watcher {
    struct ipc_select  *sel;         /* select set watching this event */
    uint32_t            user_data;   /* caller-supplied opaque value */
    struct poll_watcher *next;       /* linked list per event type */
} poll_watcher_t;

typedef struct {
    poll_watcher_t *watchers[POLL_EV_MAX];  /* head of watcher list per event */
} poll_struct_t;
```

`poll_struct_t` is allocated lazily when the first `ipc_select_add` targets an
endpoint.  The pointer is stored in `ipc_endpoint_t.poll_struct` (new field).

#### Registration: ipc_select_add (redesigned)

```c
int ipc_select_add(uint32_t select_id, uint32_t endpoint_id) {
    ipc_select_t *sel = ipc_select_find_locked(select_id);
    if (!sel) return IPC_ERR_INVALID;

    ipc_endpoint_t *ep = ipc_endpoint_get(endpoint_id);
    if (!ep) { spinlock_unlock(&sel->lock); return IPC_ERR_INVALID; }

    /* Allocate poll_struct on ep if first watcher */
    if (!ep->poll_struct)
        ep->poll_struct = kzalloc(sizeof(poll_struct_t));

    /* Register watcher for EV_IN (message ready) */
    poll_watcher_t *w = kzalloc(sizeof(poll_watcher_t));
    w->sel = sel;
    w->user_data = 0;
    w->next = ep->poll_struct->watchers[POLL_EV_IN];
    ep->poll_struct->watchers[POLL_EV_IN] = w;

    sel->ep_ids[sel->ep_count++] = endpoint_id;
    spinlock_unlock(&ep->lock);
    spinlock_unlock(&sel->lock);
    return IPC_OK;
}
```

#### Notification: poll_notify (replaces scan-on-send)

```c
void poll_notify(poll_struct_t *ps, poll_ev_t ev, uint32_t ep_id) {
    if (!ps) return;
    poll_watcher_t *w = ps->watchers[ev];
    while (w) {
        ipc_select_signal_sel(w->sel, ep_id);  /* wake sel->event */
        w = w->next;
    }
}
```

In `ipc_send_from` (replacing the current `g_active_select_count` check):

```c
/* After spinlock_unlock(&ep->lock): */
uint32_t ep_id = ep->id;
poll_struct_t *ps = ep->poll_struct;   /* read without lock (immutable once created) */
smp_rmb();
if (ps)
    poll_notify(ps, POLL_EV_IN, ep_id);
```

This eliminates the O(N) global table scan entirely.  Notifications go directly
from the sender to only the registered select sets — O(watchers per endpoint),
typically O(1) in practice.

#### ipc_select_destroy: cleanup

```c
void ipc_select_destroy(uint32_t select_id) {
    /* Unregister from all endpoint poll_structs */
    for (uint32_t i = 0; i < sel->ep_count; i++) {
        ipc_endpoint_t *ep = ipc_endpoint_get(sel->ep_ids[i]);
        if (!ep) continue;
        poll_struct_remove(ep->poll_struct, POLL_EV_IN, sel);
        spinlock_unlock(&ep->lock);
    }
    /* Wake any blocked waiter, mark in_use=0, decrement counter */
}
```

---

### WASM Reentrancy Guard

wasm3 is not reentrant.  With multiple threads per process now possible, a
per-process guard prevents concurrent wasm3 execution:

```c
/* process_t gains: */
spinlock_t wasm3_lock;    /* held for the duration of wasm3 entry_fn call */
uint32_t   wasm3_owner;   /* TID that currently holds the lock; 0 = free */
```

In `process_trampoline`:

```c
/* Before calling entry_fn: */
spinlock_lock(&proc->wasm3_lock);
proc->wasm3_owner = thread_current_tid();

cpu_local()->last_run_result = entry_fn(proc, proc->arg);

proc->wasm3_owner = 0;
spinlock_unlock(&proc->wasm3_lock);
```

If a second WASM thread is dispatched and `wasm3_lock` is held, it will spin
briefly.  The spinlock must not be held across a `process_yield` — the entry
function must complete its current step (PROCESS_RUN_YIELDED) before wasm3 state
is safe for the next thread.

Kernel worker threads (native C, `is_kernel_worker = 1`) do not go through
`process_trampoline` and never acquire `wasm3_lock`.

---

### Lock Hierarchy

Strict global ordering to prevent deadlock.  From outermost to innermost:

```
cpu_sched_t.lock         (per-CPU ready queue)
  │
  └─► process_t.wasm3_lock   (WASM reentrancy guard, if needed)
        │
        └─► sched_event_t.lock   (event wait list)
              │
              └─► thread_t.s_lock  (transition guard, held briefly)
                    │
                    └─► ipc_endpoint_t.lock  (message queue + poll_struct)
                          │
                          └─► futex_table_bucket.lock  (futex hash bucket)
```

Rules:
- **Never** acquire a coarser lock while holding a finer one.
- `cpu_sched_t.lock` is acquired with IRQs saved (`spinlock_lock_irqsave`) when
  modifying `ready_list` from an IRQ context.
- `sched_event_t.lock` is always acquired before the thread transitions to
  BLOCKED, and released before `process_yield` is called.
- `ipc_endpoint_t.lock` is acquired under `g_endpoint_table_lock` (existing
  lock order, unchanged).
- Futex bucket lock is a leaf lock (nothing acquired while it is held).

---

### Data Structure Summary of Changes

#### `thread_t` additions

| Field | Type | Purpose |
|---|---|---|
| `ctx_canary_pre` | `uint64_t` | Moved from `process_t` |
| `ctx_canary_post` | `uint64_t` | Moved from `process_t` |
| `sched_prio` | `uint8_t` | Priority 0–6 |
| `cpu_affinity` | `uint32_t` | Allowed CPU bitmask |
| `sched_node` | `list_head_t` | Linkage in `cpu_sched_t.ready_list` |
| `event_node` | `list_head_t` | Linkage in `sched_event_t.wait_list` |
| `wait_event` | `sched_event_t *` | Event this thread is blocked on |
| `pend_state` | `uint32_t` | `SCHED_PEND_*` set by waker |
| `pend_data` | `uint64_t` | Value from waker (futex retcode, etc.) |
| `blocking_transition` | `uint8_t` (atomic) | Already present; kept |
| `last_cpu` | `uint32_t` | CPU where thread last ran |

#### `thread_t` removals

None — fields only added, existing fields kept for compatibility during
transition.

#### `process_t` changes

| Field | Change |
|---|---|
| `ctx` (`process_context_t`) | Removed (was shared worker ctx) |
| `ctx_canary_pre/post` | Removed (moved to per-thread) |
| `state` | Simplified to UNUSED/ALIVE/REAPING/ZOMBIE |
| `block_reason` | Removed (now per-thread `wait_event`) |
| `in_ready_queue` | Removed (process no longer queued directly) |
| `wasm3_lock` | **New**: per-process wasm3 reentrancy spinlock |
| `wasm3_owner` | **New**: TID of current wasm3 occupant |

#### `ipc_endpoint_t` changes

| Field | Change |
|---|---|
| `waiter_tid` | Replaced by `sched_event_t event` |
| `poll_struct` | **New**: `poll_struct_t *` (lazy-allocated) |

---

### Migration Strategy

1. **Flag:** `cmake -DWASMOS_SCHED_THREADABLE=ON`
2. **New files:**
   - `src/kernel/sched_thread.c` — `cpu_sched_t` operations, enqueue/dequeue, priority logic
   - `src/kernel/sched_event.c` — `sched_event_t` wait/wake
   - `src/kernel/futex.c` — futex hash table, wait/wake
   - `src/kernel/poll.c` — `poll_struct_t` notify, register, destroy
   - `src/kernel/include/sched.h`, `sched_event.h`, `poll.h`, `futex.h`
3. **Modified files:** `process.c`, `process.h`, `thread.h`, `ipc.c`, `ipc.h`,
   `wasm3_link.c`, `api.h`, `libsys.h`
4. **Removed code (after flag flip):** inline worker pass (added this session),
   `g_active_select_count` global, `g_ready_queue` FIFO.
5. **Test gate:** all existing `run-qemu-test` and `run-qemu-cli-test` markers
   must pass under the new scheduler before the flag defaults to ON.

---

### What Stays Unchanged

- `context_switch.S` assembly — the register save/restore format (`process_context_t`)
  is unchanged; the new scheduler uses the same switch routine.
- `process_trampoline` — same trampoline; gains wasm3_lock acquire/release.
- Preemption path (`process_preempt_from_irq`) — still fires on CPL3 frames;
  rewrites frame to jump to `process_preempt_trampoline`.
- IPC message format (`ipc_message_t`) and endpoint creation API.
- WASM packaging, app format, and libsys IPC call patterns.
- Select-set WASM API (`wasmos_ipc_select_*`) — implementation changes internally
  to use poll_struct push rather than scan-on-send.

---

### Relationship to Minos2 Concepts

| Minos2 construct | WASMOS adaptation |
|---|---|
| `pcpu.local_rdy_grp` bitmap + `ffs_one_table` | `cpu_sched_t.ready_bitmap` + `ffs_table` |
| `pcpu.ready_list[OS_PRIO_MAX]` | `cpu_sched_t.ready_list[SCHED_PRIO_MAX]` |
| `task.prio` | `thread_t.sched_prio` |
| `struct event` + `wait_list` | `sched_event_t` |
| `__wait_event` / `do_wait_event` | `sched_event_wait` |
| `__wake_up_event_waiter` | `sched_event_wake_one` / `sched_event_wake_all` |
| `TASK_STATE_WAKING` | thread BLOCKED → READY transition (existing `blocking_transition` flag) |
| `futex_t` hash table | `g_futex_table[FUTEX_TABLE_SIZE]` |
| `poll_struct` + `pevent_item` | `poll_struct_t` + `poll_watcher_t` |
| `poll_event_send` | `poll_notify` |
| `poll_hub` + `__poll_hub_read` | `ipc_select_t` + `ipc_select_wait` (existing, with push model) |
| `sem_pend` via `iqueue.isem` | endpoint blocking via `sched_event_wait(&ep->event, to)` |

Key divergence from Minos2: WASMOS does not adopt Minos2's user-kernel kobject
hierarchy (`struct kobject`, handles, `right_t`).  The existing capability model
(`capability_has`) and endpoint ownership (`owner_context_id`) are retained.
The poll-hub is implemented at the IPC layer only, not as a general kobject
property.
