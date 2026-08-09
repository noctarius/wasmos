/* test_as_eventloop.ts — behaviour tests for the AssemblyScript event loop.
 *
 * The loop and its future bridge are the AS port of the C event loop in
 * wasmos/libsys.h and ipc_future_wasm.c, so the cases below are written against
 * what those do: replies route to the intent that asked for them, everything
 * else routes to a handler, and NOTHING is discarded. That last part is the
 * whole point -- the primitive this replaces (ipc.call in wasmos.ts) silently
 * dropped every message that was not the reply it was waiting for.
 *
 * The host harness (tests/unit/as/run_as_test.mjs) provides a fake IPC fabric:
 * per-endpoint queues plus a last-received slot. The `harness` imports below
 * are its control surface, so each case scripts the peer behaviour it needs.
 *
 * Each case returns 0 or a distinct marker; runTests() returns the first
 * non-zero one, which the harness prints.
 */

import {EventLoop, IpcFuture, IpcMessage, OnMessage, ReplyStatus} from "./eventloop";
import {Box, Coroutine, FutureState, Runtime, Task, TASK_COMPLETE, TASK_YIELDED} from "./coroutine";
import {runShuffled, TestCase} from "./shuffle";


@external("harness", "plant")
declare function plant(
    endpoint: i32,
    type: i32,
    requestId: i32,
    source: i32,
    a0: i32,
    a1: i32,
    a2: i32,
    a3: i32,
): i32;


@external("harness", "plantOnWait")
declare function plantOnWait(
    endpoint: i32,
    type: i32,
    requestId: i32,
    source: i32,
    a0: i32,
    a1: i32,
    a2: i32,
    a3: i32,
): i32;


@external("harness", "failSendsTo") declare function failSendsTo(endpoint: i32): i32;


@external("harness", "pending") declare function pending(endpoint: i32): i32;


@external("harness", "peek")
declare function peek(endpoint: i32, index: i32, field: i32): i32;


@external("harness", "waitCount") declare function waitCount(): i32;


@external("harness", "timeoutWaitCount") declare function timeoutWaitCount(): i32;


@external("harness", "lastTimeoutMs") declare function lastTimeoutMs(): i32;


@external("harness", "breakSelect") declare function breakSelect(broken: i32): i32;


@external("harness", "sendCount") declare function sendCount(): i32;


@external("harness", "reset") declare function reset(): i32;

const SELF: i32 = 10;
const PEER: i32 = 20;

const TYPE_REQ: i32 = 0x100;
const TYPE_RESP: i32 = 0x180;
const TYPE_NOTIFY: i32 = 0x200;
const TYPE_OTHER: i32 = 0x300;

/** Records what it was handed, so a case can assert routing and payload. */
class Recorder extends OnMessage {
    calls: i32 = 0;
    lastType: i32 = 0;
    lastRequestId: i32 = 0;
    lastArg0: i32 = 0;
    lastSource: i32 = 0;

    call(msg: IpcMessage): void {
        this.calls++;
        this.lastType = msg.type;
        this.lastRequestId = msg.requestId;
        this.lastArg0 = msg.arg0;
        this.lastSource = msg.source;
    }
}

/** Accepts only TYPE_RESP; anything else rejects with `status`. */
class ExpectResp extends ReplyStatus {
    constructor(public status: i32) {
        super();
    }

    call(reply: IpcMessage): i32 {
        return reply.type == TYPE_RESP ? 0 : this.status;
    }
}

function freshLoop(): EventLoop {
    reset();
    const loop = new EventLoop();
    loop.init(SELF, 1);
    return loop;
}

// ---------------------------------------------------------------- case 1

/**
 * The defining behaviour: a message that is not the awaited reply is handled,
 * not dropped. Planted BEFORE the reply so the loop must get past it.
 */
