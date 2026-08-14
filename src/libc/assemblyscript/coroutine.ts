/* coroutine.ts - cooperative stackless coroutines for AssemblyScript guests.
 *
 * Zig, Rust and Go bind src/libsys/wasm/coroutine_wasm.c by compiling that C
 * file into their module; `asc` has no external linking, so AssemblyScript
 * carries this port instead. coroutine_wasm.c is the reference: the semantics
 * here must match it, and the shared behaviour tests mirror its test suite.
 *
 * Stackless, as in the C original. A task never has its call stack saved; its
 * resume method records its own progress in fields it owns, returns YIELDED,
 * and is called again by the runtime. That is what makes it work on a WASM
 * guest at all -- neither engine can swap a guest stack.
 *
 * Ownership: apps are built with `--runtime stub`, a bump allocator with no
 * collector, so every allocation is permanent. Callers own and REUSE their
 * Coroutine/Future/Continuation objects, exactly as the C API takes
 * caller-provided structs; allocating a Future per IPC round-trip in a
 * long-lived driver leaks until the process exits. The runtime itself still
 * allocates small helpers -- a Box per resume and per continuation dispatch,
 * two callback objects per group source, and a FutureGroup per race()/all()
 * call. A long-lived loop uses raceInto/allInto with caller-owned group storage.
 */

/** Terminal and pending states of a future. */
export enum FutureState {
    Pending = 0,
    Ready = 1,
    Failed = 2,
}

/**
 * Lifecycle of a coroutine: New before asyncStart, Ready while queued, Running
 * inside its resume, Waiting while parked on a future, Dead once its resume
 * returned anything other than TASK_YIELDED.
 */
export enum CoroutineState {
    New = 0,
    Ready = 1,
    Running = 2,
    Waiting = 3,
    Dead = 4,
}

/** A task resume returns COMPLETE (with its value boxed) or YIELDED. */
export const TASK_COMPLETE: i32 = 0;
export const TASK_YIELDED: i32 = 1;
/** future.await returns this after parking; the task must return YIELDED. */
export const AWAIT_PENDING: i32 = 1;
/** A success callback may return this with the next future boxed in `out`. */
export const FUTURE_CHAIN_NEXT: i32 = 2;

/**
 * Out-parameter carrier. The C API passes `uintptr_t*`; AS has no pointer to a
 * local, so callers pass a Box they own and reuse.
 */
export class Box {
    value: usize = 0;
}

/**
 * A stackless task body. The subclass instance carries what C passed as the
 * `void* user` argument, so there is no separate user pointer.
 */
export abstract class Task {
    abstract resume(out: Box): i32;
}

/** Continuation callbacks. Same substitution: the instance is the user data. */
export abstract class OnSuccess {
    abstract call(value: usize, out: Box): i32;
}

export abstract class OnError {
    abstract call(status: i32, out: Box): i32;
}

/**
 * Settlement rule of a FutureGroup: Race settles on the first input to settle
 * either way, All on the first failure or on every input succeeding.
 */
export enum FutureGroupKind {
    Race = 0,
    All = 1,
}

/* The active runtime while a resume is executing, so a future acquires it on
 * first await. Module-level, matching the C file's g_current_runtime; a WASM
 * guest has one cooperative runtime per module. */
let g_currentRuntime: Runtime | null = null;

/**
 * The observing half of a one-shot result, settled exactly once by the Promise
 * bound to it. `status` is 0 once resolved and the rejecting negative status
 * once failed; `value` is the resolution value, protocol-defined and often an
 * object address. `runtime` is adopted from the first await or `then`, and
 * mixing a future between runtimes is refused.
 */
export class Future {
    state: FutureState = FutureState.Pending;
    status: i32 = 0;
    value: usize = 0;
    runtime: Runtime | null = null;
    waiters: Coroutine | null = null;
    continuations: Continuation | null = null;

