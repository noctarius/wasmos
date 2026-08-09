/* coroutine.ts - cooperative stackless coroutines for AssemblyScript guests.
 *
 * Every other guest language binds src/libsys/wasm/coroutine_wasm.c: Zig, Rust
 * and Go each compile that C file into their module. AssemblyScript cannot --
 * `asc` has no external linking -- which is the only reason AS had no
 * coroutines at all. This is a port of that runtime, not a new design: the
 * semantics below are the ones coroutine_wasm.c implements, and the shared
 * behaviour tests mirror its test suite.
 *
 * Stackless, like the C original. A task never has its call stack saved; its
 * resume method records its own progress in fields it owns, returns YIELDED,
 * and is called again by the runtime. That is what makes it work on a WASM
 * guest at all -- neither engine can swap a guest stack.
 *
 * Ownership: apps are built with `--runtime stub`, which is a bump allocator
 * with no collector, so nothing here allocates per operation. Callers own and
 * REUSE their Coroutine/Future/Continuation objects, exactly as the C API takes
 * caller-provided structs. Allocating a Future per IPC round-trip in a
 * long-lived driver would leak until the process exits.
 */

/** Terminal and pending states of a future. */
export enum FutureState {
    Pending = 0,
    Ready = 1,
    Failed = 2,
}

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

export enum FutureGroupKind {
    Race = 0,
    All = 1,
}

/* The active runtime while a resume is executing, so a future acquires it on
 * first await. Module-level, matching the C file's g_current_runtime; a WASM
 * guest has one cooperative runtime per module. */
let g_currentRuntime: Runtime|null = null;

export class Future {
    state: FutureState = FutureState.Pending;
    status: i32 = 0;
    value: usize = 0;
    runtime: Runtime|null = null;
    waiters: Coroutine|null = null;
    continuations: Continuation|null = null;

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
    poll(outStatus: Box|null, outValue: Box|null): bool {
        if (this.state == FutureState.Pending)
            return false;
        if (outStatus !== null)
            outStatus.value = <usize>this.status;
        if (outValue !== null)
            outValue.value = this.value;
        return true;
    }

