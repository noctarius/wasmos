/* test_as_coroutine.ts — parity tests for the AssemblyScript coroutine runtime.
 *
 * Replicates tests/unit/test_wasm_coroutine.c case for case, with the same
 * scenarios, the same values and the same assertions, so the AS port is held to
 * the behaviour of the C runtime every other guest language links rather than
 * to a fresh reading of the spec.
 *
 * Cases 1-4 of the C suite are here. Its cases 5 and 6 (test_ipc_future,
 * test_fs_request_future) exercise ipc_future_wasm.c and the FS request bridge,
 * which are separate modules AS does not have yet; they are not coroutine
 * runtime tests and are deliberately not faked here.
 *
 * Each case returns 0 or its failing line, exactly like the C original, and
 * runTests() returns the first non-zero. The host harness reads that.
 */

import {
    Box,
    Continuation,
    Coroutine,
    Future,
    FutureGroup,
    OnError,
    OnSuccess,
    Promise,
    Runtime,
    Task,
    CoroutineState,
    AWAIT_PENDING,
    TASK_COMPLETE,
    TASK_YIELDED,
} from "./coroutine";
import {runShuffled, TestCase} from "./shuffle";

// ------------------------------------------------------------- case 1 tasks

class YieldTask extends Task {
    pc: i32 = 0;
    events: StaticArray<u32>;
    eventCount: i32 = 0;

    constructor(events: StaticArray<u32>) {
        super();
        this.events = events;
    }

    resume(out: Box): i32 {
        if (this.pc == 0) {
            this.events[this.eventCount++] = 1;
            this.pc = 1;
            return TASK_YIELDED;
        }
        this.events[this.eventCount++] = 3;
        out.value = 7;
        return TASK_COMPLETE;
    }
}

class WaiterTask extends Task {
    pc: i32 = 0;
    status: i32 = 0;
    value: usize = 0;
    private box: Box = new Box();

    constructor(private future: Future) {
        super();
    }

    resume(out: Box): i32 {
        if (this.pc == 0) {
            this.pc = 1;
            this.status = this.future.await(this.box);
            this.value = this.box.value;
            if (this.status == AWAIT_PENDING) return TASK_YIELDED;
        }
        if (this.pc == 1) {
            this.status = this.future.await(this.box);
            this.value = this.box.value;
            if (this.status == AWAIT_PENDING) return TASK_YIELDED;
            out.value = this.value;
            return this.status;
        }
        return -1;
    }
}

class ResolverTask extends Task {
    constructor(
        private promise: Promise,
        private v: usize,
    ) {
        super();
    }

    resume(out: Box): i32 {
        if (!this.promise.resolve(this.v)) return -1;
        out.value = this.v;
        return 0;
    }
}

function testYieldAwaitAndJoin(): i32 {
    const runtime = new Runtime();
    const first = new Coroutine();
    const waiter = new Coroutine();
    const resolver = new Coroutine();
    const source = new Future();
    const promise = new Promise();
    const events = new StaticArray<u32>(4);

    source.init(promise);
    const yieldTask = new YieldTask(events);
    const waiterTask = new WaiterTask(source);
    const resolverTask = new ResolverTask(promise, 42);

    if (runtime.asyncStart(first, yieldTask) === null) return 110;
    if (runtime.asyncStart(waiter, waiterTask) === null) return 111;
    if (runtime.asyncStart(resolver, resolverTask) === null) return 112;
    /* Five resumes: yield task twice, waiter twice (parks, then completes),
     * resolver once. The count is the schedule, so it is asserted exactly. */
    if (runtime.run() != 5) return 113;
    if (yieldTask.eventCount != 2) return 114;
    if (events[0] != 1) return 115;
    if (events[1] != 3) return 116;
    if (waiter.state != CoroutineState.Dead) return 117;
    if (waiterTask.status != 0) return 118;
    if (waiterTask.value != 42) return 119;

    const joined = new Box();
    if (first.join(joined) != 0) return 120;
    if (joined.value != 7) return 121;

    const status = new Box();
    const value = new Box();
    if (!waiter.completion.poll(status, value)) return 122;
    if (<i32>status.value != 0) return 123;
    if (value.value != 42) return 124;
    /* A settled promise cannot settle twice. */
    if (promise.resolve(9)) return 125;
    return 0;
}