function testNothingIsDiscarded(): i32 {
    const loop = freshLoop();
    const notify = new Recorder();
    const fallback = new Recorder();
    loop.register(TYPE_NOTIFY, notify);
    loop.setDefault(fallback);

    const reply = new Recorder();
    const requestId = loop.intentSend(PEER, SELF, TYPE_REQ, 1, 2, 3, 4, reply);
    if (requestId != 1) return 101;
    if (pending(PEER) != 1) return 102;
    if (peek(PEER, 0, 0) != TYPE_REQ) return 103;
    if (peek(PEER, 0, 1) != requestId) return 104;

    plant(SELF, TYPE_NOTIFY, 0, PEER, 55, 0, 0, 0);
    plant(SELF, TYPE_OTHER, 0, PEER, 66, 0, 0, 0);
    plant(SELF, TYPE_RESP, requestId, PEER, 77, 0, 0, 0);

    if (loop.poll(8) != 3) return 105;
    if (notify.calls != 1 || notify.lastArg0 != 55) return 106;
    if (fallback.calls != 1 || fallback.lastArg0 != 66) return 107;
    if (reply.calls != 1 || reply.lastArg0 != 77) return 108;
    if (reply.lastRequestId != requestId || reply.lastSource != PEER) return 109;

    /* A reply whose request id matches no live intent is still delivered. */
    plant(SELF, TYPE_RESP, 4242, PEER, 88, 0, 0, 0);
    if (loop.poll(2) != 1) return 110;
    if (fallback.calls != 2 || fallback.lastArg0 != 88) return 111;
    return 0;
}

// ---------------------------------------------------------------- case 2

/** An intent claims its reply ahead of a typed handler for the same type. */
function testIntentBeatsHandler(): i32 {
    const loop = freshLoop();
    const typed = new Recorder();
    loop.register(TYPE_RESP, typed);

    const reply = new Recorder();
    const requestId = loop.intentSend(PEER, SELF, TYPE_REQ, 0, 0, 0, 0, reply);
    plant(SELF, TYPE_RESP, requestId, PEER, 9, 0, 0, 0);
    if (loop.poll(2) != 1) return 201;
    if (reply.calls != 1 || typed.calls != 0) return 202;

    /* Intent consumed: the same type now reaches the typed handler. */
    plant(SELF, TYPE_RESP, requestId, PEER, 10, 0, 0, 0);
    if (loop.poll(2) != 1) return 203;
    if (reply.calls != 1 || typed.calls != 1) return 204;

    /* Re-registering a type replaces the handler rather than adding one. */
    const replacement = new Recorder();
    if (loop.register(TYPE_RESP, replacement) != 0) return 205;
    plant(SELF, TYPE_RESP, 0, PEER, 11, 0, 0, 0);
    if (loop.poll(2) != 1) return 206;
    if (typed.calls != 1 || replacement.calls != 1) return 207;
    return 0;
}

// ---------------------------------------------------------------- case 3

/** Intent table: ids increase from the base, the table is finite, and a
 * failed send releases the slot it had claimed rather than leaking it. */
function testIntentTable(): i32 {
    const loop = freshLoop();
    const sink = new Recorder();

    const first = loop.intentSend(PEER, SELF, TYPE_REQ, 0, 0, 0, 0, sink);
    const second = loop.intentSend(PEER, SELF, TYPE_REQ, 0, 0, 0, 0, sink);
    if (first != 1 || second != 2) return 301;

    /* Fill the remaining slots, then confirm the next send is refused. */
    for (let i = 2; i < 16; ++i) {
        if (loop.intentSend(PEER, SELF, TYPE_REQ, 0, 0, 0, 0, sink) < 0) return 302;
    }
    const sends = sendCount();
    if (loop.intentSend(PEER, SELF, TYPE_REQ, 0, 0, 0, 0, sink) >= 0) return 303;
    /* Refused before sending: an intent with nowhere to record its reply must
     * not put a request on the wire. */
    if (sendCount() != sends) return 304;

    /* Freeing one slot lets the next send through. */
    loop.intentCancel(first);
    if (loop.intentSend(PEER, SELF, TYPE_REQ, 0, 0, 0, 0, sink) < 0) return 305;

    /* A send that fails at the transport must free its slot too, otherwise a
     * flaky peer would exhaust the table. */
    const drained = freshLoop();
    failSendsTo(PEER);
    for (let i = 0; i < 20; ++i) {
        if (drained.intentSend(PEER, SELF, TYPE_REQ, 0, 0, 0, 0, sink) >= 0) return 306;
    }
    failSendsTo(-1);
    if (drained.intentSend(PEER, SELF, TYPE_REQ, 0, 0, 0, 0, sink) < 0) return 307;
    return 0;
}