    /** Reset to pending and bind `promise` as the only settling handle. */
    init(promise: Promise): void {
        this.state = FutureState.Pending;
        this.status = 0;
        this.value = 0;
        this.runtime = null;
        this.waiters = null;
        this.continuations = null;
        promise.future = this;
    }

    /** True once settled, writing status/value into the boxes provided. */
    poll(outStatus: Box | null, outValue: Box | null): bool {
        if (this.state == FutureState.Pending) return false;
        if (outStatus !== null) outStatus.value = <usize>this.status;
        if (outValue !== null) outValue.value = this.value;
        return true;
    }

    /**
     * Settled: returns its status and writes the value. Pending: parks the
     * running coroutine and returns AWAIT_PENDING, on which the caller MUST
     * return TASK_YIELDED without touching the value.
     */
    await(outValue: Box | null): i32 {
        if (this.state != FutureState.Pending) {
            if (outValue !== null) outValue.value = this.value;
            return this.status;
        }
        const runtime = this.runtime !== null ? this.runtime : g_currentRuntime;
        if (runtime === null) return -1;
        const coroutine = runtime.current;
        if (
            coroutine === null ||
            coroutine.state != CoroutineState.Running ||
            (this.runtime !== null && this.runtime !== runtime)
        ) {
            return -1;
        }
        this.runtime = runtime;
        coroutine.waitNext = this.waiters;
        this.waiters = coroutine;
        coroutine.state = CoroutineState.Waiting;
        return AWAIT_PENDING;
    }
}

/** The settling half of a future. Only the holder may resolve or reject. */
export class Promise {
    future: Future | null = null;

    /**
     * Settles the bound future as ready, waking every parked waiter and queueing
     * every registered continuation. False when there is no bound future or it
     * has already settled.
     */
    resolve(value: usize): bool {
        return promiseComplete(this, 0, value);
    }

    /** Rejection requires a negative status; 0 or positive is refused. */
    reject(status: i32): bool {
        return status < 0 && promiseComplete(this, status, 0);
    }
}

/**
 * One stackless task and its completion future. asyncStart rebinds the whole
 * record, so an instance is reusable once its previous run is Dead; `result` is
 * the status its resume last returned.
 */
export class Coroutine {
    runtime: Runtime | null = null;
    next: Coroutine | null = null;
    waitNext: Coroutine | null = null;
    task: Task | null = null;
    state: CoroutineState = CoroutineState.New;
    result: i32 = 0;
    completion: Future = new Future();
    completionPromise: Promise = new Promise();

    /** Await this coroutine's completion; see Future.await for the contract. */
    join(outResult: Box | null): i32 {
        const box = new Box();
        const status = this.completion.await(box);
        if (status == 0 && outResult !== null) outResult.value = box.value;
        return status;
    }
}

/**
 * Registration record for one `then` callback, carrying the child future that
 * callback settles. It must stay live until the callback has been dispatched and
 * cannot hold two live registrations: `active` is what refuses the second.
 */
export class Continuation {
    next: Continuation | null = null;
    future: Future | null = null;
    onSuccess: OnSuccess | null = null;
    onError: OnError | null = null;
    group: FutureGroup | null = null;
    groupIndex: i32 = 0;
    /* The C original reuses its `group` field to carry the adopt continuation for
     * then_flat, distinguished only by the fact that group callbacks never return
     * CHAIN_NEXT. Kept as a separate field here, which is the same behaviour and
     * makes the confusion structurally impossible rather than conventional. */
    adopt: Continuation | null = null;
    child: Future = new Future();
    childPromise: Promise = new Promise();
    active: bool = false;
}

/**
 * Storage combining several futures into one. The group, its continuation array
 * and (for All) its value array must stay live until the group future settles;
 * at that point the still-registered continuations on unsettled inputs are
 * released, so they need not outlive the slowest source. `completed` counts the
 * source callbacks that ran -- a settled race leaves it at 1 -- and `active`
 * goes false once the group settles, after which a late source callback is
 * ignored.
 */