// ------------------------------------------------------------- case 2 tasks

class Increment extends OnSuccess {
    calls: u32 = 0;
    call(value: usize, out: Box): i32 {
        this.calls++;
        out.value = value + 1;
        return 0;
    }
}

class Recover extends OnError {
    calls: u32 = 0;
    call(status: i32, out: Box): i32 {
        this.calls++;
        if (status >= 0) return -1;
        out.value = 55;
        return 0;
    }
}

class RejectCallback extends OnSuccess {
    call(value: usize, out: Box): i32 {
        return -41;
    }
}

function testFutureChainsAndDeferredCallbacks(): i32 {
    const runtime = new Runtime();
    const source = new Future();
    const sourcePromise = new Promise();
    const plusOne = new Continuation();
    const increment = new Increment();
    const status = new Box();
    const value = new Box();

    source.init(sourcePromise);
    let child = runtime.then(source, plusOne, increment, null);
    if (child === null) return 210;
    if (!sourcePromise.resolve(20)) return 211;
    /* The callback is deferred: resolving does not run it, the runtime does. */
    if (increment.calls != 0) return 212;
    if (runtime.run() != 0) return 213;
    if (increment.calls != 1) return 214;
    if (!child.poll(status, value)) return 215;
    if (<i32>status.value != 0) return 216;
    if (value.value != 21) return 217;

    const rejected = new Future();
    const rejectedPromise = new Promise();
    const recoverContinuation = new Continuation();
    const recover = new Recover();
    rejected.init(rejectedPromise);
    child = runtime.then(rejected, recoverContinuation, null, recover);
    if (child === null) return 220;
    if (!rejectedPromise.reject(-23)) return 221;
    /* A second rejection of a settled promise is refused. */
    if (rejectedPromise.reject(-24)) return 222;
    if (runtime.run() != 0) return 223;
    if (!child.poll(status, value)) return 224;
    /* The error handler recovered, so the child SUCCEEDS with its value. */
    if (<i32>status.value != 0) return 225;
    if (value.value != 55) return 226;

    const source2 = new Future();
    const source2Promise = new Promise();
    const rejectContinuation = new Continuation();
    source2.init(source2Promise);
    child = runtime.then(source2, rejectContinuation, new RejectCallback(), null);
    if (child === null) return 230;
    if (!source2Promise.resolve(1)) return 231;
    if (runtime.run() != 0) return 232;
    if (!child.poll(status, value)) return 233;
    /* A success callback returning a negative status rejects the child. */
    if (<i32>status.value != -41) return 234;
    if (value.value != 0) return 235;

    /* Registering on an ALREADY-SETTLED future must still defer: the callback
     * runs from the runtime, never inline from then(). The C suite only ever
     * registers before settling, so this path was untested there too -- a
     * mutation that dispatched inline survived its whole suite. */
    const settled = new Future();
    const settledPromise = new Promise();
    const lateContinuation = new Continuation();
    const lateIncrement = new Increment();
    settled.init(settledPromise);
    if (!settledPromise.resolve(70)) return 240;
    child = runtime.then(settled, lateContinuation, lateIncrement, null);
    if (child === null) return 241;
    if (lateIncrement.calls != 0) return 242;
    if (runtime.run() != 0) return 243;
    if (lateIncrement.calls != 1) return 244;
    if (!child.poll(status, value)) return 245;
    if (<i32>status.value != 0) return 246;
    if (value.value != 71) return 247;
    return 0;
}

// ------------------------------------------------------------------ case 3