    /**
     * Settled: returns its status and writes the value. Pending: parks the
     * running coroutine and returns AWAIT_PENDING, on which the caller MUST
     * return TASK_YIELDED without touching the value.
     */
    await(outValue: Box|null): i32 {
        if (this.state != FutureState.Pending) {
            if (outValue !== null)
                outValue.value = this.value;
            return this.status;
        }
        const runtime = this.runtime !== null ? this.runtime : g_currentRuntime;
        if (runtime === null)
            return -1;
        const coroutine = runtime.current;
        if (coroutine === null || coroutine.state != CoroutineState.Running ||
            (this.runtime !== null && this.runtime !== runtime)) {
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
    future: Future|null = null;

    resolve(value: usize): bool {
        return promiseComplete(this, 0, value);
    }

    /** Rejection requires a negative status; 0 or positive is refused. */
    reject(status: i32): bool {
        return status < 0 && promiseComplete(this, status, 0);
    }
}

export class Coroutine {
    runtime: Runtime|null = null;
    next: Coroutine|null = null;
    waitNext: Coroutine|null = null;
    task: Task|null = null;
    state: CoroutineState = CoroutineState.New;
    result: i32 = 0;
    completion: Future = new Future();
    completionPromise: Promise = new Promise();

    /** Await this coroutine's completion; see Future.await for the contract. */
    join(outResult: Box|null): i32 {
        const box = new Box();
        const status = this.completion.await(box);
        if (status == 0 && outResult !== null)
            outResult.value = box.value;
        return status;
    }
}

export class Continuation {
    next: Continuation|null = null;
    future: Future|null = null;
    onSuccess: OnSuccess|null = null;
    onError: OnError|null = null;
    group: FutureGroup|null = null;
    groupIndex: i32 = 0;
    /* The C original reuses its `group` field to carry the adopt continuation for
     * then_flat, distinguished only by the fact that group callbacks never return
     * CHAIN_NEXT. Kept as a separate field here, which is the same behaviour and
     * makes the confusion structurally impossible rather than conventional. */
    adopt: Continuation|null = null;
    child: Future = new Future();
    childPromise: Promise = new Promise();
    active: bool = false;
}

export class FutureGroup {
    runtime: Runtime|null = null;
    future: Future = new Future();
    promise: Promise = new Promise();
    continuations: StaticArray<Continuation>|null = null;
    values: StaticArray<usize>|null = null;
    count: i32 = 0;
    completed: i32 = 0;
    kind: FutureGroupKind = FutureGroupKind.Race;
    settled: bool = false;
    active: bool = false;
}

export class Runtime {
    current: Coroutine|null = null;
    readyHead: Coroutine|null = null;
    readyTail: Coroutine|null = null;
    continuationHead: Continuation|null = null;
    continuationTail: Continuation|null = null;
    running: bool = false;

    /** Schedule `task` on `coroutine`; returns its completion future. */
    asyncStart(coroutine: Coroutine, task: Task): Future|null {
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
        if (this.running)
            return -1;
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
                        if (status == 0)
                            coroutine.completionPromise.resolve(box.value);
                        else
                            coroutine.completionPromise.reject(status);
                    }
                    continue;
                }
            }
            const continuation = this.dequeueContinuation();
            if (continuation === null)
                break;
            continuationDispatch(continuation);
        }
        this.running = false;
        return resumed;
    }

    /** Run until nothing is ready. */
    run(): i32 {
        return this.runBudget(i32.MAX_VALUE);
    }

    /**
     * Register `continuation` on `future`, returning the child future that
     * settles with the callback's result. Null if the continuation is already in
     * use or the future belongs to another runtime.
     */
    then(future: Future, continuation: Continuation, onSuccess: OnSuccess|null,
         onError: OnError|null): Future|null {
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
    thenFlat(future: Future, continuation: Continuation, adoptContinuation: Continuation,
             onSuccess: OnSuccess|null, onError: OnError|null): Future|null {
        const child = this.then(future, continuation, onSuccess, onError);
        if (child === null)
            return null;
        continuation.adopt = adoptContinuation;
        return child;
    }

    /** Settles with the first input to settle, success or failure. */
    race(inputs: StaticArray<Future>, continuations: StaticArray<Continuation>): Future|null {
        return this.groupStart(inputs, continuations, null, FutureGroupKind.Race,
                               new FutureGroup());
    }

    /**
     * Settles once every input succeeds, with `values` filled in input order,
     * or rejects on the first failure.
     */
    all(inputs: StaticArray<Future>, continuations: StaticArray<Continuation>,
        values: StaticArray<usize>): Future|null {
        return this.groupStart(inputs, continuations, values, FutureGroupKind.All,
                               new FutureGroup());
    }

    /** race/all onto caller-owned group storage, for allocation-free reuse. */
    raceInto(group: FutureGroup, inputs: StaticArray<Future>,
             continuations: StaticArray<Continuation>): Future|null {
        return this.groupStart(inputs, continuations, null, FutureGroupKind.Race, group);
    }

    allInto(group: FutureGroup, inputs: StaticArray<Future>,
            continuations: StaticArray<Continuation>, values: StaticArray<usize>): Future|null {
        return this.groupStart(inputs, continuations, values, FutureGroupKind.All, group);
    }

    // ---------------------------------------------------------------- internals

    enqueue(coroutine: Coroutine): void {
        coroutine.next = null;
        const tail = this.readyTail;
        if (tail !== null)
            tail.next = coroutine;
        else
            this.readyHead = coroutine;
        this.readyTail = coroutine;
    }

    dequeue(): Coroutine|null {
        const coroutine = this.readyHead;
        if (coroutine === null)
            return null;
        this.readyHead = coroutine.next;
        if (this.readyHead === null)
            this.readyTail = null;
        coroutine.next = null;
        return coroutine;
    }

    enqueueContinuation(continuation: Continuation): void {
        continuation.next = null;
        const tail = this.continuationTail;
        if (tail !== null)
            tail.next = continuation;
        else
            this.continuationHead = continuation;
        this.continuationTail = continuation;
    }

    dequeueContinuation(): Continuation|null {
        const continuation = this.continuationHead;
        if (continuation === null)
            return null;
        this.continuationHead = continuation.next;
        if (this.continuationHead === null)
            this.continuationTail = null;
        continuation.next = null;
        return continuation;
    }

    private groupStart(inputs: StaticArray<Future>, continuations: StaticArray<Continuation>,
                       values: StaticArray<usize>|null, kind: FutureGroupKind,
                       group: FutureGroup): Future|null {
        const count = inputs.length;
        if (count == 0 || continuations.length < count)
            return null;
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
    if (future === null || future.state != FutureState.Pending)
        return false;
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
            if (runtime !== null)
                runtime.enqueue(waiter);
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
        if (!this.target.resolve(value))
            return -1;
        out.value = value;
        return 0;
    }
}

