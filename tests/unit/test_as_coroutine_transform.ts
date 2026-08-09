/* test_as_coroutine_transform.ts — behaviour of transform-generated coroutines.
 *
 * tools/as_coroutine_transform.mjs lowers a @coroutine function into a Task
 * whose resume() is a pc state machine. What has to be proven is not that it
 * emits plausible code but that the machine BEHAVES: that it parks exactly
 * where the suspension is, resumes with the settled value, keeps its locals
 * across the suspension, and does all of that when the suspension sits inside
 * a loop -- which is the case a naive linear split gets wrong.
 *
 * Every case pairs the lowered coroutine against the same logic hand-written as
 * a Task, so a divergence is a transform bug rather than a change of intent.
 *
 * Each case returns 0 or a distinct marker; the order is randomized (see
 * tests/unit/as/shuffle.ts).
 */

import {
    Box,
    Coroutine,
    Future,
    Promise,
    Runtime,
    Task,
    AWAIT_PENDING,
    TASK_COMPLETE,
    TASK_YIELDED,
} from "./coroutine";
import {runShuffled, TestCase} from "./shuffle";

/** The suspension primitive: the contract @suspend names for the compiler. */
@suspend
function awaitValue(future: Future, out: Box): i32 {
    return future.await(out);
}

/**
 * A channel delivering a series of values on one reusable future.
 *
 * Re-arming belongs to the AWAITING side, and that is a property of the
 * lowering rather than a convenience: resuming re-runs the suspension call, so
 * the future must still be settled at that moment. Re-arming it from outside
 * before the coroutine resumes would lose the value. Taking it and re-arming in
 * the same call is exactly what IpcFuture does per round trip.
 */
class Channel {
    future: Future = new Future();
    promise: Promise = new Promise();

    constructor() {
        this.future.init(this.promise);
    }

    send(value: i32): void {
        this.promise.resolve(<usize>value);
    }
}


@suspend
function receive(channel: Channel, out: Box): i32 {
    const status = channel.future.await(out);
    if (status != AWAIT_PENDING) {
        channel.future.init(channel.promise); /* consumed; ready for the next */
    }
    return status;
}

// ------------------------------------------------------------------ subjects

/** Two suspensions in sequence, with a local live across both. */
@coroutine
function sumTwo(a: Future, b: Future): i32 {
    let x: i32 = awaitValue(a);
    let y: i32 = awaitValue(b);
    return x + y;
}

/** A suspension INSIDE a loop: the accumulator and the induction variable must
 * both survive every trip through the state machine. */
@coroutine
function sumAll(channel: Channel, rounds: i32): i32 {
    let total: i32 = 0;
    for (let i: i32 = 0; i < rounds; ++i) {
        let v: i32 = receive(channel);
        total += v;
    }
    return total;
}

/** Control flow around a suspension: break, continue and an early return. */
@coroutine
function untilSentinel(channel: Channel, limit: i32): i32 {
    let seen: i32 = 0;
    while (true) {
        let v: i32 = receive(channel);
        if (v == 0) {
            break;
        }
        if (v == 1) {
            continue;
        }
        seen += v;
        if (seen >= limit) {
            return seen;
        }
    }
    return -seen;
}

/**
 * break and continue inside a FOR loop, which differ from a while loop: a
 * continue must jump to the incrementor, not the condition, or the induction
 * variable never advances.
 */
@coroutine
function collectEven(channel: Channel, rounds: i32): i32 {
    let total: i32 = 0;
    for (let i: i32 = 0; i < rounds; ++i) {
        let v: i32 = receive(channel);
        if (v < 0) {
            break;
        }
        if (v % 2 == 1) {
            continue;
        }
        total += v;
    }
    return total;
}

/**
 * A local whose name also names a PROPERTY somewhere in the body. Hoisting
 * rewrites references to the local as fields, and it must not touch the
 * property: `holder.value` is not a reference to a local called `value`.
 */
class Holder {
    value: i32 = 0;

    constructor(value: i32) {
        this.value = value;
    }
}


@coroutine
function shadowsAProperty(channel: Channel, holder: Holder): i32 {
    let value: i32 = receive(channel);
    return value + holder.value;
}

/**
 * A suspension whose result is a REFERENCE, not a number. The value comes back
 * through a Box, which carries a usize, so the lowering has to reinterpret it
 * rather than numerically cast it -- the two are not interchangeable in asc.
 * This is what a driver awaiting a message record does.
 */
class Payload {
    n: i32 = 0;

    constructor(n: i32) {
        this.n = n;
    }
}

class PayloadChannel {
    future: Future = new Future();
    promise: Promise = new Promise();

    constructor() {
        this.future.init(this.promise);
    }

    send(payload: Payload): void {
        this.promise.resolve(changetype<usize>(payload));
    }
}


@suspend
function receivePayload(channel: PayloadChannel, out: Box): i32 {
    const status = channel.future.await(out);
    if (status != AWAIT_PENDING) {
        channel.future.init(channel.promise);
    }
    return status;
}