function testRaceAndAll(): i32 {
    const runtime = new Runtime();
    const status = new Box();
    const value = new Box();

    const first = new Future();
    const second = new Future();
    const third = new Future();
    const firstPromise = new Promise();
    const secondPromise = new Promise();
    const thirdPromise = new Promise();
    first.init(firstPromise);
    second.init(secondPromise);
    third.init(thirdPromise);

    const raceInputs = StaticArray.fromArray<Future>([first, second, third]);
    const raceContinuations = new StaticArray<Continuation>(3);
    for (let i = 0; i < 3; ++i) raceContinuations[i] = new Continuation();
    const raceGroup = new FutureGroup();
    let result = runtime.raceInto(raceGroup, raceInputs, raceContinuations);
    if (result === null) return 310;
    if (!secondPromise.resolve(2)) return 311;
    if (!firstPromise.reject(-9)) return 312;
    if (!thirdPromise.resolve(3)) return 313;
    if (runtime.run() != 0) return 314;
    if (!result.poll(status, value)) return 315;
    /* First to settle wins, and the later ones cannot change it. */
    if (<i32>status.value != 0) return 316;
    if (value.value != 2) return 317;
    if (raceGroup.active) return 318;

    const a = new Future();
    const b = new Future();
    const c = new Future();
    const aPromise = new Promise();
    const bPromise = new Promise();
    const cPromise = new Promise();
    a.init(aPromise);
    b.init(bPromise);
    c.init(cPromise);
    const allInputs = StaticArray.fromArray<Future>([a, b, c]);
    const allContinuations = new StaticArray<Continuation>(3);
    for (let i = 0; i < 3; ++i) allContinuations[i] = new Continuation();
    const values = new StaticArray<usize>(3);
    const allGroup = new FutureGroup();
    result = runtime.allInto(allGroup, allInputs, allContinuations, values);
    if (result === null) return 320;
    /* Settled out of order; values must still land in INPUT order. */
    if (!cPromise.resolve(3)) return 321;
    if (!aPromise.resolve(1)) return 322;
    if (!bPromise.resolve(2)) return 323;
    if (runtime.run() != 0) return 324;
    if (!result.poll(status, value)) return 325;
    if (<i32>status.value != 0) return 326;
    if (value.value != changetype<usize>(values)) return 327;
    if (values[0] != 1) return 328;
    if (values[1] != 2) return 329;
    if (values[2] != 3) return 330;
    if (allGroup.active) return 331;

    const d = new Future();
    const e = new Future();
    const dPromise = new Promise();
    const ePromise = new Promise();
    d.init(dPromise);
    e.init(ePromise);
    const failedInputs = StaticArray.fromArray<Future>([d, e]);
    const failedContinuations = new StaticArray<Continuation>(2);
    for (let i = 0; i < 2; ++i) failedContinuations[i] = new Continuation();
    const failedGroup = new FutureGroup();
    result = runtime.allInto(
        failedGroup,
        failedInputs,
        failedContinuations,
        new StaticArray<usize>(2),
    );
    if (result === null) return 340;
    /* Fail-fast: the first rejection settles the group, marks it inactive and
     * unlinks the still-pending source, so its later completion is a no-op that
     * never touches group storage. */
    if (!dPromise.reject(-7)) return 341;
    if (runtime.run() != 0) return 342;
    if (!result.poll(status, value)) return 343;
    if (<i32>status.value != -7) return 344;
    if (!failedGroup.settled) return 345;
    if (failedGroup.active) return 346;
    if (failedContinuations[1].active) return 347;
    if (e.continuations !== null) return 348;
    if (!ePromise.resolve(2)) return 349;
    if (runtime.run() != 0) return 350;
    if (failedGroup.active) return 351;
    return 0;
}

// ------------------------------------------------------------------ case 4

function testContracts(): i32 {
    const runtime = new Runtime();
    const other = new Runtime();
    const coroutine = new Coroutine();
    const future = new Future();
    const promise = new Promise();
    const continuation = new Continuation();

    future.init(promise);
    /* Awaiting outside a running coroutine is refused. */
    if (future.await(null) != -1) return 410;
    /* Rejection demands a negative status. */
    if (promise.reject(0)) return 411;
    if (runtime.then(future, continuation, null, null) === null) return 412;
    /* A future belongs to one runtime, and a continuation to one registration. */
    if (other.then(future, new Continuation(), null, null) !== null) return 413;
    if (runtime.asyncStart(coroutine, new YieldTask(new StaticArray<u32>(4))) === null) {
        return 414;
    }
    /* A coroutine already scheduled cannot be started again. */
    if (runtime.asyncStart(coroutine, new YieldTask(new StaticArray<u32>(4))) !== null) {
        return 415;
    }
    /* Empty groups are refused. */
    if (runtime.race(new StaticArray<Future>(0), new StaticArray<Continuation>(0)) !== null) {
        return 416;
    }
    if (
        runtime.all(
            new StaticArray<Future>(0),
            new StaticArray<Continuation>(0),
            new StaticArray<usize>(0),
        ) !== null
    ) {
        return 417;
    }
    return 0;
}