class FlatForwardError extends OnError {
    constructor(private target: Promise) {
        super();
    }
    call(status: i32, out: Box): i32 {
        if (!this.target.reject(status))
            return -1;
        out.value = 0;
        return status;
    }
}

function continuationDispatch(continuation: Continuation): void {
    const future = continuation.future;
    const box = new Box();
    continuation.active = false;
    continuation.future = null;
    if (future === null || future.state == FutureState.Pending)
        return;

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
        if (next === null || runtime === null ||
            runtime.then(next, adopt, new FlatForward(continuation.childPromise),
                         new FlatForwardError(continuation.childPromise)) === null) {
            continuation.childPromise.reject(-1);
        }
    } else if (status == 0) {
        continuation.childPromise.resolve(box.value);
    } else {
        continuation.childPromise.reject(status < 0 ? status : -1);
    }
}

function continuationListRemoveFromFuture(future: Future, target: Continuation): void {
    let prev: Continuation|null = null;
    let cur = future.continuations;
    while (cur !== null) {
        if (cur === target) {
            if (prev !== null)
                prev.next = cur.next;
            else
                future.continuations = cur.next;
            return;
        }
        prev = cur;
        cur = cur.next;
    }
}

function continuationListRemoveFromRuntime(runtime: Runtime, target: Continuation): void {
    let prev: Continuation|null = null;
    let cur = runtime.continuationHead;
    while (cur !== null) {
        if (cur === target) {
            if (prev !== null)
                prev.next = cur.next;
            else
                runtime.continuationHead = cur.next;
            if (runtime.continuationTail === target)
                runtime.continuationTail = prev;
            return;
        }
        prev = cur;
        cur = cur.next;
    }
}

/* Detach a still-registered continuation from its pending source future, or
 * from the dispatch queue if the source already settled. One that has already
 * been dispatched (active == false) is left alone. */
function continuationCancel(runtime: Runtime|null, continuation: Continuation): void {
    if (!continuation.active)
        return;
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
    if (continuations === null)
        return;
    for (let i = 0; i < group.count; ++i) {
        continuationCancel(group.runtime, continuations[i]);
    }
    group.active = false;
}

/** One source's callbacks for a race/all group. */
class GroupCallback extends OnSuccess {
    constructor(private group: FutureGroup, private index: i32) {
        super();
    }

    call(value: usize, out: Box): i32 {
        const group = this.group;
        if (!group.active)
            return -1;
        if (group.kind == FutureGroupKind.Race) {
            if (!group.settled) {
                group.settled = true;
                group.promise.resolve(value);
                futureGroupAbandon(group);
            }
        } else {
            const values = group.values;
            if (values !== null)
                values[this.index] = value;
            if (!group.settled && group.completed + 1 == group.count) {
                group.settled = true;
                group.promise.resolve(values !== null ? changetype<usize>(values) : 0);
                futureGroupAbandon(group);
            }
        }
        group.completed++;
        if (group.completed == group.count)
            group.active = false;
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
        if (!group.active || status >= 0)
            return -1;
        if (!group.settled) {
            group.settled = true;
            group.promise.reject(status);
            futureGroupAbandon(group);
        }
        group.completed++;
        if (group.completed == group.count)
            group.active = false;
        out.value = 0;
        return status;
    }
}