export class FutureGroup {
    runtime: Runtime | null = null;
    future: Future = new Future();
    promise: Promise = new Promise();
    continuations: StaticArray<Continuation> | null = null;
    values: StaticArray<usize> | null = null;
    count: i32 = 0;
    completed: i32 = 0;
    kind: FutureGroupKind = FutureGroupKind.Race;
    settled: bool = false;
    active: bool = false;
}

/**
 * The cooperative scheduler: a queue of runnable coroutines and a queue of
 * continuations whose source future has settled. A guest normally has one, and
 * nothing runs until run/runBudget is called.
 */
export class Runtime {
    current: Coroutine | null = null;
    readyHead: Coroutine | null = null;
    readyTail: Coroutine | null = null;
    continuationHead: Continuation | null = null;
    continuationTail: Continuation | null = null;
    running: bool = false;

    /**
     * Schedule `task` on `coroutine`; returns its completion future.
     * Null when the coroutine is neither New nor Dead, so a live task cannot be
     * restarted. The completion future resolves with the task's boxed value or
     * rejects with its failure status; nothing runs until the runtime is driven.
     */
    asyncStart(coroutine: Coroutine, task: Task): Future | null {
        if (coroutine.state != CoroutineState.New && coroutine.state != CoroutineState.Dead) {
            return null;
        }
        coroutine.runtime = this;
        coroutine.next = null;
        coroutine.waitNext = null;
        coroutine.task = task;
        coroutine.state = CoroutineState.Ready;
        coroutine.result = 0;
        coroutine.completion.init(coroutine.completionPromise);
        coroutine.completion.runtime = this;
        this.enqueue(coroutine);
        return coroutine.completion;
    }

    /**
     * Resume up to `budget` coroutines, then drain continuations. Returns the
     * number resumed, or -1 if already running (re-entry is refused).
     */
    runBudget(budget: i32): i32 {
        if (this.running) return -1;
        this.running = true;
        let resumed = 0;
        for (;;) {
            if (budget != 0) {
                const coroutine = this.dequeue();
                if (coroutine !== null) {
                    const box = new Box();
                    this.current = coroutine;
                    coroutine.state = CoroutineState.Running;
                    resumed++;
                    budget--;
                    g_currentRuntime = this;
                    const task = coroutine.task;
                    const status = task !== null ? task.resume(box) : -1;
                    g_currentRuntime = null;
                    this.current = null;
                    if (status == TASK_YIELDED) {
                        /* A task that yielded without parking itself on a future is
                         * simply rescheduled; one that awaited is already WAITING. */
                        if (coroutine.state == CoroutineState.Running) {
                            coroutine.state = CoroutineState.Ready;
                            this.enqueue(coroutine);
                        }
                    } else {
                        coroutine.result = status;
                        coroutine.state = CoroutineState.Dead;
                        if (status == 0) coroutine.completionPromise.resolve(box.value);
                        else coroutine.completionPromise.reject(status);
                    }
                    continue;
                }
            }
            const continuation = this.dequeueContinuation();
            if (continuation === null) break;
            continuationDispatch(continuation);
        }
        this.running = false;
        return resumed;
    }

    /**
     * Run until nothing is ready.
     * Coroutines parked on futures nothing settles are left parked, not
     * reported; -1 on re-entry, as for runBudget.
     */
    run(): i32 {
        return this.runBudget(i32.MAX_VALUE);
    }