// ------------------------------------------- cases 5-8: native-suite parity
//
// tests/unit/test_native_coroutine.c covers these and the WASM suites did not.
// None of them depend on the stackful model, so they apply here unchanged.

const WAITER_COUNT: i32 = 4;

class ManyWaiterTask extends Task {
    pc: i32 = 0;
    status: i32 = 0;
    value: usize = 0;
    private box: Box = new Box();

    constructor(private future: Future) {
        super();
    }

    resume(out: Box): i32 {
        if (this.pc == 0) {
            this.pc = 1;
            this.status = this.future.await(this.box);
            if (this.status == AWAIT_PENDING) return TASK_YIELDED;
        } else {
            this.status = this.future.await(this.box);
            if (this.status == AWAIT_PENDING) return TASK_YIELDED;
        }
        this.value = this.box.value;
        out.value = this.value;
        return this.status;
    }
}

/* Several coroutines parked on ONE future. The runtime keeps them on a
 * singly-linked wait list spliced at settle time; with a single waiter that
 * loop body runs once and `next` is always null, so nothing exercised it. */
function testMultipleWaiters(): i32 {
    for (let failing = 0; failing < 2; ++failing) {
        const runtime = new Runtime();
        const future = new Future();
        const promise = new Promise();
        future.init(promise);
        const tasks = new StaticArray<ManyWaiterTask>(WAITER_COUNT);
        const coroutines = new StaticArray<Coroutine>(WAITER_COUNT);
        for (let i = 0; i < WAITER_COUNT; ++i) {
            tasks[i] = new ManyWaiterTask(future);
            coroutines[i] = new Coroutine();
            if (runtime.asyncStart(coroutines[i], tasks[i]) === null) return 510 + failing;
        }
        /* Every waiter parks on the first drain. */
        if (runtime.run() != WAITER_COUNT) return 520 + failing;
        if (failing == 0) {
            if (!promise.resolve(77)) return 530;
        } else {
            if (!promise.reject(-31)) return 531;
        }
        /* Settling wakes ALL of them, so the second drain resumes each once. */
        if (runtime.run() != WAITER_COUNT) return 540 + failing;
        for (let i = 0; i < WAITER_COUNT; ++i) {
            if (coroutines[i].state != CoroutineState.Dead) return 550 + failing;
            if (failing == 0) {
                if (tasks[i].status != 0) return 560;
                if (tasks[i].value != 77) return 561;
            } else {
                if (tasks[i].status != -31) return 570;
                if (tasks[i].value != 0) return 571;
            }
        }
    }
    return 0;
}

class TargetTask extends Task {
    pc: i32 = 0;
    resume(out: Box): i32 {
        if (this.pc == 0) {
            this.pc = 1;
            return TASK_YIELDED;
        }
        out.value = 23;
        return TASK_COMPLETE;
    }
}

class JoinerTask extends Task {
    status: i32 = 0;
    result: usize = 0;
    pc: i32 = 0;
    private box: Box = new Box();

    constructor(private target: Coroutine) {
        super();
    }

    resume(out: Box): i32 {
        this.status = this.target.join(this.box);
        if (this.status == AWAIT_PENDING) {
            this.pc = 1;
            return TASK_YIELDED;
        }
        this.result = this.box.value;
        return 0;
    }
}

/* The same wait list, reached through a coroutine's completion future. */
function testMultipleJoiners(): i32 {
    const runtime = new Runtime();
    const target = new Coroutine();
    if (runtime.asyncStart(target, new TargetTask()) === null) return 610;
    const joiners = new StaticArray<Coroutine>(WAITER_COUNT);
    const tasks = new StaticArray<JoinerTask>(WAITER_COUNT);
    for (let i = 0; i < WAITER_COUNT; ++i) {
        joiners[i] = new Coroutine();
        tasks[i] = new JoinerTask(target);
        if (runtime.asyncStart(joiners[i], tasks[i]) === null) return 611;
    }
    if (runtime.run() < 0) return 612;
    for (let i = 0; i < WAITER_COUNT; ++i) {
        if (joiners[i].state != CoroutineState.Dead) return 620;
        if (tasks[i].status != 0) return 621;
        /* Every joiner sees the target's return value, not just the first. */
        if (tasks[i].result != 23) return 622;
    }
    if (target.state != CoroutineState.Dead) return 623;
    return 0;
}

