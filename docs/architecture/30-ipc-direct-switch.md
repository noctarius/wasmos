## IPC Direct Switch

This document specifies the IPC direct-switch optimization for WASMOS: when a
caller makes a synchronous IPC request to a service whose receiver thread is
currently blocked waiting for work (off the ready queue), the kernel bypasses the
scheduler and switches directly to the service thread, donating the caller's
time-slice for the duration of the request.  The chain extends transitively — if
the service immediately calls a downstream service, that call also direct-switches
without returning to the scheduler.

The authoritative implementation sources will be `src/kernel/ipc.c`,
`src/kernel/process.c`, `src/kernel/wasm3_link.c`, and
`src/libc/include/wasmos/ipc.h`.  This doc relates to
`docs/architecture/09-process-and-ipc.md` (IPC transport),
`docs/architecture/07-scheduling-and-preemption.md` (scheduler), and
`docs/architecture/29-threadable-scheduler.md` (per-CPU queues, event system).

---

### Motivation

Caller-CPU bias (already in place) ensures that when a sender wakes a receiver,
the receiver is enqueued on the sender's CPU.  This eliminates cross-CPU IPC
spinlock contention and improves cache locality.  However, it does not eliminate
the scheduler round-trip: the sender still blocks, control returns to the
scheduler, the scheduler dequeues the receiver, and finally dispatches it.  A
second round-trip unwinds the reply.

For a single app → service → driver request chain (three hops), that is six
scheduler calls and six context switches, all on the same CPU and all for work
that could run sequentially on that CPU without interruption.

The direct-switch optimization reduces each synchronous hop from two scheduler
calls to zero when the fast-path guard conditions are satisfied.  The receiver
runs immediately in the caller's scheduling slot.  For a three-hop chain, this
collapses six context switches to one (the initial dispatch of the app) plus one
for the final return to the app after the whole chain has completed.

The optimization is transparent to receivers: a service that loops on
`wasmos_ipc_recv` does not need to change.  Only callers that use the new
`wasmos_ipc_call` hostcall (or the ring3 `IPC_CALL` syscall, which already
exists) participate.

---

### Terminology

| Term                     | Meaning                                                                                                           |
|--------------------------|-------------------------------------------------------------------------------------------------------------------|
| **sync call**            | A send immediately followed by a blocking wait for the matching reply, issued as a single atomic kernel operation |
| **direct switch**        | Bypassing the scheduler to transfer CPU control directly from caller to receiver                                  |
| **call chain**           | The transitive sequence of direct-switch hops: A → B → C → B → A                                                  |
| **chain depth**          | The number of outstanding direct-switch frames on the current CPU                                                 |
| **fast path**            | The direct-switch code path, taken when all guard conditions hold                                                 |
| **fallback path**        | Normal `ipc_send_from` + `sched_wake_thread`, taken when any guard fails                                          |
| **reply fast path**      | The symmetric direct-switch back from receiver to caller when the receiver sends to the caller's reply endpoint   |
| **priority inheritance** | Temporarily raising a receiver's priority to at least the caller's priority for the duration of the call          |

---

### Scope and Non-Goals

In scope:
- New `"wasmos"."ipc_call"` WASM hostcall (kernel primitive; replaces the
  user-space `send + recv` helper for callers that want the fast path)
- Fast-path guard evaluation and direct-switch in `ipc_send_from`
- Reply fast path: direct switch back on `ipc_send` to a sync-call reply endpoint
- Priority inheritance through the chain
- Chain depth limit and fallback to normal IPC
- `libsys` and `libc` migration for high-frequency call sites

Out of scope:
- Asynchronous or fire-and-forget IPC (no change)
- Notification endpoints (no change)
- Multi-waiter endpoints (the fast path requires exactly one blocked receiver)
- Kernel-to-kernel calls that do not pass through `ipc_send_from`
- Cross-CPU considerations specific to per-CPU queue designs (deferred; see SMP Safety section)

---

### Current State