@coroutine
function sumPayloads(channel: PayloadChannel, rounds: i32): i32 {
    let total: i32 = 0;
    for (let i: i32 = 0; i < rounds; ++i) {
        let payload: Payload = receivePayload(channel);
        total += payload.n;
    }
    return total;
}

/** No suspension at all still has to work. */
@coroutine
function noSuspension(a: i32): i32 {
    let total: i32 = 0;
    for (let i: i32 = 0; i < a; ++i) {
        total += i;
    }
    return total;
}

// ------------------------------------------------------------- hand-written

/** sumTwo, written by hand: the shape the transform is replacing. */
class SumTwoByHand extends Task {
    pc: i32 = 0;
    x: i32 = 0;
    y: i32 = 0;
    box: Box = new Box();

    constructor(
        public a: Future,
        public b: Future,
    ) {
        super();
    }

    resume(out: Box): i32 {
        if (this.pc == 0) {
            const status = this.a.await(this.box);
            if (status == AWAIT_PENDING) return TASK_YIELDED;
            if (status != 0) return status;
            this.x = <i32>this.box.value;
            this.pc = 1;
        }
        const status = this.b.await(this.box);
        if (status == AWAIT_PENDING) return TASK_YIELDED;
        if (status != 0) return status;
        this.y = <i32>this.box.value;
        out.value = <usize>(this.x + this.y);
        return TASK_COMPLETE;
    }
}

// ------------------------------------------------------------------- helpers

class Pair {
    future: Future = new Future();
    promise: Promise = new Promise();

    constructor() {
        this.future.init(this.promise);
    }

    /** Re-arm for another round trip, as a reused future must be. */
    reset(): void {
        this.future.init(this.promise);
    }
}

// --------------------------------------------------------------------- cases

/** A suspension parks the coroutine and resumes with the settled value. */
function testSequentialSuspensions(): i32 {
    const runtime = new Runtime();
    const a = new Pair();
    const b = new Pair();
    const coroutine = new Coroutine();
    const completion = runtime.asyncStart(coroutine, sumTwo(a.future, b.future));
    if (completion === null) return 101;

    /* Runs until the first suspension, then parks: neither future is settled. */
    if (runtime.run() != 1) return 102;
    const status = new Box();
    if (completion.poll(status, null)) return 103;

    a.promise.resolve(10);
    if (runtime.run() != 1) return 104;
    if (completion.poll(status, null)) return 105;

    b.promise.resolve(32);
    if (runtime.run() != 1) return 106;
    const value = new Box();
    if (!completion.poll(status, value)) return 107;
    if (<i32>status.value != 0) return 108;
    if (<i32>value.value != 42) return 109;
    return 0;
}

/** The lowered coroutine and the hand-written Task agree step for step. */
function testMatchesTheHandWrittenTask(): i32 {
    const values = new StaticArray<i32>(2);
    const resumes = new StaticArray<i32>(2);
    for (let variant = 0; variant < 2; ++variant) {
        const runtime = new Runtime();
        const a = new Pair();
        const b = new Pair();
        const coroutine = new Coroutine();
        const task: Task =
            variant == 0
                ? <Task>sumTwo(a.future, b.future)
                : <Task>new SumTwoByHand(a.future, b.future);
        const completion = runtime.asyncStart(coroutine, task);
        if (completion === null) return 201;

        let count = 0;
        count += runtime.run();
        a.promise.resolve(7);
        count += runtime.run();
        b.promise.resolve(5);
        count += runtime.run();

        const value = new Box();
        if (!completion.poll(null, value)) return 202 + variant;
        values[variant] = <i32>value.value;
        resumes[variant] = count;
    }
    if (values[0] != values[1]) return 204;
    if (values[0] != 12) return 205;
    /* Same number of resumes, i.e. it parks in the same places. */
    if (resumes[0] != resumes[1]) return 206;
    return 0;
}

/** The hard case: a suspension inside a loop, with locals live across it. */
function testSuspensionInsideALoop(): i32 {
    const runtime = new Runtime();
    const channel = new Channel();
    const coroutine = new Coroutine();
    const completion = runtime.asyncStart(coroutine, sumAll(channel, 3));
    if (completion === null) return 301;

    let total = 0;
    for (let round = 0; round < 3; ++round) {
        if (runtime.run() != 1) return 310 + round;
        /* Still pending: it parked inside the loop body. */
        if (completion.poll(null, null)) return 320 + round;
        channel.send(round + 1);
        total += round + 1;
    }
    if (runtime.run() != 1) return 302;
    const value = new Box();
    if (!completion.poll(null, value)) return 303;
    /* 1 + 2 + 3: the accumulator survived all three suspensions. */
    if (<i32>value.value != total) return 304;
    return 0;
}