const STRESS_COUNT: i32 = 8;
const STRESS_YIELDS: i32 = 4;

class StressTask extends Task {
    private remaining: i32 = STRESS_YIELDS;
    constructor(private counter: Box) {
        super();
    }
    resume(out: Box): i32 {
        if (this.remaining > 0) {
            this.remaining--;
            return TASK_YIELDED;
        }
        this.counter.value++;
        out.value = 0;
        return TASK_COMPLETE;
    }
}

/* Round-robin fairness at scale: the exact resume count is the schedule, so a
 * queue that starved or double-scheduled anyone changes it. */
function testSchedulerStress(): i32 {
    const runtime = new Runtime();
    const completed = new Box();
    const coroutines = new StaticArray<Coroutine>(STRESS_COUNT);
    for (let i = 0; i < STRESS_COUNT; ++i) {
        coroutines[i] = new Coroutine();
        if (runtime.asyncStart(coroutines[i], new StressTask(completed)) === null) return 710;
    }
    if (runtime.run() != STRESS_COUNT * (STRESS_YIELDS + 1)) return 711;
    if (<i32>completed.value != STRESS_COUNT) return 712;
    const status = new Box();
    const value = new Box();
    for (let i = 0; i < STRESS_COUNT; ++i) {
        if (!coroutines[i].completion.poll(status, value)) return 720;
        if (<i32>status.value != 0) return 721;
        if (value.value != 0) return 722;
    }
    return 0;
}

/* A callback that re-enters run() and registers a further continuation. */
class ReentrantCallback extends OnSuccess {
    runResult: i32 = 0;
    nestedChild: Future | null = null;

    constructor(
        private runtime: Runtime,
        private settled: Future,
        private nested: Continuation,
    ) {
        super();
    }

    call(value: usize, out: Box): i32 {
        this.runResult = this.runtime.run();
        this.nestedChild = this.runtime.then(this.settled, this.nested, new Increment(), null);
        out.value = value;
        return this.nestedChild !== null ? 0 : -1;
    }
}

class RespawnTask extends Task {
    constructor(private runs: Box) {
        super();
    }
    resume(out: Box): i32 {
        this.runs.value++;
        out.value = 0;
        return TASK_COMPLETE;
    }
}

function testReentrancyRespawnAndPendingPoll(): i32 {
    const runtime = new Runtime();
    const source = new Future();
    const sourcePromise = new Promise();
    const settled = new Future();
    const settledPromise = new Promise();
    const continuation = new Continuation();
    const nested = new Continuation();
    source.init(sourcePromise);
    settled.init(settledPromise);

    /* poll() on a PENDING future reports nothing, leaving the boxes alone. */
    const status = new Box();
    const value = new Box();
    status.value = 123;
    value.value = 456;
    if (source.poll(status, value)) return 810;
    if (status.value != 123) return 811;
    if (value.value != 456) return 812;

    if (!settledPromise.resolve(9)) return 820;
    const callback = new ReentrantCallback(runtime, settled, nested);
    if (runtime.then(source, continuation, callback, null) === null) return 821;
    if (!sourcePromise.resolve(7)) return 822;
    if (runtime.run() != 0) return 823;
    /* run() from inside a dispatched callback is refused, not recursed. */
    if (callback.runResult != -1) return 824;
    const nestedChild = callback.nestedChild;
    if (nestedChild === null) return 825;
    /* A continuation registered from inside a callback still settles. */
    if (!nestedChild.poll(status, value)) return 826;
    if (value.value != 10) return 827;

    /* A DEAD coroutine record may be reused; asyncStart accepts New or Dead. */
    const respawnRuntime = new Runtime();
    const coroutine = new Coroutine();
    const runs = new Box();
    if (respawnRuntime.asyncStart(coroutine, new RespawnTask(runs)) === null) return 830;
    if (respawnRuntime.asyncStart(coroutine, new RespawnTask(runs)) !== null) return 831;
    if (respawnRuntime.run() != 1) return 832;
    if (runs.value != 1) return 833;
    if (coroutine.state != CoroutineState.Dead) return 834;
    if (respawnRuntime.asyncStart(coroutine, new RespawnTask(runs)) === null) return 835;
    if (respawnRuntime.run() != 1) return 836;
    if (runs.value != 2) return 837;
    return 0;
}