// ---------------------------------------------------------------- case 4

/** A cancelled intent's late reply is dispatched as an ordinary message; the
 * dead callback is never invoked. */
function testCancelAndExplicitRequestIds(): i32 {
    const loop = freshLoop();
    const reply = new Recorder();
    const fallback = new Recorder();
    loop.setDefault(fallback);

    const requestId = loop.intentSend(PEER, SELF, TYPE_REQ, 0, 0, 0, 0, reply);
    loop.intentCancel(requestId);
    plant(SELF, TYPE_RESP, requestId, PEER, 5, 0, 0, 0);
    if (loop.poll(2) != 1) return 401;
    if (reply.calls != 0) return 402;
    if (fallback.calls != 1 || fallback.lastArg0 != 5) return 403;

    /* Cancelling an id that is not live is a no-op, not a corruption. */
    loop.intentCancel(requestId);
    loop.intentCancel(0);
    loop.intentCancel(-7);

    /* Protocol-dictated request ids: a live id is refused rather than shadowed,
     * because two intents on one id makes the reply route ambiguous. */
    if (loop.intentSendWithRequestId(PEER, SELF, 900, TYPE_REQ, 0, 0, 0, 0, reply) != 0) return 404;
    if (loop.intentSendWithRequestId(PEER, SELF, 900, TYPE_REQ, 0, 0, 0, 0, reply) == 0) return 405;
    if (loop.intentSendWithRequestId(PEER, SELF, 0, TYPE_REQ, 0, 0, 0, 0, reply) == 0) return 406;
    plant(SELF, TYPE_RESP, 900, PEER, 6, 0, 0, 0);
    if (loop.poll(2) != 1) return 407;
    if (reply.calls != 1 || reply.lastArg0 != 6) return 408;
    return 0;
}

// ---------------------------------------------------------------- case 5

/** Releasing the intent slot BEFORE running its callback is what lets a
 * callback start the next request from a full table. */
function testSlotFreedBeforeCallback(): i32 {
    const loop = freshLoop();
    const sink = new Recorder();
    const first = loop.intentSend(PEER, SELF, TYPE_REQ, 0, 0, 0, 0, sink);
    for (let i = 1; i < 16; ++i) {
        if (loop.intentSend(PEER, SELF, TYPE_REQ, 0, 0, 0, 0, sink) < 0) return 501;
    }
    const chained = new ChainingHandler(loop, sink);
    /* Re-target the first intent's callback by cancelling and re-issuing it
     * with the chaining handler on the freed slot. */
    loop.intentCancel(first);
    const reissued = loop.intentSend(PEER, SELF, TYPE_REQ, 0, 0, 0, 0, chained);
    if (reissued < 0) return 502;
    plant(SELF, TYPE_RESP, reissued, PEER, 0, 0, 0, 0);
    if (loop.poll(2) != 1) return 503;
    if (chained.calls != 1) return 504;
    if (chained.followUp < 0) return 505;
    return 0;
}

class ChainingHandler extends OnMessage {
    calls: i32 = 0;
    followUp: i32 = -1;

    constructor(
        private loop: EventLoop,
        private sink: OnMessage,
    ) {
        super();
    }

    call(msg: IpcMessage): void {
        this.calls++;
        this.followUp = this.loop.intentSend(PEER, SELF, TYPE_REQ, 0, 0, 0, 0, this.sink);
    }
}

// ---------------------------------------------------------------- case 6

