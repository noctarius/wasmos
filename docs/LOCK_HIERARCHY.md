# Kernel Lock Hierarchy

This document defines the required acquisition order for every spinlock in
the WASMOS kernel.  Violating the order introduces deadlock cycles.  Every
new lock site must be checked against this hierarchy before merging.

---

## Locks Inventory

| Lock                          | Location         | Purpose                                 |
|-------------------------------|------------------|-----------------------------------------|
| `g_pfa_lock`                  | `physmem.c`      | Physical page frame allocator free-list |
| `g_slab_lock`                 | `slab.c`         | Kernel slab/heap allocator              |
| `g_endpoint_table_lock`       | `ipc.c`          | IPC endpoint table scan/alloc           |
| `ep->lock`                    | `ipc_endpoint_t` | Per-endpoint queue and waiter state     |
| `g_process_table_lock`        | `process.c`      | Process slot array reads and writes     |
| `g_ready_queue_lock`          | `process.c`      | Scheduler ready queue                   |
| `g_thread_table_lock`         | `thread.c`       | Thread slot array reads and writes      |
| `g_contexts_lock`             | `memory.c`       | Virtual-address context table           |
| `g_shared_lock`               | `memory.c`       | Shared-memory region table              |
| `g_wasm3_heap_lock`           | `wasm3_shim.c`   | wasm3 per-process heap bookkeeping      |
| `g_serial_lock`               | `serial.c`       | Serial port output serialisation        |
| `g_wasm_driver_registry_lock` | `wasm_driver.c`  | WASM driver registry array              |
| `driver->lock`                | `wasm_driver_t`  | Per-driver state                        |

---

## Required Acquisition Order

### Scheduler / IPC group

These locks nest.  Acquire only in the order shown; never hold an inner lock
while attempting to acquire an outer one.

```
g_endpoint_table_lock          (1 — outermost IPC)
  └─ ep->lock                  (2 — per-endpoint, acquired while table lock
  |                                  is still held, then table lock released)
  |    └─ g_thread_table_lock  (3 — via process_block_on_ipc → thread_set_state)
  |
g_process_table_lock           (1 — outermost scheduler)
  ├─ g_ready_queue_lock        (2 — via ready_queue_enqueue_with_proc)
  └─ g_thread_table_lock       (2 — via thread_set_state, thread_owner_tid_at,
                                      thread_wake_if_blocked)

g_ready_queue_lock             (can also appear alone)
  └─ g_thread_table_lock       (via ready_queue_dequeue → thread_get)
```

`g_endpoint_table_lock` and `g_process_table_lock` are parallel roots —
neither is acquired while holding the other.

`g_thread_table_lock` is a leaf: nothing is acquired while holding it.

### Physical memory group

```
g_shared_lock                  (outer — shared-region table)
  └─ g_pfa_lock                (inner — pfa_alloc/free_pages called under
                                         g_shared_lock in mm_shared_create)

g_slab_lock                    (leaf — may call pfa internally; verified
                                        no reverse nesting exists)
```

`g_pfa_lock` is the innermost physical-memory lock.  Nothing that holds
`g_pfa_lock` may acquire `g_shared_lock` or `g_slab_lock`.

### Independent / leaf locks

These locks are never acquired while holding another spinlock, and no other
spinlock is acquired while holding them:

- `g_contexts_lock` — mm context table, short critical sections only
- `g_wasm3_heap_lock` — wasm3 heap bookkeeping, no outward calls
- `g_serial_lock` — serial output, no outward calls
- `g_wasm_driver_registry_lock` — driver registry array scan/insert
- `driver->lock` — per-driver state, no outward spinlock calls

---

## Cross-group rules

- No lock from the **scheduler / IPC group** is acquired while holding any
  lock from the **physical memory group**, and vice versa.
- `__atomic_*` and `__sync_*` operations on individual flags
  (`blocking_transition`, `g_sched_progress_logged`, `g_idle_process`, etc.)
  are not spinlocks and carry no ordering constraint relative to the above
  hierarchy — they use explicit memory-order arguments instead.

---

## Checklist for new lock sites

Before adding a `spinlock_lock` call, answer:

1. **Which locks (if any) are already held** at every call site?
2. **Is the new acquisition consistent** with the hierarchy above?
   - Scheduler group: are you acquiring a lock at a lower numbered level than
     everything already held?
   - Physical memory group: are you acquiring `g_pfa_lock` only when no other
     physical-memory lock is held at a higher level?
   - Independent locks: confirm the new lock does not call out to any other
     spinlock internally.
3. **Are there reverse paths?**  Search for any code that holds the *new* lock
   and calls into code that acquires any lock you hold at the call site.
4. **Add a comment** at the acquisition site citing the lock order, e.g.:
   ```c
   /* Lock order: g_process_table_lock → g_ready_queue_lock */
   spinlock_lock(&g_ready_queue_lock);
   ```

---

## Known exceptions and notes

- `g_endpoint_table_lock → ep->lock`: the table lock is held while acquiring
  the endpoint lock to prevent the slot being freed between lookup and lock.
  The table lock is released immediately after `ep->lock` is acquired.  All
  subsequent work is done under `ep->lock` alone.

- `g_shared_lock → g_pfa_lock`: `mm_shared_create` allocates physical pages
  (via `pfa_alloc_pages`) while holding `g_shared_lock`.  The reverse path
  (`g_pfa_lock` → `g_shared_lock`) does not currently exist; `g_pfa_lock`
  callers are the slab allocator and page-fault handler, neither of which
  touches shared regions.  This ordering must be preserved.

- SMP interrupt safety: every `spinlock_lock` saves and clears RFLAGS (cli)
  so IRQ handlers cannot run on the same CPU while any spinlock is held.
  The `cli` in `kernel_boot_run_scheduler_loop` is separate and only prevents
  re-entrant scheduler invocation from the timer ISR on the local CPU — it
  does not substitute for spinlocks on shared state.