    /**
     * Register `continuation` on `future`, returning the child future that
     * settles with the callback's result. Null if the continuation is already in
     * use or the future belongs to another runtime.
     *
     * The callback never runs inline: it is queued on this runtime -- at once if
     * the future has already settled -- and dispatched from run/runBudget, at
     * most once. With no callback for the outcome that happened, the child
     * inherits the source's value or status.
     */
    then(
        future: Future,
        continuation: Continuation,
        onSuccess: OnSuccess | null,
        onError: OnError | null,
    ): Future | null {
        if (continuation.active || (future.runtime !== null && future.runtime !== this)) {
            return null;
        }
        future.runtime = this;
        continuation.next = null;
        continuation.future = future;
        continuation.onSuccess = onSuccess;
        continuation.onError = onError;
        continuation.group = null;
        continuation.groupIndex = 0;
        continuation.adopt = null;
        continuation.active = true;
        continuation.child.init(continuation.childPromise);
        continuation.child.runtime = this;
        if (future.state == FutureState.Pending) {
            continuation.next = future.continuations;
            future.continuations = continuation;
        } else {
            this.enqueueContinuation(continuation);
        }
        return continuation.child;
    }

    /**
     * As `then`, but a success callback returning FUTURE_CHAIN_NEXT with a
     * future boxed in `out` makes the child adopt that future's eventual result.
     * `adoptContinuation` is caller-owned storage for the adoption.
     */
    thenFlat(
        future: Future,
        continuation: Continuation,
        adoptContinuation: Continuation,
        onSuccess: OnSuccess | null,
        onError: OnError | null,
    ): Future | null {
        const child = this.then(future, continuation, onSuccess, onError);
        if (child === null) return null;
        continuation.adopt = adoptContinuation;
        return child;
    }

    /**
     * Settles with the first input to settle, success or failure.
     *
     * The losers are abandoned, not cancelled: their own work continues and
     * their results are discarded. Null when `inputs` is empty, `continuations`
     * is shorter than it, a continuation is already in use, or an input belongs
     * to another runtime. Allocates the FutureGroup; a long-lived loop uses
     * raceInto.
     */
    race(inputs: StaticArray<Future>, continuations: StaticArray<Continuation>): Future | null {
        return this.groupStart(
            inputs,
            continuations,
            null,
            FutureGroupKind.Race,
            new FutureGroup(),
        );
    }

    /**
     * Settles once every input succeeds, with `values` filled in input order,
     * or rejects on the first failure.
     *
     * Resolves with the address of `values`; the array is only fully populated
     * on success, and must stay live until the group future settles. Same
     * refusals as `race`, plus a `values` shorter than `inputs`.
     */
    all(
        inputs: StaticArray<Future>,
        continuations: StaticArray<Continuation>,
        values: StaticArray<usize>,
    ): Future | null {
        return this.groupStart(
            inputs,
            continuations,
            values,
            FutureGroupKind.All,
            new FutureGroup(),
        );
    }

    /**
     * race/all onto caller-owned group storage, for allocation-free reuse.
     * The group is reset by the call, so it may be reused once the previous
     * group future has settled.
     */
    raceInto(
        group: FutureGroup,
        inputs: StaticArray<Future>,
        continuations: StaticArray<Continuation>,
    ): Future | null {
        return this.groupStart(inputs, continuations, null, FutureGroupKind.Race, group);
    }

    /** `all` onto caller-owned group storage; see raceInto and all. */
    allInto(
        group: FutureGroup,
        inputs: StaticArray<Future>,
        continuations: StaticArray<Continuation>,
        values: StaticArray<usize>,
    ): Future | null {
        return this.groupStart(inputs, continuations, values, FutureGroupKind.All, group);
    }

    // ---------------------------------------------------------------- internals
    // Queue plumbing, public only because the promise helpers below are module
    // functions rather than methods. Application code drives the runtime through
    // asyncStart/run/then and does not call these.

    enqueue(coroutine: Coroutine): void {
        coroutine.next = null;
        const tail = this.readyTail;
        if (tail !== null) tail.next = coroutine;
        else this.readyHead = coroutine;
        this.readyTail = coroutine;
    }

    dequeue(): Coroutine | null {
        const coroutine = this.readyHead;
        if (coroutine === null) return null;
        this.readyHead = coroutine.next;
        if (this.readyHead === null) this.readyTail = null;
        coroutine.next = null;
        return coroutine;
    }