/** The future bridge: resolve, reject via ReplyStatus, and reuse. */
function testIpcFuture(): i32 {
    const loop = freshLoop();
    const operation = new IpcFuture(new ExpectResp(-42));

    const future = operation.send(loop, PEER, SELF, TYPE_REQ, 7, 0, 0, 0);
    if (future === null) return 601;
    if (future.state != FutureState.Pending) return 602;
    if (!operation.active) return 603;
    /* In flight: a second send on the same record is refused rather than
     * overwriting the request id the first reply will arrive on. */
    if (operation.send(loop, PEER, SELF, TYPE_REQ, 0, 0, 0, 0) !== null) return 604;

    plant(SELF, TYPE_RESP, operation.requestId, PEER, 33, 44, 0, 0);
    if (loop.poll(2) != 1) return 605;
    if (future.state != FutureState.Ready) return 606;
    if (operation.active) return 607;
    if (operation.reply.arg0 != 33 || operation.reply.arg1 != 44) return 608;

    /* The reply was COPIED, not aliased to the loop's shared record: later
     * traffic must not rewrite what a settled future already reported. */
    plant(SELF, TYPE_OTHER, 0, PEER, 999, 999, 0, 0);
    loop.setDefault(new Recorder());
    if (loop.poll(2) != 1) return 609;
    if (operation.reply.arg0 != 33 || operation.reply.arg1 != 44) return 610;

    /* Re-arm and go again; the record is reusable by design because a driver
     * cannot allocate one per round trip under a bump allocator. */
    operation.init(new ExpectResp(-42));
    const second = operation.send(loop, PEER, SELF, TYPE_REQ, 8, 0, 0, 0);
    if (second === null || second.state != FutureState.Pending) return 611;
    plant(SELF, TYPE_RESP, operation.requestId, PEER, 21, 0, 0, 0);
    if (loop.poll(2) != 1) return 612;
    if (second.state != FutureState.Ready || operation.reply.arg0 != 21) return 613;

    /* A reply of the wrong type rejects with the validator's status instead of
     * resolving with a message the caller's state machine cannot read. */
    operation.init(new ExpectResp(-42));
    const third = operation.send(loop, PEER, SELF, TYPE_REQ, 0, 0, 0, 0);
    if (third === null) return 614;
    plant(SELF, TYPE_NOTIFY, operation.requestId, PEER, 0, 0, 0, 0);
    if (loop.poll(2) != 1) return 615;
    if (third.state != FutureState.Failed || third.status != -42) return 616;

    /* No validator accepts whatever comes back. */
    const permissive = new IpcFuture(null);
    const fourth = permissive.send(loop, PEER, SELF, TYPE_REQ, 0, 0, 0, 0);
    if (fourth === null) return 617;
    plant(SELF, TYPE_NOTIFY, permissive.requestId, PEER, 0, 0, 0, 0);
    if (loop.poll(2) != 1) return 618;
    if (fourth.state != FutureState.Ready) return 619;
    return 0;
}

// ---------------------------------------------------------------- case 7

/** A send that cannot reach the wire still settles its future, so a caller
 * that chained onto it learns of the failure through the same path. */
function testIpcFutureFailureAndCancel(): i32 {
    const loop = freshLoop();
    failSendsTo(PEER);
    const operation = new IpcFuture(null);
    const future = operation.send(loop, PEER, SELF, TYPE_REQ, 0, 0, 0, 0);
    if (future === null) return 701;
    if (future.state != FutureState.Failed) return 702;
    if (operation.active) return 703;
    failSendsTo(-1);

    /* Cancel rejects with the caller's status. */
    const cancelled = new IpcFuture(null);
    const pendingFuture = cancelled.send(loop, PEER, SELF, TYPE_REQ, 0, 0, 0, 0);
    if (pendingFuture === null || pendingFuture.state != FutureState.Pending) return 704;
    const requestId = cancelled.requestId;
    cancelled.cancel(-55);
    if (pendingFuture.state != FutureState.Failed || pendingFuture.status != -55) return 705;
    if (cancelled.active) return 706;

    /* A late reply to a cancelled request cannot re-settle the future; it is
     * delivered as an ordinary message. */
    const fallback = new Recorder();
    loop.setDefault(fallback);
    plant(SELF, TYPE_RESP, requestId, PEER, 12, 0, 0, 0);
    if (loop.poll(2) != 1) return 707;
    if (pendingFuture.status != -55) return 708;
    if (fallback.calls != 1) return 709;

    /* Cancelling an idle record is a no-op. */
    cancelled.cancel(-1);
    if (pendingFuture.status != -55) return 710;
    return 0;
}