**Ring3 `IPC_CALL` (syscall 6)**: a single kernel operation — sends to the
destination, then calls `ipc_recv_blocking_for(source_endpoint)` — but it goes
through the normal scheduler.  The direct-switch optimization will be applied to
this path in Phase 1.

**WASM `wasmos_ipc_call` helper**: a user-space C function in
`src/libc/include/wasmos/ipc.h` that wraps `wasmos_ipc_send` + `wasmos_ipc_recv`
in a retry loop.  It is not a kernel primitive; the kernel sees two separate
hostcall invocations and cannot apply direct-switch.  Phase 2 introduces a real
`"wasmos"."ipc_call"` hostcall.

**Native driver/service API (`wasmos_driver_api_t`)**: native services such as
`font-service` and `gfx-compositor` run in kernel mode and call IPC through
function-pointer callbacks in `wasmos_driver_api_t`.  `wasmos_sys_ipc_call_native`
in `libsys_native.c` is the intended sync-call helper for this path, but
currently calls `api->ipc_send` + `wasmos_sys_ipc_recv_matching_native` as two
separate operations, so the kernel cannot apply direct-switch.  Phase 3 adds an
`ipc_call` function pointer to `wasmos_driver_api_t` backed by the same
fast-path kernel function, and updates `wasmos_sys_ipc_call_native` to call it.
Native services that currently bypass `wasmos_sys_ipc_call_native` with local
spin-poll wrappers (font-service, gfx-compositor) migrate to it as part of
Phase 3.

**Caller-CPU bias**: `sched_wake_thread` sets `receiver->last_cpu =
cpu_local()->cpu_id` before enqueuing the receiver.  This keeps receiver cache
state warm relative to the caller but is not itself a guard condition for the
direct-switch path.

---

### Fast-Path Guard Conditions

The current scheduler uses a single global ready queue (`g_cpu_sched`).  A
BLOCKED thread is simply off that queue; it carries no CPU affiliation.
"Idle on the same CPU" is therefore not a meaningful concept today — if a thread
is BLOCKED, it is available to be run on any CPU.

Four conditions must hold for the kernel to take the direct-switch path.  If any
fails, the fallback path is taken silently.

| #  | Condition                                                      | Rationale                                                         |
|----|----------------------------------------------------------------|-------------------------------------------------------------------|
| G1 | `ep->count == 0` — endpoint queue is empty                     | No queued messages precede ours; FIFO ordering preserved          |
| G2 | `ep->event.wait_list` has exactly one entry                    | Exactly one thread is directly waiting; we know who to switch to  |
| G3 | `receiver->state == THREAD_STATE_BLOCKED`                      | Receiver is not running on any CPU                                |
| G4 | `cpu_local()->sched.chain_depth < IPC_DIRECT_SWITCH_MAX_DEPTH` | Chain depth limit not exceeded                                    |

G1 + G2 describe the "service is idle" state: endpoint is drained and one thread
is directly waiting for the next message.  G3 confirms the receiver is available.
G4 prevents unbounded stack growth from deeply nested synchronous chains.

**Select-set threads**: a thread waiting on a select set blocks on `sel->event`,
not on any individual endpoint event.  G2 handles this correctly: if the only
waiter is select-blocked, the endpoint wait_list is empty and G2 fails.  The
fallback path delivers the message normally via `poll_notify`.

**Cache locality note**: `receiver->last_cpu` records the last CPU that ran the
receiver.  When the caller-CPU bias is in place, services tend to have
`last_cpu == cpu_local()->cpu_id` for their primary clients, so direct-switch
naturally tends to keep hot working sets on the same CPU.  This is a consequence
of the bias, not a guard condition.  The fast path is taken regardless of
`last_cpu`; it never falls back on a cold-cache basis alone.

**Future: per-CPU queues.** When per-CPU ready queues arrive (see
`docs/architecture/29-threadable-scheduler.md`), the picture changes: a
BLOCKED thread may be conceptually associated with a specific CPU's queue.
At that point, `last_cpu` becomes a stronger placement signal and an optional
same-CPU guard may be added as G6 to avoid unnecessary cross-CPU migrations.
The current design deliberately omits it.

