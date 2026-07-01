## Claude

### What signals would solve:

1. Graceful shutdown — `process_kill` is hard termination. There's no way to ask a WASM service to flush, close file
   handles, or acknowledge it's stopping. `SIGTERM`-style is the canonical missing piece.
2. Async child-death notification — `process_wait` blocks synchronously. If the process manager or a supervisor wants to
   be told when a child dies without sitting in a wait loop, it needs something like `SIGCHLD`.
3. Hardware exception delivery to the host — If a WASM module triggers a trap (bad memory, divide-by-zero), the runtime
   currently deals with it internally. A signal could let the WASM host process decide how to respond (restart the
   module, log and die, etc.) rather than having that policy baked into the runtime.

### Why you probably don't need classic POSIX signals:

The IPC system already covers most of this. `IPC_ENDPOINT_TYPE_NOTIFICATION` + `ipc_select_wait` is essentially a
signal-like async delivery mechanism for anything event-driven. `SIGUSR1`/`SIGUSR2` patterns are fully replaceable by a
message on a well-known endpoint. Full POSIX signals also bring serious complexity: signal trampolines, stack switching,
async-signal-safe restrictions, masking/blocking — none of which fits the project's minimalism goal, and interrupting an
in-progress wasm3/WARP execution at a safe point is particularly tricky.

### The real gap

A narrow lifecycle protocol — a standard way for the process manager to request graceful shutdown and receive an ack —
is the only truly missing primitive. Everything else (async notification, child exit, inter-process events) can be
expressed cleanly with IPC. This could be as simple as a reserved `IPC_MSG_SHUTDOWN` message type that PM sends to a
process's main endpoint, with an optional timeout before `process_kill` fires.

Recommendation: don't add a signal subsystem; instead, define a small lifecycle convention over IPC (shutdown request +
ack + timeout-then-kill). It solves the real gap without the complexity cost.

---

### Lifecycle Convention over IPC

The core idea: everything is built on the existing IPC primitives (`ipc_message_t`, endpoints, notification channels)
and routed through the PM, which is already the lifecycle authority. No new kernel primitives. No signal trampolines.
Two concrete capabilities:

1. Graceful Shutdown (`SIGTERM` equivalent)

The gap: `process_kill` is hard termination. A WASM service with open shared-memory borrows, a framebuffer lock, or an
in-progress FS transfer has no way to clean up.

### The mechanism

A process optionally registers a shutdown endpoint with PM at startup via a new PM message type:

```
PROC_IPC_REGISTER_SHUTDOWN_EP // arg0=endpoint, arg1=timeout_ticks
```

PM stores this in `pm_app_state_t` (two new fields: `shutdown_endpoint`, `shutdown_timeout_ticks`). When a caller
requests graceful kill:

```
PROC_IPC_KILL_GRACEFUL // arg0=target_pid
```

PM's event loop follows this path:

```
if app.shutdown_endpoint != IPC_ENDPOINT_NONE:
    ipc_send → PROC_IPC_SHUTDOWN_REQ to app.shutdown_endpoint
    record pending graceful kill: (pid, deadline_tick)
    wait for PROC_IPC_SHUTDOWN_ACK from target, or tick deadline
    on ack OR timeout → process_kill(pid, EXITCODE_SHUTDOWN)
else:
    process_kill(pid, exit_status) immediately ← current behavior unchanged
```

The PM event loop already runs `pm_check_waits` and `pm_poll_spawn` per tick — a `pm_check_graceful_kills` pass fits
naturally alongside them. No new scheduling mechanism is needed.

WASM runtime side: The `warp` driver / `wasm3` host registers a shutdown endpoint during its init, saves it, and on
receiving `PROC_IPC_SHUTDOWN_REQ` sets a `volatile uint8_t shutdown_requested` flag in the runtime context. Both `WARP`
and `wasm3` hit hostcall boundaries regularly — that's the natural safe point to check the flag and unwind the WASM
instance before calling the `exit` syscall. This avoids interrupting mid-instruction execution in the interpreter.