// ---------------------------------------------------------------- case 8

/** Idle means block, never spin: one wait per poll, not one per budget unit. */
function testPollBlocksInsteadOfSpinning(): i32 {
    const loop = freshLoop();
    const fallback = new Recorder();
    loop.setDefault(fallback);

    if (loop.poll(8) != 0) return 801;
    if (waitCount() != 1) return 802;

    /* The message that arrives while blocked is handled on that same poll. */
    plantOnWait(SELF, TYPE_OTHER, 0, PEER, 31, 0, 0, 0);
    if (loop.poll(8) != 1) return 803;
    if (waitCount() != 2) return 804;
    if (fallback.calls != 1 || fallback.lastArg0 != 31) return 805;

    /* With work already queued the loop does not block at all, and stops at
     * the budget rather than draining everything. */
    for (let i = 0; i < 4; ++i) plant(SELF, TYPE_OTHER, 0, PEER, 40 + i, 0, 0, 0);
    if (loop.poll(2) != 2) return 806;
    if (waitCount() != 2) return 807;
    if (fallback.calls != 3) return 808;
    /* budget 0 means one message, matching the C loop. */
    if (loop.poll(0) != 1) return 809;
    if (fallback.calls != 4) return 810;
    return 0;
}

// ---------------------------------------------------------------- case 9

/** A handler that re-enters poll is refused rather than allowed to overwrite
 * the message record its own caller is still dispatching. */
function testReentrantPollIsRefused(): i32 {
    const loop = freshLoop();
    const reenter = new ReenteringHandler(loop);
    loop.setDefault(reenter);

    plant(SELF, TYPE_OTHER, 0, PEER, 61, 0, 0, 0);
    plant(SELF, TYPE_OTHER, 0, PEER, 62, 0, 0, 0);
    if (loop.poll(4) != 2) return 901;
    if (reenter.calls != 2) return 902;
    if (reenter.nestedHandled != 0) return 903;
    /* The outer dispatch still saw its own message after the nested attempt. */
    if (reenter.firstArg0 != 61 || reenter.secondArg0 != 62) return 904;
    return 0;
}

class ReenteringHandler extends OnMessage {
    calls: i32 = 0;
    nestedHandled: i32 = -1;
    firstArg0: i32 = 0;
    secondArg0: i32 = 0;

    constructor(private loop: EventLoop) {
        super();
    }

    call(msg: IpcMessage): void {
        const before = msg.arg0;
        const nested = this.loop.poll(4);
        if (this.nestedHandled < 0 || nested != 0) this.nestedHandled = nested;
        this.calls++;
        if (this.calls == 1) this.firstArg0 = before;
        else this.secondArg0 = before;
    }
}

// ---------------------------------------------------------------- case 10

/** init() re-arms a loop: stale intents and handlers from a previous life
 * must not survive, or a recycled request id would reach a dead callback. */
function testInitClearsState(): i32 {
    const loop = freshLoop();
    const stale = new Recorder();
    loop.register(TYPE_OTHER, stale);
    loop.setDefault(stale);
    const requestId = loop.intentSend(PEER, SELF, TYPE_REQ, 0, 0, 0, 0, stale);

    loop.init(SELF, 500);
    const fresh = new Recorder();
    loop.setDefault(fresh);
    plant(SELF, TYPE_RESP, requestId, PEER, 1, 0, 0, 0);
    plant(SELF, TYPE_OTHER, 0, PEER, 2, 0, 0, 0);
    if (loop.poll(4) != 2) return 1001;
    if (stale.calls != 0) return 1002;
    if (fresh.calls != 2) return 1003;
    /* Request ids restart at the new base. */
    if (loop.intentSend(PEER, SELF, TYPE_REQ, 0, 0, 0, 0, fresh) != 500) return 1004;

    /* A message deferred before the re-arm belongs to the loop's previous life
     * and must not be delivered to whoever receives next. */
    const deferring = freshLoop();
    plant(SELF, TYPE_OTHER, 0, PEER, 3, 0, 0, 0);
    if (deferring.poll(2) != 1) return 1005;
    deferring.init(SELF, 1);
    if (deferring.receive(5) !== null) return 1006;
    return 0;
}