/** break, continue and an early return, all across a suspension. */
function testControlFlowAroundASuspension(): i32 {
    const runtime = new Runtime();
    const channel = new Channel();
    const coroutine = new Coroutine();
    const completion = runtime.asyncStart(coroutine, untilSentinel(channel, 10));
    if (completion === null) return 401;

    /* 1 continues, 4 and 6 accumulate to 10 and hit the early return. */
    const feed = [1, 4, 6];
    for (let i = 0; i < feed.length; ++i) {
        if (runtime.run() != 1) return 410 + i;
        channel.send(feed[i]);
    }
    if (runtime.run() != 1) return 402;
    const value = new Box();
    if (!completion.poll(null, value)) return 403;
    if (<i32>value.value != 10) return 404;
    return 0;
}

/** The break path, which the early return above never reaches. */
function testBreakPath(): i32 {
    const runtime = new Runtime();
    const channel = new Channel();
    const coroutine = new Coroutine();
    const completion = runtime.asyncStart(coroutine, untilSentinel(channel, 100));
    if (completion === null) return 501;

    if (runtime.run() != 1) return 502;
    channel.send(5);
    if (runtime.run() != 1) return 503;
    channel.send(0); /* the sentinel: break out */
    if (runtime.run() != 1) return 504;
    const value = new Box();
    if (!completion.poll(null, value)) return 505;
    /* Fell out of the loop, so the negated total. */
    if (<i32>value.value != -5) return 506;
    return 0;
}

/** A coroutine that never suspends completes in a single resume. */
function testNoSuspension(): i32 {
    const runtime = new Runtime();
    const coroutine = new Coroutine();
    const completion = runtime.asyncStart(coroutine, noSuspension(5));
    if (completion === null) return 601;
    if (runtime.run() != 1) return 602;
    const value = new Box();
    if (!completion.poll(null, value)) return 603;
    if (<i32>value.value != 10) return 604;
    return 0;
}

/** A rejected future fails the coroutine with that status. */
function testRejectionPropagates(): i32 {
    const runtime = new Runtime();
    const a = new Pair();
    const b = new Pair();
    const coroutine = new Coroutine();
    const completion = runtime.asyncStart(coroutine, sumTwo(a.future, b.future));
    if (completion === null) return 701;

    if (runtime.run() != 1) return 702;
    a.promise.reject(-77);
    if (runtime.run() != 1) return 703;
    const status = new Box();
    if (!completion.poll(status, null)) return 704;
    if (<i32>status.value != -77) return 705;
    return 0;
}

/** A for loop whose body both continues and breaks across a suspension. */
function testForLoopBreakAndContinue(): i32 {
    const runtime = new Runtime();
    const channel = new Channel();
    const coroutine = new Coroutine();
    const completion = runtime.asyncStart(coroutine, collectEven(channel, 4));
    if (completion === null) return 801;

    /* 3 is skipped by continue, which must still advance i; -1 breaks out. */
    const feed = [2, 3, 4, -1];
    for (let i = 0; i < feed.length; ++i) {
        if (runtime.run() != 1) return 810 + i;
        if (completion.poll(null, null)) return 820 + i;
        channel.send(feed[i]);
    }
    if (runtime.run() != 1) return 802;
    const value = new Box();
    if (!completion.poll(null, value)) return 803;
    if (<i32>value.value != 6) return 804;
    return 0;
}

/** A hoisted local must not be substituted into a property of the same name. */
function testLocalShadowingAProperty(): i32 {
    const runtime = new Runtime();
    const channel = new Channel();
    const coroutine = new Coroutine();
    const completion = runtime.asyncStart(coroutine, shadowsAProperty(channel, new Holder(30)));
    if (completion === null) return 901;

    if (runtime.run() != 1) return 902;
    channel.send(12);
    if (runtime.run() != 1) return 903;
    const value = new Box();
    if (!completion.poll(null, value)) return 904;
    if (<i32>value.value != 42) return 905;
    return 0;
}

/** A reference-typed result survives the box and the frame. */
function testReferenceResult(): i32 {
    const runtime = new Runtime();
    const channel = new PayloadChannel();
    const coroutine = new Coroutine();
    const completion = runtime.asyncStart(coroutine, sumPayloads(channel, 2));
    if (completion === null) return 1001;

    if (runtime.run() != 1) return 1002;
    channel.send(new Payload(30));
    if (runtime.run() != 1) return 1003;
    /* Parked again inside the loop, holding the running total on its frame. */
    if (completion.poll(null, null)) return 1004;
    channel.send(new Payload(12));
    if (runtime.run() != 1) return 1005;

    const value = new Box();
    if (!completion.poll(null, value)) return 1006;
    if (<i32>value.value != 42) return 1007;
    return 0;
}

export function runTests(): i32 {
    const cases: StaticArray<TestCase> = [
        testSequentialSuspensions,
        testMatchesTheHandWrittenTask,
        testSuspensionInsideALoop,
        testControlFlowAroundASuspension,
        testBreakPath,
        testForLoopBreakAndContinue,
        testLocalShadowingAProperty,
        testReferenceResult,
        testNoSuspension,
        testRejectionPropagates,
    ];
    return runShuffled(cases);
}