/* Race with N candidates that ALL settle, run once per winning position.
 *
 * Every suite tested exactly one ordering (the second of three winning), so
 * nothing pinned that the result is independent of WHICH position wins. The
 * implementation is position-agnostic today; these cases are what keeps it
 * that way when someone adds an index-sensitive shortcut.
 *
 * A caveat, established by trying to break it rather than assumed: this does
 * NOT distinguish the head/middle/tail branches of the queue removal. A mutant
 * that only ever removes the head still passes, because continuationCancel
 * nulls the node's `next` regardless -- truncating the list anyway -- and nulls
 * its `future`, so a stale entry that survives is dispatched as a no-op. The
 * removal branches are not observable through the public API.
 *
 * What IS pinned: the first to settle wins from any position, and every loser
 * ends up inactive, off its source future, and off the dispatch queue. */
function raceWinnerCase(winner: i32, count: i32, base: i32): i32 {
    const runtime = new Runtime();
    const futures = new StaticArray<Future>(count);
    const promises = new StaticArray<Promise>(count);
    const continuations = new StaticArray<Continuation>(count);
    for (let i = 0; i < count; ++i) {
        futures[i] = new Future();
        promises[i] = new Promise();
        continuations[i] = new Continuation();
        futures[i].init(promises[i]);
    }
    const group = new FutureGroup();
    const result = runtime.raceInto(group, futures, continuations);
    if (result === null) return base + 0;

    const expected: usize = <usize>(100 + winner);
    /* Winner settles first, so it is dispatched first and wins; the rest settle
     * before the drain and queue behind it, which is what makes them losers that
     * were already enqueued rather than merely pending. Losers settle in
     * descending index order so the abandon order (ascending) differs from the
     * queue order. */
    if (!promises[winner].resolve(expected)) return base + 1;
    for (let i = count - 1; i >= 0; --i) {
        if (i == winner) continue;
        if (!promises[i].resolve(<usize>(900 + i))) return base + 2;
    }
    if (runtime.continuationHead === null) return base + 3;
    if (runtime.run() != 0) return base + 4;

    const status = new Box();
    const value = new Box();
    if (!result.poll(status, value)) return base + 5;
    if (<i32>status.value != 0) return base + 6;
    /* First to settle wins, whichever position it holds. */
    if (value.value != expected) return base + 7;
    if (!group.settled) return base + 8;
    if (group.active) return base + 9;
    /* The losers were DISCARDED, not merely outvoted: exactly one group callback
     * ran. Without this, a runtime that leaves them queued and lets them run --
     * their resolve refused because the group already settled -- looks identical
     * to one that cancels them. */
    if (group.completed != 1) return base + 55;

    for (let i = 0; i < count; ++i) {
        if (continuations[i].active) return base + 10 + i;
        if (futures[i].continuations !== null) return base + 20 + i;
    }
    /* The losers were removed from the dispatch queue, not merely ignored. */
    if (runtime.continuationHead !== null) return base + 30;
    if (runtime.continuationTail !== null) return base + 31;
    /* Nothing further is pending, and the result is unchanged. */
    if (runtime.run() != 0) return base + 32;
    if (!result.poll(status, value)) return base + 33;
    if (value.value != expected) return base + 34;
    return 0;
}

function testRaceEveryWinner(): i32 {
    let rc = raceWinnerCase(0, 3, 900);
    if (rc == 0) rc = raceWinnerCase(1, 3, 1000);
    if (rc == 0) rc = raceWinnerCase(2, 3, 1100);
    /* A fourth candidate, so more than two losers are abandoned at once. */
    if (rc == 0) rc = raceWinnerCase(1, 4, 1800);
    return rc;
}

/* The same three positions on the failure path: whichever source rejects
 * first settles the group and the other two must be discarded. */