// ---------------------------------------------------------------- case 11

/** A driver whose device is polled, not IPC-driven, still must not spin: the
 * idle wait is bounded instead of absent. */
function testPollTimeout(): i32 {
    const loop = freshLoop();
    const fallback = new Recorder();
    loop.setDefault(fallback);

    if (loop.pollTimeout(4, 25) != 0) return 1101;
    if (timeoutWaitCount() != 1) return 1102;
    if (lastTimeoutMs() != 25) return 1103;

    /* A timed wait that a message arrives during behaves like any other. */
    plantOnWait(SELF, TYPE_OTHER, 0, PEER, 71, 0, 0, 0);
    if (loop.pollTimeout(4, 25) != 1) return 1104;
    if (fallback.calls != 1 || fallback.lastArg0 != 71) return 1105;

    /* Work already queued is handled without any wait at all. */
    const timedWaits = timeoutWaitCount();
    plant(SELF, TYPE_OTHER, 0, PEER, 72, 0, 0, 0);
    if (loop.pollTimeout(4, 25) != 1) return 1106;
    if (timeoutWaitCount() != timedWaits) return 1107;

    /* poll() is the untimed case, and must not take the timed path. */
    if (loop.poll(2) != 0) return 1108;
    if (timeoutWaitCount() != timedWaits) return 1109;
    return 0;
}

// ---------------------------------------------------------------- case 12

/** awaitFuture drives startup steps: the reply settles the future while other
 * traffic is still handled, and a loop that cannot park refuses to spin. */
function testAwaitFuture(): i32 {
    const loop = freshLoop();
    const fallback = new Recorder();
    loop.setDefault(fallback);

    const operation = new IpcFuture(new ExpectResp(-9));
    const future = operation.send(loop, PEER, SELF, TYPE_REQ, 0, 0, 0, 0);
    if (future === null) return 1201;
    /* A client request racing in during the handshake is HANDLED, not dropped;
     * that is the whole reason this replaces a blocking recv. */
    plant(SELF, TYPE_NOTIFY, 0, PEER, 81, 0, 0, 0);
    plantOnWait(SELF, TYPE_RESP, operation.requestId, PEER, 82, 0, 0, 0);
    if (!loop.awaitFuture(future, 4)) return 1202;
    if (future.state != FutureState.Ready) return 1203;
    if (operation.reply.arg0 != 82) return 1204;
    if (fallback.calls != 1 || fallback.lastArg0 != 81) return 1205;

    /* An already-settled future returns immediately without polling. */
    const waits = waitCount();
    if (!loop.awaitFuture(future, 4)) return 1206;
    if (waitCount() != waits) return 1207;

    /* No select set means no way to park. Rather than burn the CPU until the
     * reply happens to arrive, awaitFuture reports that it cannot wait. */
    reset();
    breakSelect(1);
    const blind = new EventLoop();
    blind.init(SELF, 1);
    if (blind.canBlock()) return 1208;
    const orphan = new IpcFuture(null);
    const orphanFuture = orphan.send(blind, PEER, SELF, TYPE_REQ, 0, 0, 0, 0);
    if (orphanFuture === null) return 1209;
    if (blind.awaitFuture(orphanFuture, 4)) return 1210;
    if (orphanFuture.state != FutureState.Pending) return 1211;
    breakSelect(0);
    return 0;
}

// ---------------------------------------------------------------- case 13

/** receive() is the sequential face of the loop: it returns what nothing else
 * claimed, and demultiplexes everything else out of the way first. */