2. Async Death Watch (`SIGCHLD` equivalent)

The gap: `process_wait` blocks synchronously. A supervisor or session manager that spawns multiple services can't be
notified when any one of them exits without either polling or dedicating a thread per child.

The mechanism:

A process registers interest in another PID's death via:

```
PROC_IPC_WATCH_PID // arg0=target_pid, arg1=notify_endpoint
PROC_IPC_UNWATCH_PID // arg0=target_pid
```

PM maintains a small watch table (similar to the existing `pm_wait_state_t` list). When a watched process is reaped (
`pm_reap_apps` already fires on exit), PM sends:

```
PROC_IPC_DEATH_NOTIFY // arg0=target_pid, arg1=exit_status
```

to each registered `notify_endpoint`. The watcher is blocked in `ipc_select_wait` watching its own set of endpoints — it
wakes up immediately, reads the notification, and calls `process_get_exit_status` to confirm the code.

The watcher can mix death-watch endpoints into the same `ipc_select_wait` call it already uses for its regular IPC
work — zero extra threads, zero polling.

3. Exit status conventions

To let a watcher distinguish how a process died without adding new fields, encode it in the exit status:

```
#define EXITCODE_SHUTDOWN    (-1)   // graceful shutdown completed
#define EXITCODE_TIMEOUT     (-2)   // shutdown requested, ack never came, hard-killed
#define EXITCODE_WASM_TRAP   (-3)   // WASM runtime trapped (invalid mem, stack overflow, etc.)
#define EXITCODE_CRASH       (-4)   // kernel-detected fault (page fault, GPF)
```

Positive values remain free for application-defined exit codes. `PROC_IPC_DEATH_NOTIFY`'s `arg1` carries one of these,
so the watcher can branch without a follow-up call.

### What this does NOT add

- No trampoline, no signal mask, no async-signal-safe discipline
- No interruption of a running thread mid-instruction — shutdown is checked at safe points only
- No new scheduler state — graceful kill tracking is a PM-side table checked per tick
- No changes to processes that don't opt in — they behave exactly as today

### Sketch of the new constants

These would live alongside the existing PM IPC message types (wherever `PROC_IPC_SPAWN`, `PROC_IPC_KILL`, etc. are
currently defined):

```
#define PROC_IPC_REGISTER_SHUTDOWN_EP 0x40 // process → PM: register shutdown endpoint
#define PROC_IPC_KILL_GRACEFUL 0x41 // caller → PM: request graceful stop
#define PROC_IPC_SHUTDOWN_REQ 0x42 // PM → process: please exit
#define PROC_IPC_SHUTDOWN_ACK 0x43 // process → PM: acknowledged, exiting
#define PROC_IPC_WATCH_PID 0x44 // caller → PM: notify me when pid dies
#define PROC_IPC_UNWATCH_PID 0x45 // caller → PM: cancel watch
#define PROC_IPC_DEATH_NOTIFY 0x46 // PM → watcher: pid has exited
```

Seven new message types, two new fields in `pm_app_state_t`, one new small table in `pm_state_t`, and a check pass in
PM's tick loop — the entire feature fits inside what's already there.

## Codex

A signaling system could make sense, but not a Unix-style POSIX signal layer as the first step.

WASMOS already has most of the synchronous lifecycle controls:
`docs/architecture/09-process-and-ipc.md` shows `wait`, `kill`, `status`, IPC endpoints, notification endpoints, and
scheduler-backed blocking/wake behavior. In particular, notification IPC already exists as a lightweight event
primitive, and process ownership checks are enforced by the process manager. So the question is not “do we need async
process events at all?”, but “is there a missing class of async control that IPC + `wait`/`kill` does not cover well?”

### Why you might want signaling

- Asynchronous cancellation or interruption. A blocked or long-running process may need “stop what you’re doing soon”
  without forcing an immediate kill.