    enqueueContinuation(continuation: Continuation): void {
        continuation.next = null;
        const tail = this.continuationTail;
        if (tail !== null) tail.next = continuation;
        else this.continuationHead = continuation;
        this.continuationTail = continuation;
    }

    dequeueContinuation(): Continuation | null {
        const continuation = this.continuationHead;
        if (continuation === null) return null;
        this.continuationHead = continuation.next;
        if (this.continuationHead === null) this.continuationTail = null;
        continuation.next = null;
        return continuation;
    }

    private groupStart(
        inputs: StaticArray<Future>,
        continuations: StaticArray<Continuation>,
        values: StaticArray<usize> | null,
        kind: FutureGroupKind,
        group: FutureGroup,
    ): Future | null {
        const count = inputs.length;
        if (count == 0 || continuations.length < count) return null;
        if (kind == FutureGroupKind.All && (values === null || values.length < count)) {
            return null;
        }
        for (let i = 0; i < count; ++i) {
            const input = inputs[i];
            if (continuations[i].active || (input.runtime !== null && input.runtime !== this)) {
                return null;
            }
        }
        group.runtime = this;
        group.continuations = continuations;
        group.values = values;
        group.count = count;
        group.completed = 0;
        group.kind = kind;
        group.settled = false;
        group.active = true;
        group.future.init(group.promise);
        group.future.runtime = this;
        for (let i = 0; i < count; ++i) {
            const onSuccess = new GroupCallback(group, i);
            const onError = new GroupErrorCallback(group);
            if (this.then(inputs[i], continuations[i], onSuccess, onError) === null) {
                group.active = false;
                return null;
            }
            continuations[i].group = group;
            continuations[i].groupIndex = i;
        }
        return group.future;
    }
}

// -------------------------------------------------------------------- helpers

function promiseComplete(promise: Promise, status: i32, value: usize): bool {
    const future = promise.future;
    if (future === null || future.state != FutureState.Pending) return false;
    future.state = status == 0 ? FutureState.Ready : FutureState.Failed;
    future.status = status;
    future.value = value;

    let waiter = future.waiters;
    future.waiters = null;
    while (waiter !== null) {
        const next = waiter.waitNext;
        waiter.waitNext = null;
        if (waiter.state == CoroutineState.Waiting) {
            waiter.state = CoroutineState.Ready;
            const runtime = waiter.runtime;
            if (runtime !== null) runtime.enqueue(waiter);
        }
        waiter = next;
    }

    let continuation = future.continuations;
    future.continuations = null;
    const runtime = future.runtime;
    while (continuation !== null) {
        const next = continuation.next;
        if (continuation.active && runtime !== null) {
            runtime.enqueueContinuation(continuation);
        }
        continuation = next;
    }
    return true;
}

/** Forwards an adopted future's settlement onto the flat-map child. */
class FlatForward extends OnSuccess {
    constructor(private target: Promise) {
        super();
    }
    call(value: usize, out: Box): i32 {
        if (!this.target.resolve(value)) return -1;
        out.value = value;
        return 0;
    }
}

class FlatForwardError extends OnError {
    constructor(private target: Promise) {
        super();
    }
    call(status: i32, out: Box): i32 {
        if (!this.target.reject(status)) return -1;
        out.value = 0;
        return status;
    }
}