function testReceive(): i32 {
    const loop = freshLoop();
    const typed = new Recorder();
    loop.register(TYPE_NOTIFY, typed);

    const operation = new IpcFuture(new ExpectResp(-1));
    const future = operation.send(loop, PEER, SELF, TYPE_REQ, 0, 0, 0, 0);
    if (future === null) return 1301;

    /* A reply, a handled type and two unclaimed messages, interleaved. */
    plant(SELF, TYPE_OTHER, 0, PEER, 91, 0, 0, 0);
    plant(SELF, TYPE_RESP, operation.requestId, PEER, 92, 0, 0, 0);
    plant(SELF, TYPE_NOTIFY, 0, PEER, 93, 0, 0, 0);
    plant(SELF, TYPE_OTHER, 0, PEER, 94, 0, 0, 0);

    /* Only the unclaimed ones come back, oldest first. */
    const first = loop.receive(0);
    if (first === null || first.arg0 != 91) return 1302;
    const second = loop.receive(0);
    if (second === null || second.arg0 != 94) return 1303;
    /* The reply settled its future and the handled type ran, without either
     * being returned here. */
    if (future.state != FutureState.Ready || operation.reply.arg0 != 92) return 1304;
    if (typed.calls != 1 || typed.lastArg0 != 93) return 1305;

    /* Nothing left: a bounded wait gives up and says so. */
    if (loop.receive(5) !== null) return 1306;
    if (lastTimeoutMs() != 5) return 1307;

    /* A message arriving while parked is returned by that same call. */
    plantOnWait(SELF, TYPE_OTHER, 0, PEER, 95, 0, 0, 0);
    const woken = loop.receive(0);
    if (woken === null || woken.arg0 != 95) return 1308;

    /* A default handler consumes what receive() would have returned; the two
     * are the same slot, and an indefinite receive then cannot make progress. */
    const fallback = new Recorder();
    loop.setDefault(fallback);
    plant(SELF, TYPE_OTHER, 0, PEER, 96, 0, 0, 0);
    if (loop.receive(5) !== null) return 1309;
    if (fallback.calls != 1 || fallback.lastArg0 != 96) return 1310;
    return 0;
}

// ---------------------------------------------------------------- case 14

/** With no handler and nowhere to put it, a message waits in the kernel queue
 * rather than being dropped: backpressure, not data loss. */
function testDeferralBackpressure(): i32 {
    const loop = freshLoop();
    const total = 12; /* deliberately more than DEFERRED_MAX */
    for (let i = 0; i < total; ++i) plant(SELF, TYPE_OTHER, 0, PEER, 200 + i, 0, 0, 0);

    /* A generous budget still stops at the ring's capacity. */
    const handled = loop.poll(32);
    if (handled != 8) return 1401;
    if (pending(SELF) != total - 8) return 1402;

    /* Draining the ring lets the rest through, in order and complete. */
    for (let i = 0; i < total; ++i) {
        const msg = loop.receive(0);
        if (msg === null) return 1403 + i * 0;
        if (msg.arg0 != 200 + i) return 1450 + i;
    }
    if (pending(SELF) != 0) return 1404;
    if (loop.receive(5) !== null) return 1405;
    return 0;
}

// ---------------------------------------------------------------- case 15

/** A loop that cannot park must not spin in receive() either. */
function testReceiveCannotPark(): i32 {
    reset();
    breakSelect(1);
    const blind = new EventLoop();
    blind.init(SELF, 1);
    if (blind.canBlock()) return 1501;

    /* Queued work is still returned without needing to park. */
    plant(SELF, TYPE_OTHER, 0, PEER, 77, 0, 0, 0);
    const msg = blind.receive(0);
    if (msg === null || msg.arg0 != 77) return 1502;
    /* With nothing queued it reports that it cannot wait, rather than looping. */
    if (blind.receive(0) !== null) return 1503;
    if (waitCount() != 0) return 1504;
    breakSelect(0);
    return 0;
}

// ---------------------------------------------------------------- case 16

/** armMessage() is receive() for a coroutine: a future settled by the next
 * message nothing else claims. */