```c
#define IPC_DIRECT_SWITCH_MAX_DEPTH  8
```

---

### Thread Data Structure Changes

Three fields are added to `thread_t` (in `src/kernel/include/thread.h`):

```c
typedef struct thread {
    /* ... existing fields unchanged ... */

    /* IPC direct-switch chain state.
     * sync_reply_ep:   endpoint this thread is waiting for a reply on;
     *                  valid only while block_reason == THREAD_BLOCK_SYNC_CALL.
     * sync_chain_depth: nesting depth in the current direct-switch chain;
     *                  0 means this thread is not inside a chain.
     * sync_prio_saved: sched_prio before priority inheritance; restored on reply. */
    uint32_t  sync_reply_ep;
    uint8_t   sync_chain_depth;
    uint8_t   sync_prio_saved;
} thread_t;
```

A new block reason is added to `thread_block_reason_t`:

```c
typedef enum {
    THREAD_BLOCK_NONE = 0,
    THREAD_BLOCK_IPC,
    THREAD_BLOCK_WAIT_PROCESS,
    THREAD_BLOCK_WAIT_THREAD,
    THREAD_BLOCK_EVENT,
    THREAD_BLOCK_SYNC_CALL,   /* NEW: blocked waiting for a direct-switch reply */
} thread_block_reason_t;
```

A chain-depth counter is added to `cpu_sched_t` (in
`src/kernel/include/arch/x86_64/smp.h`):

```c
typedef struct {
    /* ... existing fields ... */
    uint8_t  chain_depth;   /* NEW: direct-switch nesting depth on this CPU */
} cpu_sched_t;
```

---

### New WASM Hostcall: `wasmos_ipc_call`

A new kernel hostcall is registered alongside `ipc_send` and `ipc_recv`.

**Kernel registration (wasm3_link.c):**
```c
rc |= wasm3_link_raw(module, "wasmos", "ipc_call", "i(iiiiiiii)", wasmos_ipc_call_hc);
```

**Signature (api.h):**
```c
/* Synchronous send + wait-for-reply as a single kernel operation.
 * On success (IPC_OK), the reply message fields are available via
 * wasmos_ipc_last_field(0..7).
 * On failure, returns IPC_ERR_INVALID, IPC_ERR_PERM, or IPC_ERR_FULL. */
extern int32_t wasmos_ipc_call(
    int32_t destination_endpoint,
    int32_t reply_endpoint,
    int32_t type,
    int32_t request_id,
    int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3
) WASMOS_WASM_IMPORT("wasmos", "ipc_call");
```

**Libc wrapper (ipc.h)** — the existing user-space `wasmos_ipc_call` helper
becomes a thin wrapper that calls the hostcall directly.  The retry loop is
removed from the fast path; retries are still available via
`wasmos_ipc_call_retry` for callers that need them.

The ring3 `WASMOS_SYSCALL_IPC_CALL` (syscall 6) implementation gains the
same optimization inside its kernel handler without changing the user ABI.

---

### Direct-Switch Call Sequence

The following describes the fast path.  Steps are performed inside the kernel
hostcall handler for `wasmos_ipc_call` (and the ring3 `IPC_CALL` syscall
handler).