- Parent/dependency lifecycle events. If a service dies, dependents may need a structured shutdown/reload notification
  instead of discovering failure indirectly.
- Job-control style behavior later. `SIGSTOP`/`SIGCONT` equivalents become relevant if the CLI grows real background job
  control.
- Exceptional delivery. Timers, watchdog expiry, terminal hangup, or child-death notifications are naturally async.

### Why you probably do not want full Unix signals yet

- They are hard to define cleanly with your current model. Threads block on `sched_event_wait`, IPC, futexes, and joins;
  “deliver at any point” semantics would complicate the scheduler and thread state machine in
  `docs/architecture/08-threading-and-lifecycle.md`.
- WASM runtimes make async upcalls awkward. An out-of-band handler that can interrupt arbitrary user code is much less
  natural than in a native Unix process.
- Signals are historically messy: mask rules, per-thread vs per-process delivery, restartable syscalls, default
  dispositions, zombies, reentrancy hazards.

### Pragmatic recommendation

- Yes, add a signaling concept only if you keep it explicit and minimal.
- Start with a kernel/PM-supported control-event mailbox, not POSIX signals.
- Make delivery cooperative: pending bits or queued control events checked at safe points like syscall return, IPC wait
  wakeup, yield, or event-loop boundaries.
- Initial event set should be small: `terminate`, `cancel`, `child-exited`, maybe `reload` or `hangup`.
- Keep `kill` as the non-cooperative hard-stop path.

So: a signaling system is useful if you need structured asynchronous control. You do not need POSIX signals yet. In this
design, a small PM-mediated control-event mechanism built on existing notification/IPC semantics would fit the
architecture much better than importing Unix signal semantics wholesale.

---

### Core Idea

Add a per-process control channel owned by the kernel/PM. Other authorized actors do not “interrupt the thread”; they
enqueue a control event for the target process. The target observes and handles those events when it reaches defined
safe points:

- syscall return
- yield
- wake from IPC/futex/join/wait
- event-loop helpers in `libsys`
- optional explicit `poll_control()` / `control_wait()`

That fits your current model because `docs/architecture/09-process-and-ipc.md` already uses explicit IPC and scheduler
events, and `docs/architecture/08-threading-and-lifecycle.md` already has clear blocked/running transitions. You avoid
“run a handler on an arbitrary stack frame” entirely.

### Why This Shape

The main design constraint is WASM. Async handler injection into arbitrary user code is a bad fit:

- `wasm3`/`WARP` execution is entered through controlled runtime calls
- threads already block through kernel-managed wait paths
- reentrancy and interrupted hostcalls would get complicated fast

So instead of “deliver now, anywhere”, use “mark pending, deliver soon at boundary X”.

### Suggested Kernel Objects

Per process:

- `pending_control_bits`
- optional small fixed-size control-event queue
- `sched_event_t control_event` for blocking waits
- policy metadata for who may send which events

I would use both bits and a queue:

- bits for coalescing level-triggered events like `TERMINATE`, `CANCEL`, `CHILD_EXITED`
- queue for payload-bearing events if you later need `pid`, `exit status`, `timer id`, `tty id`

A minimal struct could look like:

```
typedef enum {
PROC_CTRL_TERMINATE = 1,
PROC_CTRL_CANCEL = 2,
PROC_CTRL_CHILD_EXITED = 3,
PROC_CTRL_RELOAD = 4,
PROC_CTRL_HANGUP = 5,
} proc_ctrl_type_t;

typedef struct {
uint32_t type;
uint32_t arg0;
uint32_t arg1;
} proc_ctrl_event_t;
```

### Delivery Semantics

Use process-scoped pending delivery, not thread-scoped delivery, at least initially.

Reason:

- your current ownership and lifecycle model is process-centric
- most immediate needs are service/app lifecycle controls
- per-thread semantics create hard questions: which thread gets it, what if it is blocked, what if two threads are in
  the runtime