function testMessageFuture(): i32 {
    const loop = freshLoop();
    const typed = new Recorder();
    loop.register(TYPE_NOTIFY, typed);

    const first = loop.nextMessage();
    if (first.state != FutureState.Pending) return 1601;
    /* Arming twice hands back the same pending future rather than dropping the
     * first, which would strand whoever awaits it. */
    if (loop.nextMessage() !== first) return 1602;

    /* A handled type runs its handler and does NOT settle the future. */
    plant(SELF, TYPE_NOTIFY, 0, PEER, 11, 0, 0, 0);
    if (loop.poll(2) != 1) return 1603;
    if (typed.calls != 1) return 1604;
    if (first.state != FutureState.Pending) return 1605;

    /* An unclaimed one settles it, carrying the message. */
    plant(SELF, TYPE_OTHER, 0, PEER, 12, 0, 0, 0);
    if (loop.poll(2) != 1) return 1606;
    if (first.state != FutureState.Ready) return 1607;
    if (loop.messageRecord.arg0 != 12) return 1608;

    /* A settled future takes nothing further; the next message defers instead. */
    plant(SELF, TYPE_OTHER, 0, PEER, 13, 0, 0, 0);
    if (loop.poll(2) != 1) return 1609;
    if (loop.messageRecord.arg0 != 12) return 1610;

    /* Re-arming picks up what was deferred meanwhile, so switching between the
     * two styles loses nothing. */
    loop.rearmMessage();
    const second = loop.nextMessage();
    if (second.state != FutureState.Ready) return 1611;
    if (loop.messageRecord.arg0 != 13) return 1612;

    /* An intent still wins: a reply settles its own future, not this one. */
    loop.rearmMessage();
    const third = loop.nextMessage();
    if (third.state != FutureState.Pending) return 1613;
    const operation = new IpcFuture(null);
    const reply = operation.send(loop, PEER, SELF, TYPE_REQ, 0, 0, 0, 0);
    if (reply === null) return 1614;
    plant(SELF, TYPE_RESP, operation.requestId, PEER, 14, 0, 0, 0);
    if (loop.poll(2) != 1) return 1615;
    if (reply.state != FutureState.Ready) return 1616;
    if (third.state != FutureState.Pending) return 1617;
    return 0;
}

// ---------------------------------------------------------------- case 17

/** A coroutine parked on the message future must survive a second armMessage.
 * Re-initialising an armed future clears its wait list, stranding the waiter --
 * invisible unless something is actually parked on it. */
class AwaitingTask extends Task {
    resumes: i32 = 0;
    arg0: i32 = -1;

    constructor(private loop: EventLoop) {
        super();
    }

    resume(out: Box): i32 {
        this.resumes++;
        const status = this.loop.nextMessage().await(out);
        if (status == 1 /* AWAIT_PENDING */) return TASK_YIELDED;
        this.arg0 = this.loop.messageRecord.arg0;
        this.loop.rearmMessage();
        out.value = 0;
        return TASK_COMPLETE;
    }
}

function testArmingTwiceKeepsTheWaiter(): i32 {
    const loop = freshLoop();
    const runtime = new Runtime();
    const coroutine = new Coroutine();
    const task = new AwaitingTask(loop);
    if (runtime.asyncStart(coroutine, task) === null) return 1701;

    /* Parks on the message future. */
    if (runtime.run() != 1) return 1702;
    if (task.resumes != 1) return 1703;

    /* A second arm must be a no-op while one is outstanding. */
    loop.nextMessage();

    plant(SELF, TYPE_OTHER, 0, PEER, 21, 0, 0, 0);
    if (loop.poll(2) != 1) return 1704;
    /* The waiter is still on the list, so the runtime resumes it. */
    if (runtime.run() != 1) return 1705;
    if (task.resumes != 2) return 1706;
    if (task.arg0 != 21) return 1707;
    return 0;
}

/** 0 if every case passed, else the failing case's marker. */
export function runTests(): i32 {
    /* Randomized order: a case that leaks state must not be able to make
     * its neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    const cases: StaticArray<TestCase> = [
        testNothingIsDiscarded,
        testIntentBeatsHandler,
        testIntentTable,
        testCancelAndExplicitRequestIds,
        testSlotFreedBeforeCallback,
        testIpcFuture,
        testIpcFutureFailureAndCancel,
        testPollBlocksInsteadOfSpinning,
        testReentrantPollIsRefused,
        testInitClearsState,
        testPollTimeout,
        testAwaitFuture,
        testReceive,
        testDeferralBackpressure,
        testReceiveCannotPark,
        testMessageFuture,
        testArmingTwiceKeepsTheWaiter,
    ];
    return runShuffled(cases);
}