```
1. Acquire ep->lock.

2. Evaluate guards G1–G4 (see above).
   If any guard fails → release ep->lock → execute fallback path → return.

3. Extract receiver = first entry of ep->event.wait_list.

4. Remove receiver from ep->event.wait_list; cancel its sched_event_t
   registration (set receiver->wait_event = NULL, pend_state = SCHED_PEND_OK).

5. Deliver the message directly to receiver's pending-message slot:
   ep->queue[ep->tail] = msg; ep->tail = (ep->tail+1) % IPC_QUEUE_DEPTH; ep->count++
   (equivalent to ipc_enqueue, but receiver will see it immediately on resume).
   Alternatively, copy the message fields directly into receiver's
   process context via a pinned per-thread "direct message" slot — avoids
   the queue entirely and leaves ep->count == 0 throughout.
   (See Open Decisions §1.)

6. Priority inheritance:
   caller->sync_prio_saved = caller->sched_prio  (save before chain entry)
   if receiver->sched_prio > caller->sched_prio:
       inherited_prio = caller->sched_prio        (caller is higher priority)
   else:
       inherited_prio = receiver->sched_prio      (keep receiver's own priority)
   Actually: inherited = min(caller->sched_prio, receiver->sched_prio)
   (lower numeric value = higher priority in SCHED_PRIO_* enum)
   receiver->sync_prio_saved = receiver->sched_prio
   receiver->sched_prio = inherited_prio

7. Save caller's context:
   caller->sync_reply_ep    = reply_endpoint
   caller->sync_chain_depth = cpu_local()->sched.chain_depth + 1
   caller->block_reason     = THREAD_BLOCK_SYNC_CALL
   caller->state            = THREAD_STATE_BLOCKED
   cpu_local()->sched.chain_depth++

8. Set receiver->sync_chain_depth = caller->sync_chain_depth
   (receiver inherits the chain depth so nested calls account correctly)
   receiver->state = THREAD_STATE_RUNNING
   cpu_local()->sched.running = receiver

9. Release ep->lock.

10. context_switch(&caller->ctx, &receiver->ctx)
    (saves caller registers, restores receiver registers, returns in receiver)
```

The receiver resumes inside its `ipc_recv_blocking_for` call, which finds the
message in the queue (or direct slot) and returns `IPC_OK` to the service loop.
The service sees a normal IPC receive — it is unaware that a direct switch
occurred.

---

### Reply Fast Path

When the receiver (or any thread in the chain) calls `ipc_send_from` targeting
an endpoint whose sole blocked waiter has `block_reason == THREAD_BLOCK_SYNC_CALL`
and `waiter->sync_reply_ep == ep->id`, the kernel takes the reply fast path:

```
1. Acquire ep->lock.

2. Check:
   - ep->event.wait_list has exactly one entry (the caller thread)
   - that thread's block_reason == THREAD_BLOCK_SYNC_CALL
   - that thread's sync_reply_ep == ep->id
   - that thread's state == THREAD_STATE_BLOCKED
   If any check fails → normal ipc_send_from delivery → return.

3. Deliver the reply message to the caller's pending slot (same as step 5
   of the call sequence above, or direct slot).

4. Restore priorities:
   current_thread->sched_prio = current_thread->sync_prio_saved
   caller->sched_prio         = caller->sync_prio_saved

5. cpu_local()->sched.chain_depth--

6. Clear caller's sync fields:
   caller->sync_reply_ep    = IPC_ENDPOINT_NONE
   caller->sync_chain_depth = 0
   caller->block_reason     = THREAD_BLOCK_NONE
   caller->state            = THREAD_STATE_RUNNING
   cpu_local()->sched.running = caller

7. Release ep->lock.

8. context_switch(&current_thread->ctx, &caller->ctx)
   (saves current, restores caller — caller returns from wasmos_ipc_call
   with the reply fields populated)
```

After the context switch the caller is running again.  The reply endpoint
retains the message (ep->count == 1 or the direct slot is marked valid), which
the hostcall returns to the WASM caller via `wasmos_ipc_last_field`.

When the current_thread (the service) returns from `ipc_send_from` it finds
itself on the CPU with an empty stack frame.  It loops back into
`wasmos_ipc_recv` and blocks again — this time via the normal scheduler path
(since there is no longer a chain in progress from this thread).

---

### Chain Example: App → FS → FAT