Then pick one receiving rule:

- “main thread receives control events” for user-visible handling
- “any thread may observe process-pending `terminate`/`cancel` at safe points” for cooperative cancellation

I’d split them:

- termination/cancel are process-wide flags visible to all threads
- consumable events like `CHILD_EXITED` are read by one designated consumer, usually main thread or event loop thread

### Safe Points

A process checks pending control at:

- before returning to `ring3` from a syscall
- immediately after `sched_event_wait` returns
- before entering userspace after IPC/futex/join/wait unblock
- before and after WASM hostcall dispatch
- in `libsys` blocking helpers that already multiplex IPC/select

This matters because it gives bounded latency without arbitrary interruption.

Example:

1. Process blocks in `ipc_recv_blocking_for`
2. PM sends `TERMINATE`
3. Kernel marks `PROC_CTRL_TERMINATE` pending and wakes one blocked thread via `control_event` or the existing wait path
4. Thread resumes in kernel, sees pending terminate before returning to user
5. Kernel either:
    - forces exit immediately for hard terminate, or
    - returns an interrupted status so userland can unwind for cooperative cancel

### Event Classes

I would define two classes.

1. Advisory/cooperative:

- `CANCEL`
- `RELOAD`
- `HANGUP`
- `CHILD_EXITED`

These are observable by userland. They should not kill the process by themselves.

2. Mandatory:

- `TERMINATE`

This can still be cooperative-first:

- mark pending terminate
- wake blocked threads
- give process a short chance to exit cleanly if it handles it
- if policy/user requested hard kill, call existing `process_kill`

That preserves your current kill behavior as the final enforcement tool.

### User API

Do not copy `signal()`/`sigaction()` first. Start with explicit APIs.

Kernel/PM side:

- `proc_control_send(pid, type, arg0, arg1)`
- policy check: owner or privileged sender only

User side:

- `proc_control_poll(out_event)` nonblocking
- `proc_control_wait(out_event, timeout_ms)` blocking
- `proc_control_ack(type)` only if you need explicit ack semantics
- `proc_cancel_requested()` cheap bit check helper

For libc/libsys event loops:

- integrate control handling into the existing select/wait helpers so apps/services do not need a separate polling loop

That is the key usability point: if every service already blocks on IPC/select, the control path should surface through
the same loop.

### Interaction With Existing IPC

I would not model this as ordinary user-owned IPC endpoints.

Reasons:

- control is lifecycle-sensitive and should work even if the process is misbehaving
- ownership/policy is kernel-level, not just endpoint routing
- termination/cancel should wake blocked waits even if userland never created the right endpoint

But the semantics can resemble IPC:

- queued records
- wakeups via `sched_event_t`
- optional integration with select/event loops

So architecturally it is “kernel-managed control IPC”, not regular service IPC.

### Permissions and Policy

Reuse the process-manager ownership model already described in `docs/architecture/09-process-and-ipc.md`:

- parent/owner can send `CANCEL`
- owner or privileged process can send `TERMINATE`
- kernel/PM generates `CHILD_EXITED`
- terminal/session owner later can send `HANGUP`
- admin/system capability can send `RELOAD` or global control events

This avoids the Unix problem where signal permissions become ad hoc.

### Blocked Syscalls and Interruptibility

You need one policy decision: what happens if a thread is blocked?

Recommended behavior:

- wake interruptible waits for `CANCEL` and `TERMINATE`
- return a distinct status like `ERR_INTERRUPTED`
- leave non-interruptible low-level waits extremely rare and explicit

That means:

- IPC receive
- futex wait
- process wait
- thread join should all be able to resume with “interrupted by control event”.

This is much cleaner than inventing async handler preemption.

### Threading Implications

For multithreaded processes:

- `TERMINATE` is process-wide
- `CANCEL` is process-wide by default
- one thread draining `CHILD_EXITED` queue is enough
- if one thread is blocked and another is running, both can observe pending terminate at their next safe point