function raceRejectCase(loser: i32, base: i32): i32 {
    const runtime = new Runtime();
    const futures = new StaticArray<Future>(3);
    const promises = new StaticArray<Promise>(3);
    const continuations = new StaticArray<Continuation>(3);
    for (let i = 0; i < 3; ++i) {
        futures[i] = new Future();
        promises[i] = new Promise();
        continuations[i] = new Continuation();
        futures[i].init(promises[i]);
    }
    const group = new FutureGroup();
    const result = runtime.raceInto(group, futures, continuations);
    if (result === null) return base + 0;

    const status = new Box();
    const value = new Box();
    if (!promises[loser].reject(-40 - loser)) return base + 1;
    if (runtime.run() != 0) return base + 2;
    if (!result.poll(status, value)) return base + 3;
    if (<i32>status.value != -40 - loser) return base + 4;
    if (!group.settled) return base + 5;
    if (group.active) return base + 6;
    if (group.completed != 1) return base + 55;
    for (let i = 0; i < 3; ++i) {
        if (continuations[i].active) return base + 10 + i;
        if (i != loser && futures[i].continuations !== null) return base + 20 + i;
    }
    if (runtime.continuationHead !== null) return base + 30;
    for (let i = 0; i < 3; ++i) {
        if (i == loser) continue;
        if (!promises[i].resolve(999)) return base + 40 + i;
    }
    if (runtime.run() != 0) return base + 50;
    if (!result.poll(status, value)) return base + 51;
    if (<i32>status.value != -40 - loser) return base + 52;
    return 0;
}

function testRaceEveryRejecter(): i32 {
    let rc = raceRejectCase(0, 1200);
    if (rc == 0) rc = raceRejectCase(1, 1300);
    if (rc == 0) rc = raceRejectCase(2, 1400);
    return rc;
}

/* all() fails fast, so the rejecting source's position decides the same three
 * removal branches for the survivors. */
function allRejectCase(rejecter: i32, base: i32): i32 {
    const runtime = new Runtime();
    const futures = new StaticArray<Future>(3);
    const promises = new StaticArray<Promise>(3);
    const continuations = new StaticArray<Continuation>(3);
    for (let i = 0; i < 3; ++i) {
        futures[i] = new Future();
        promises[i] = new Promise();
        continuations[i] = new Continuation();
        futures[i].init(promises[i]);
    }
    const group = new FutureGroup();
    const result = runtime.allInto(group, futures, continuations, new StaticArray<usize>(3));
    if (result === null) return base + 0;

    const status = new Box();
    const value = new Box();
    if (!promises[rejecter].reject(-60 - rejecter)) return base + 1;
    if (runtime.run() != 0) return base + 2;
    if (!result.poll(status, value)) return base + 3;
    if (<i32>status.value != -60 - rejecter) return base + 4;
    if (!group.settled) return base + 5;
    if (group.active) return base + 6;
    if (group.completed != 1) return base + 55;
    for (let i = 0; i < 3; ++i) {
        if (continuations[i].active) return base + 10 + i;
        if (i != rejecter && futures[i].continuations !== null) return base + 20 + i;
    }
    if (runtime.continuationHead !== null) return base + 30;
    for (let i = 0; i < 3; ++i) {
        if (i == rejecter) continue;
        if (!promises[i].resolve(999)) return base + 40 + i;
    }
    if (runtime.run() != 0) return base + 50;
    if (<i32>status.value != -60 - rejecter) return base + 51;
    return 0;
}

function testAllEveryRejecter(): i32 {
    let rc = allRejectCase(0, 1500);
    if (rc == 0) rc = allRejectCase(1, 1600);
    if (rc == 0) rc = allRejectCase(2, 1700);
    return rc;
}

/** 0 if every case passed, else the failing case's marker. */
export function runTests(): i32 {
    /* Randomized order: a case that leaks state must not be able to make
     * its neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    const cases: StaticArray<TestCase> = [
        testYieldAwaitAndJoin,
        testFutureChainsAndDeferredCallbacks,
        testRaceAndAll,
        testContracts,
        testMultipleWaiters,
        testMultipleJoiners,
        testSchedulerStress,
        testReentrancyRespawnAndPendingPoll,
        testRaceEveryWinner,
        testRaceEveryRejecter,
        testAllEveryRejecter,
    ];
    return runShuffled(cases);
}