```
App (SCHED_PRIO_WASM = 4) calls wasmos_ipc_call(fs_ep, ...)
  Guard check passes: fs-manager is blocked on fs_ep, queue empty, one waiter.
  App blocked (THREAD_BLOCK_SYNC_CALL, sync_reply_ep = app_ep).
  fs-manager priority raised to 4 (inherits App's prio). [step 6]
  Direct switch → fs-manager.

fs-manager processes, calls wasmos_ipc_call(fat_ep, ...)
  Guard check passes: fat_fs is blocked on fat_ep, queue empty, one waiter.
  chain_depth now = 2.
  fs-manager blocked (THREAD_BLOCK_SYNC_CALL, sync_reply_ep = fs_ep).
  fat_fs priority raised to 4 (inherits chain prio). [step 6]
  Direct switch → fat_fs.

fat_fs reads data, calls ipc_send(fs_ep, reply) → reply fast path.
  fat_fs priority restored to original.
  chain_depth → 1.
  Direct switch back → fs-manager.

fs-manager sends result, calls ipc_send(app_ep, reply) → reply fast path.
  fs-manager priority restored.
  chain_depth → 0.
  Direct switch back → App.

App returns from wasmos_ipc_call with the reply.
```

Total scheduler calls: 0 within the chain.  The App was dispatched by the
normal scheduler before the chain began; it resumes in the scheduler's normal
flow after.

---

### Priority Inheritance Details

Priority uses the `SCHED_PRIO_*` scale where lower numeric value = higher
priority.  Inheritance propagates the **minimum** value (highest priority)
encountered anywhere in the chain.

```c
/* On call entry (step 6): */
uint8_t inherited = (receiver->sched_prio < caller->sched_prio)
                  ? receiver->sched_prio   /* receiver already higher prio */
                  : caller->sched_prio;    /* caller is higher prio */
receiver->sync_prio_saved = receiver->sched_prio;
receiver->sched_prio      = inherited;
```

In a chain A(4) → B(2) → C(1):
- B already at prio 2, A at prio 4: B keeps prio 2 (already higher)
- C at prio 1, B at prio 2: C keeps prio 1 (already higher)

In a chain A(2) → B(4) → C(6):
- B raised from 4 to 2 (inherits A)
- C raised from 6 to 2 (inherits chain minimum)

Priority is restored in reverse order as the chain unwinds (reply fast path
step 4).  If a preemption fires mid-chain and a thread is later re-dispatched,
it resumes with the inherited priority still set — the priority is only restored
by the reply fast path, not by the scheduler.

**Priority leak guard**: if a thread in the chain exits or is killed before
sending a reply, the chain is considered broken.  The process cleanup path
(existing `process_kill` / zombie path) must restore `sync_prio_saved` for any
thread with `sync_chain_depth > 0`.  This prevents leaked inherited priorities
from persisting beyond the broken chain.

---

### Preemption Safety

Direct-switch does **not** disable preemption.  The PIT timer fires normally.
If the timer preempts the currently running service thread mid-chain:

1. The preemption handler saves the service thread's context as usual.
2. The service thread's `sync_chain_depth > 0` and `sync_prio_saved` remain set.
3. The scheduler runs, potentially dispatching another thread.
4. When the service thread is next dispatched (from the ready queue), it resumes
   mid-chain — its `sync_reply_ep` and `sync_chain_depth` are still valid.
5. The reply fast path fires when the service eventually calls `ipc_send` to the
   reply endpoint.