Do not add per-thread control delivery until there is a concrete use case.

### Default Userland Pattern

A service loop would look like:

1. block in existing `libsys` wait/select helper
2. helper wakes for either IPC or control
3. if control event:
    - `TERMINATE`: shutdown and exit
    - `RELOAD`: re-read config/state
    - `CHILD_EXITED`: reap/update internal state
4. otherwise process normal IPC

That gives you most benefits people want from signals without hidden control flow.

### What Problems This Solves

It gives you:

- graceful shutdown
- cooperative cancellation
- parent/child lifecycle notifications
- later session/TTY hangup
- future timer/watchdog delivery
- event-loop integration

Without:

- async handler reentrancy
- per-thread signal masks
- restart-syscall semantics
- “which frame was interrupted?” ambiguity

### What I Would Not Add Yet

Avoid these initially:

- signal numbers modeled after POSIX
- arbitrary user-installed async handlers
- per-thread masks
- default-stop/default-core semantics
- nested handler frames
- synchronous fault-to-signal translation like `SIGSEGV`

Faults should remain explicit exceptions/panics until you have a stronger userspace ABI story.

### Minimal Rollout

I’d stage it like this:

1. Kernel: add pending terminate/cancel bits and wake blocked waits.
2. PM: add `PROC_IPC_CONTROL_SEND` with owner/policy checks.
3. Syscalls/`libsys`: add `control_poll` and surface interrupted waits.
4. Userland: teach core services to honor `TERMINATE`.
5. Later: add `CHILD_EXITED`, `RELOAD`, `HANGUP`.

## Grok

Short answer: a Unix-style signal system is not a good fit for WASMOS right now. Most of what people use signals for is
already covered—or better served—by IPC, lifecycle syscalls, and cooperative exit. What you might want later is a 6:54
AM small, explicit control channel, not POSIX signals.

### What you have instead of signals

WASMOS already has several mechanisms that overlap with “signaling,” but none of them interrupt arbitrary running code:

| Need                  | Today’s primitive                                            |
|-----------------------|--------------------------------------------------------------|
| Wake a blocked thread | IPC message/notification endpoints + `sched_event`           |
| Wait for child exit   | `WAIT` syscall / `PROC_IPC_WAIT` -> `process_wake_waiters`   |
| Force termination     | `process_kill` / `PROC_IPC_KILL` (immediate, no handler)     |
| Hardware/async events | IRQ routed to IPC endpoints                                  |
| In-process sync       | Futex (`wasmos_futex_wait` / `wake`)                         |
| User faults           | Kernel terminates the faulting process (`exit_status = -11`) |

Fault handling is deliberately blunt—user-mode faults do not get delivered as recoverable signals:

```
process_set_exit_status(proc, -11);
process_yield(PROCESS_RUN_EXITED);
return 0;
```

`process_kill` is the same model: mark exited, wake waiters, no in-process handler:

```
int process_kill(uint32_t pid, int32_t exit_status) {
    ...
    process_mark_exited(target, exit_status);
    process_try_auto_reap(target);
    return 0;
}
```

POSIX signal semantics are explicitly out of scope:

- **POSIX compatibility.** The libc shims provide just enough for WASM apps to compile; they are not a complete POSIX
  implementation. System call numbers, signal semantics, and POSIX process model details are not faithfully reproduced.

### What Unix signals actually provide

Signals are asynchronous interrupts to running threads: deliver mid-execution, optionally run a handler, optionally
resume, mask/defer, and encode rich semantics (`SIGTERM` vs `SIGKILL`, `SIGCHLD`, `SIGALRM`, etc.).

That model assumes:

• Native code with a stable register frame you can hijack
• Handler functions that can run safely in arbitrary contexts
• `siglongjmp` recovery from faults

WASMOS is WASM-first, microkernel-shaped, and event-loop oriented. Those assumptions do not hold cleanly.

### Why you might think you need signals

These are the real motivations, and whether they justify a signal system:

1. Graceful shutdown (`SIGTERM`-like): A supervisor wants a service to drain work before exit. Useful, but better as an
   explicit IPC control message on a known endpoint (“shutdown requested, deadline N ms”) than as a signal that preempts
   WASM mid-instruction.
2. Cancel long-running work (`SIGINT`-like): Useful, but in WASM you want cooperative cancellation: check a flag at
   yield/IPC/hostcall boundaries, not inject a handler while `wasm3_lock` is held.
3. Child exit notification (`SIGCHLD`-like): Already covered by `wait` + `sched_event_wake_all` when a child becomes
   zombie. No signal layer needed.
4. Timer / alarm (`SIGALRM`-like): Useful, but a timer service posting to an IPC notification endpoint is simpler and
   matches the architecture (“notifications distinct from request/reply”).
5. Fault recovery (`SIGSEGV` / `SIGFPE`): Poor fit for WASMOS. Goals say user faults terminate the process. WASM
   sandboxing + capability kill-on-violation already enforce isolation. Recoverable fault handlers add huge complexity
   for little gain in a research microkernel.
6. POSIX / libc portability: Signals are a large surface (`sigaction`, masks, `SA_RESTART`, async-signal-safe functions,
   etc.). Goals say this is a non-goal unless you deliberately pivot toward POSIX compat.
7. Debugging (`SIGTRAP` / `SIGSTOP`): Maybe later, but that’s a dedicated debug protocol, not general process signals.

### Why a full signal system is a poor fit here

1. WASM execution is not safely preemptible at arbitrary points. The kernel holds `wasm3_lock` across interpreter entry;
   interrupting mid-WASM requires defining a whole new ABI (where to save linear memory state, hostcall invariants,
   etc.).
2. Microkernel style favors explicit contracts. “Send `PROC_CONTROL_SHUTDOWN` to endpoint X” is auditable. “Raise signal
   15” is implicit and harder to capability-gate.
3. You already have the blocking-side story. IPC notifications + select/poll wake blocked threads efficiently. Signals
   mainly add value when code is busy-spinning without yielding—and WASMOS services are expected to run event loops, not
   spin.
4. Minimalism test fails. Per architectural direction: if it can be built from existing primitives in user space, it
   belongs there. Most signal semantics can be.

### What would make sense instead

If you need “signaling,” a lighter design aligned with WASMOS:

Process control IPC — At spawn, grant each process a control endpoint (or bind `proc.control` in metadata). Parents, PM,
or a supervisor send typed messages:

- `SHUTDOWN` (cooperative; process exits on next event-loop turn)
- `CANCEL` (abort current operation; return error from blocking call)
- `PING` / health check

Cooperative cancellation flag — Kernel sets `proc->cancel_requested`; checked at syscall/hostcall/yield entry. No
handler, no stack hijack.

Keep kill as the hard stop — `process_kill` remains `SIGKILL`-like: no negotiation, immediate zombie transition.

Keep faults as fatal — Unless you add a full debugger, don’t try `siglongjmp`-style recovery.

That gives you 80% of the operational value (supervision, graceful teardown, cancellation) without POSIX signal
complexity.

### When a real signal system would make sense

Consider it only if you have a concrete requirement that cooperative IPC cannot satisfy, for example:

- Running native ring-3 ELF with POSIX libc expecting real `sigaction`
- A debugger that must stop threads mid-execution
- Job control (terminal foreground/background groups)
- Recoverable native fault handling outside WASM

None of those are current WASMOS goals.

---

Bottom line: You do not need a Unix signal system for WASMOS’s current model. You need explicit, capability-gated
control messages for shutdown/cancel, which IPC notifications and the process manager already sit close to. Reserve true
asynchronous signals for a future native-userland or debugging track—not for the WASM microkernel path.

If you want to go further, we could sketch a minimal `proc.control` IPC contract (opcodes, capability rules, cooperative
vs. forced kill) without touching the kernel much.