function continuationDispatch(continuation: Continuation): void {
    const future = continuation.future;
    const box = new Box();
    continuation.active = false;
    continuation.future = null;
    if (future === null || future.state == FutureState.Pending) return;

    let status: i32;
    if (future.status == 0) {
        const onSuccess = continuation.onSuccess;
        if (onSuccess !== null) {
            status = onSuccess.call(future.value, box);
        } else {
            status = 0;
            box.value = future.value;
        }
    } else {
        const onError = continuation.onError;
        status = onError !== null ? onError.call(future.status, box) : future.status;
    }

    const adopt = continuation.adopt;
    if (status == FUTURE_CHAIN_NEXT && adopt !== null) {
        continuation.adopt = null;
        const runtime = continuation.child.runtime;
        const next = box.value != 0 ? changetype<Future>(box.value) : null;
        if (
            next === null ||
            runtime === null ||
            runtime.then(
                next,
                adopt,
                new FlatForward(continuation.childPromise),
                new FlatForwardError(continuation.childPromise),
            ) === null
        ) {
            continuation.childPromise.reject(-1);
        }
    } else if (status == 0) {
        continuation.childPromise.resolve(box.value);
    } else {
        continuation.childPromise.reject(status < 0 ? status : -1);
    }
}

function continuationListRemoveFromFuture(future: Future, target: Continuation): void {
    let prev: Continuation | null = null;
    let cur = future.continuations;
    while (cur !== null) {
        if (cur === target) {
            if (prev !== null) prev.next = cur.next;
            else future.continuations = cur.next;
            return;
        }
        prev = cur;
        cur = cur.next;
    }
}

function continuationListRemoveFromRuntime(runtime: Runtime, target: Continuation): void {
    let prev: Continuation | null = null;
    let cur = runtime.continuationHead;
    while (cur !== null) {
        if (cur === target) {
            if (prev !== null) prev.next = cur.next;
            else runtime.continuationHead = cur.next;
            if (runtime.continuationTail === target) runtime.continuationTail = prev;
            return;
        }
        prev = cur;
        cur = cur.next;
    }
}

/* Detach a still-registered continuation from its pending source future, or
 * from the dispatch queue if the source already settled. One that has already
 * been dispatched (active == false) is left alone. */
function continuationCancel(runtime: Runtime | null, continuation: Continuation): void {
    if (!continuation.active) return;
    const future = continuation.future;
    if (future !== null && future.state == FutureState.Pending) {
        continuationListRemoveFromFuture(future, continuation);
    } else if (runtime !== null) {
        continuationListRemoveFromRuntime(runtime, continuation);
    }
    continuation.next = null;
    continuation.future = null;
    continuation.active = false;
}

/* Once a group settles, release its remaining source continuations so the
 * caller's group storage need not outlive slow sources. */
function futureGroupAbandon(group: FutureGroup): void {
    const continuations = group.continuations;
    if (continuations === null) return;
    for (let i = 0; i < group.count; ++i) {
        continuationCancel(group.runtime, continuations[i]);
    }
    group.active = false;
}

/** One source's callbacks for a race/all group. */
class GroupCallback extends OnSuccess {
    constructor(
        private group: FutureGroup,
        private index: i32,
    ) {
        super();
    }

    call(value: usize, out: Box): i32 {
        const group = this.group;
        if (!group.active) return -1;
        if (group.kind == FutureGroupKind.Race) {
            if (!group.settled) {
                group.settled = true;
                group.promise.resolve(value);
                futureGroupAbandon(group);
            }
        } else {
            const values = group.values;
            if (values !== null) values[this.index] = value;
            if (!group.settled && group.completed + 1 == group.count) {
                group.settled = true;
                group.promise.resolve(values !== null ? changetype<usize>(values) : 0);
                futureGroupAbandon(group);
            }
        }
        group.completed++;
        if (group.completed == group.count) group.active = false;
        out.value = value;
        return 0;
    }
}

/* AS has no multiple inheritance, so the group's error path is its own class
 * over the same state rather than one object implementing both callbacks. */
class GroupErrorCallback extends OnError {
    constructor(private group: FutureGroup) {
        super();
    }

    call(status: i32, out: Box): i32 {
        const group = this.group;
        if (!group.active || status >= 0) return -1;
        if (!group.settled) {
            group.settled = true;
            group.promise.reject(status);
            futureGroupAbandon(group);
        }
        group.completed++;
        if (group.completed == group.count) group.active = false;
        out.value = 0;
        return status;
    }
}