The chain does not require uninterrupted execution; it merely eliminates the
scheduler calls when the conditions allow.  A preempted mid-chain thread is
re-enqueued at its inherited priority (which is at least as high as the original
caller's), so it tends to be rescheduled promptly.

---

### SMP Safety

With a single global ready queue, the correctness argument for SMP is
straightforward: G3 (`receiver->state == THREAD_STATE_BLOCKED`) is the decisive
check.  A BLOCKED thread is not in the ready queue and is not executing on any
CPU.  Removing it from `ep->event.wait_list` and switching to it on the current
CPU is safe as long as the removal is done under `ep->lock`.

The one SMP race that requires care is the narrow `RUNNING→BLOCKED` window: a
thread may have been preempted and its context save may still be completing on
another CPU when the sender evaluates G3.  The existing `blocking_transition`
flag (an atomic byte on `thread_t`, set at the start of `RUNNING→BLOCKED` and
cleared after context save completes) guards this window.  The direct-switch path
spins on `blocking_transition` before proceeding, exactly as `sched_wake_thread`
already does.

```c
/* After guard evaluation, before removing receiver from wait_list: */
while (__atomic_load_n(&receiver->blocking_transition, __ATOMIC_ACQUIRE))
    cpu_relax();
/* Safe: context save is complete on the remote CPU. */
```

**Future: per-CPU queues.** When per-CPU queues arrive, a BLOCKED thread becomes
associated with a specific CPU's scheduling context after work-stealing.  At that
point the SMP analysis must account for queue migrations, and an optional
same-CPU guard (G6) may become relevant to avoid moving a thread whose working
set is remote.  That is a future concern; the current single-queue design
requires only the `blocking_transition` spin.

---

### Fallback Path

When any guard fails, the kernel falls back to the existing `ipc_send_from` +
`sched_wake_thread` path.  The caller-CPU bias (`receiver->last_cpu =
cpu_local()->cpu_id`) is set on the receiver before enqueuing, as it already is
in the current scheduler.

No observable behavior change occurs for callers.  From the caller's perspective
`wasmos_ipc_call` either returns promptly (fast path) or blocks in the normal
scheduler (fallback).  The reply is delivered identically in both cases.

Conditions that commonly trigger the fallback:
- Service is busy (running on another CPU or processing a prior request): G3 fails.
- Endpoint has backlogged messages (high-throughput producers): G1 fails.
- Chain depth at limit (deeply recursive service calls): G4 fails.

---

### Lock Ordering

The direct-switch path acquires `ep->lock` and modifies `caller->state` and
`receiver->state` while holding it.  This fits into the existing lock hierarchy
(see `docs/architecture/09-process-and-ipc.md`) between the `sched_event_t.lock`
(which is embedded in `ep->event` and acquired via `sched_event_wait`) and the
`ipc_endpoint_t.lock`.

New ordering constraint: `cpu_sched_t.lock` must NOT be held when
`ipc_send_from` is called with potential direct-switch.  The switch path
modifies `cpu_local()->sched.running` and `chain_depth` while holding only
`ep->lock` — acquiring `cpu_sched_t.lock` afterward would invert the existing
order.  The existing `cpu_sched_enqueue` path (fallback) already respects this.

```
Existing hierarchy (outermost → innermost):
  cpu_sched_t.lock
    sched_event_t.lock
      thread_t.s_lock
        ipc_endpoint_t.lock
          futex_table_bucket.lock

Direct-switch path acquires:
  ipc_endpoint_t.lock  (step 1)
  → modifies cpu_local()->sched fields WITHOUT acquiring cpu_sched_t.lock
    (safe: modifications are to the LOCAL CPU's fields, single writer)
```

---

### libsys and libc Migration

Existing services (receivers) require **no changes**.  They loop on
`wasmos_ipc_recv`, process requests, and call `wasmos_ipc_send` for replies.
The direct-switch is transparent.

Callers benefit by switching from the user-space helper to the hostcall:

**Before (user-space helper — two separate hostcalls):**
```c
wasmos_ipc_call(fs_ep, g_ep, FS_IPC_READ_REQ, req_id, path_len, 0, 0, 0, &reply);
```

**After (kernel primitive — single hostcall, direct-switch eligible):**
```c
/* api.h wasmos_ipc_call is now a real hostcall; libc ipc.h wrapper calls it */
wasmos_ipc_call(fs_ep, g_ep, FS_IPC_READ_REQ, req_id, path_len, 0, 0, 0, &reply);
```

The API is identical.  The libc wrapper `wasmos_ipc_call` in `ipc.h` is updated
to call the new hostcall instead of the send+recv loop.  Callers that already
use `wasmos_ipc_call` automatically gain the optimization; no source changes are
required at the call site.

`wasmos_ipc_call_retry` and `wasmos_ipc_call_managed` keep their current retry
loop semantics for callers that require retry-on-full behavior; they call the new
hostcall per attempt.

**Native kernel-mode services** (`font-service`, `gfx-compositor`, and other
`wasmos_driver_api_t`-based callers) use `wasmos_sys_ipc_call_native` /
`sys.ipcCall` as the sync-call primitive.  Phase 3 wires the `api->ipc_call`
slot in `wasmos_driver_api_t` and updates `wasmos_sys_ipc_call_native` to call
it, replacing the current `api->ipc_send` + `wasmos_sys_ipc_recv_matching_native`
pair.  Services that currently bypass `wasmos_sys_ipc_call_native` with local
spin-poll wrappers migrate to `sys.ipcCall` as part of Phase 3.

---

### Rollout Plan

#### Phase 0: Infrastructure (no behavior change)

- Add `sync_reply_ep`, `sync_chain_depth`, `sync_prio_saved` to `thread_t`.
- Add `chain_depth` to `cpu_sched_t`.
- Add `THREAD_BLOCK_SYNC_CALL` to `thread_block_reason_t`.
- Add `IPC_DIRECT_SWITCH_MAX_DEPTH` constant.
- Add observability: log `[ipc-direct] chain depth=%u` on first fast-path hit per
  CPU (once guard, not every call).

Done gate: `run-qemu-test` passes; new fields zero-initialized, no behavior
change.

#### Phase 1: Ring3 `IPC_CALL` fast path

- Implement guard check + direct-switch in the `WASMOS_SYSCALL_IPC_CALL` handler.
- Implement reply fast path in `ipc_send_from` for `THREAD_BLOCK_SYNC_CALL`
  waiters.
- Implement priority inheritance and restore.
- Implement priority leak guard in `process_kill` / zombie cleanup.

Done gate: ring3 smoke tests pass; `run-qemu-test` and `run-qemu-ring3-test`
pass; chain-depth telemetry marker fires at least once under `run-qemu-test`.

#### Phase 2: WASM `"wasmos"."ipc_call"` hostcall

- Register `wasmos_ipc_call_hc` in `wasm3_link.c`.
- Implement hostcall: same guard + direct-switch logic as Phase 1.
- Update `api.h` declaration.
- Update `ipc.h` wrapper to call the hostcall instead of the send+recv loop.

Done gate: `run-qemu-test` passes; existing `wasmos_ipc_call` call sites in
services and apps gain the optimization without source changes.

#### Phase 3: Native driver API (`wasmos_driver_api_t`)

- Add `ipc_call` function pointer to `wasmos_driver_api_t`; populate it in
  `native_driver_start` with the same kernel fast-path function used by Phase 1.
- Update `wasmos_sys_ipc_call_native` in `libsys_native.c` to call `api->ipc_call`
  instead of `api->ipc_send` + `wasmos_sys_ipc_recv_matching_native`.
- Migrate `font-service` and `gfx-compositor` from their local `intentSendWithRequestId`
  spin-poll wrappers to `sys.ipcCall` (`wasmos_sys_ipc_call_native`).

Done gate: `run-qemu-test` passes; `font-service` and `gfx-compositor` hot call
paths exercise the fast path; no boot regressions.

#### Phase 4: High-frequency call-site audit

- Identify call sites in `libsys`, `fs-manager`, `device-manager`, `gfx-compositor`,
  `font-service`, and `cli` that issue synchronous calls in hot paths.
- Verify each site uses the appropriate primitive (`wasmos_ipc_call` for WASM,
  `wasmos_sys_ipc_call_native` for native); migrate any remaining raw send+recv.
- Add per-call-site chain-depth telemetry markers for at least one round-trip
  call path in each service (temporary, removable after validation).

Done gate: all identified hot paths exercise the fast path at least once during
`run-qemu-test`; no boot or interactive CLI regressions.

#### Phase 5: Hardening and stress testing

- Add kernel selftest: spawn two processes, one calls the other in a tight loop
  via `wasmos_ipc_call`; verify chain-depth telemetry and that the fast path is
  taken > 90% of the time.
- Add negative test: verify that a call that exceeds `IPC_DIRECT_SWITCH_MAX_DEPTH`
  falls back without faulting.
- Add negative test: kill a service mid-chain; verify the caller unblocks cleanly
  via the normal event wake path.
- Remove temporary telemetry markers from Phase 4.

Done gate: all selftests pass under `run-qemu-test`; no regressions across
`run-qemu-test` and `run-qemu-cli-test`.

---

### Validation Matrix

| Scenario | Expected result |
|---|---|
| Service idle, caller makes `wasmos_ipc_call` | Fast path taken; chain-depth marker fires |
| Service busy (running on another CPU) | G3 fails; fallback path; caller-CPU bias enqueues receiver |
| Endpoint has queued messages | G1 fails; fallback path |
| Chain depth reaches `IPC_DIRECT_SWITCH_MAX_DEPTH` | G6 fails; fallback path; no fault |
| Three-hop chain (app → fs → fat) | Two direct switches; zero scheduler calls within chain |
| Timer preempts mid-chain service | Chain resumes on next dispatch; reply fast path still fires |
| Service killed mid-chain | Caller unblocks via `sched_event_wake_all`; `sync_prio_saved` restored |
| Reply sent to wrong endpoint | Guard check fails; no direct switch; message queued normally |
| `run-qemu-test` baseline | All existing boot/halt markers pass |
| `run-qemu-ring3-test` baseline | All ring3 isolation markers pass |
| Threading IPC stress selftest | Completes 32 exchanges; no chain-state corruption |

---

### Open Decisions

1. **Message delivery mechanism**: step 5 of the call sequence describes two
   options — (a) enqueue into `ep->queue` as normal, or (b) write directly to a
   per-thread "direct message slot" and leave `ep->count == 0`.  Option (a) is
   simpler (no new slot, receiver uses existing queue read path) but briefly
   raises `ep->count` to 1 while the receiver is running.  Option (b) keeps
   `ep->count == 0` throughout, making the guard cleaner for nested calls, but
   requires a new field and a modified receive path.  Decide before Phase 1
   implementation.

2. **Cross-CPU fast path (Phase 2 extension)**: relax G5 to allow direct switch
   to a receiver blocked on a different CPU.  The receiver would be "pulled" to
   the calling CPU (migrated) as part of the switch.  This requires updating
   `receiver->last_cpu` and a TLB/CR3 consideration if the receiver last ran on a
   different CPU's scheduler context.  Deferred until Phase 1 is stable.

3. **`wasmos_ipc_reply` dedicated hostcall**: rather than detecting reply
   direction from the `THREAD_BLOCK_SYNC_CALL` waiter check in `ipc_send_from`,
   add an explicit `wasmos_ipc_reply(reply_ep, type, req_id, a0..3)` hostcall that
   asserts the reply-fast-path intent.  This would simplify the guard (no
   per-send check overhead) and make the service contract explicit.  Trade-off:
   services would need a source change to use `wasmos_ipc_reply` instead of
   `wasmos_ipc_send`.  Evaluate after Phase 2.

4. **Chain-depth telemetry**: the Phase 0 "first hit" log covers basic
   observability.  A future `ps`-style extension could expose per-process
   `sync_call_count` and `direct_switch_hits` counters in the process-manager's
   status output.

---

### Task Checklist (Execution Order)

1. Add `sync_reply_ep`, `sync_chain_depth`, `sync_prio_saved` to `thread_t`; add
   `chain_depth` to `cpu_sched_t`; add `THREAD_BLOCK_SYNC_CALL`.
2. Add guard evaluation helper `ipc_direct_switch_eligible(ep, receiver)`.
3. Implement direct-switch call path in `WASMOS_SYSCALL_IPC_CALL` handler.
4. Implement reply fast path in `ipc_send_from`.
5. Implement priority inheritance and restore in both paths.
6. Add priority leak guard to `process_kill` / zombie cleanup.
7. Register `"wasmos"."ipc_call"` hostcall; implement in `wasm3_link.c`.
8. Update `api.h` declaration; update `ipc.h` wrapper.
9. Audit hot-path call sites in services; migrate to `wasmos_ipc_call`.
10. Add selftests (fast-path hit, depth overflow, mid-chain kill).
