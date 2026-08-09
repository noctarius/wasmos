/* eventloop.ts - non-blocking IPC demultiplexer and future bridge for AS guests.
 *
 * The AssemblyScript port of the C event loop in
 * src/libsys/wasm/include/wasmos/libsys.h and the future bridge in
 * src/libsys/wasm/ipc_future_wasm.c. Same reason as coroutine.ts: `asc` has no
 * external linking, so AS cannot compile those in and needs a port.
 *
 * What it replaces. Before this, an AS guest's only request/reply primitive was
 * ipc.call in wasmos.ts, which sends and then loops on ipc_recv until the
 * matching request id turns up -- SILENTLY DISCARDING every other message that
 * arrives meanwhile. For a driver that is data loss: a client's request that
 * lands during a registration handshake is dropped, and the client waits for a
 * reply that will never come. A loop demultiplexes instead: replies settle the
 * future that asked for them, everything else reaches a handler.
 *
 * Blocking. poll() blocks in ipc_select_wait when there is nothing to do, which
 * is a scheduler block, not a spin. It never calls sched_yield in a loop.
 *
 * Ownership, as in coroutine.ts: `--runtime stub` is a bump allocator with no
 * collector, so nothing here allocates per message or per request. Callers own
 * and REUSE their IpcFuture records.
 */

import {
    ipc_drain,
    ipc_last_field,
    ipc_select_add,
    ipc_select_create,
    ipc_select_destroy,
    ipc_select_wait,
    ipc_select_wait_timeout,
    ipc_send,
} from "./wasmos_imports";
import {Future, FutureState, Promise} from "./coroutine";

const IPC_FIELD_TYPE: i32 = 0;
const IPC_FIELD_REQUEST_ID: i32 = 1;
const IPC_FIELD_ARG0: i32 = 2;
const IPC_FIELD_ARG1: i32 = 3;
const IPC_FIELD_SOURCE: i32 = 4;
const IPC_FIELD_DESTINATION: i32 = 5;
const IPC_FIELD_ARG2: i32 = 6;
const IPC_FIELD_ARG3: i32 = 7;

/** Matches WASMOS_SYS_INTENT_MAX / WASMOS_SYS_HANDLER_MAX in libsys.h. */
export const INTENT_MAX: i32 = 16;
export const HANDLER_MAX: i32 = 16;
/* Messages held for receive() when no handler claimed them. Bounded, because a
 * bump allocator cannot grow one; see poll's backpressure note. */
export const DEFERRED_MAX: i32 = 8;

/** One received message. The loop owns one and refills it; see EventLoop.poll. */
export class IpcMessage {
    type: i32 = 0;
    requestId: i32 = 0;
    source: i32 = 0;
    destination: i32 = 0;
    arg0: i32 = 0;
    arg1: i32 = 0;
    arg2: i32 = 0;
    arg3: i32 = 0;

    /** Fill from the caller's last-received slot. */
    readLast(): void {
        this.type = ipc_last_field(IPC_FIELD_TYPE);
        this.requestId = ipc_last_field(IPC_FIELD_REQUEST_ID);
        this.source = ipc_last_field(IPC_FIELD_SOURCE);
        this.destination = ipc_last_field(IPC_FIELD_DESTINATION);
        this.arg0 = ipc_last_field(IPC_FIELD_ARG0);
        this.arg1 = ipc_last_field(IPC_FIELD_ARG1);
        this.arg2 = ipc_last_field(IPC_FIELD_ARG2);
        this.arg3 = ipc_last_field(IPC_FIELD_ARG3);
    }

    copyFrom(other: IpcMessage): void {
        this.type = other.type;
        this.requestId = other.requestId;
        this.source = other.source;
        this.destination = other.destination;
        this.arg0 = other.arg0;
        this.arg1 = other.arg1;
        this.arg2 = other.arg2;
        this.arg3 = other.arg3;
    }
}

/**
 * A message callback. C passes a function pointer plus `void* user`; the
 * subclass instance IS the user data, the same substitution coroutine.ts makes
 * for Task and OnSuccess.
 */
export abstract class OnMessage {
    abstract call(msg: IpcMessage): void;
}

/**
 * Decides how a reply settles its future: 0 resolves, negative rejects with
 * that status. Protocol-specific validation lives here so application state
 * machines never observe a reply of the wrong type.
 */
export abstract class ReplyStatus {
    abstract call(reply: IpcMessage): i32;
}

class Intent {
    inUse: bool = false;
    requestId: i32 = 0;
    onResolve: OnMessage|null = null;
}

class Handler {
    inUse: bool = false;
    msgType: i32 = 0;
    onMessage: OnMessage|null = null;
}

/* Rejections carry the transport status when there is one; a refusal with no
 * status of its own would be a bare -1 crossing a boundary. */
function bridgeStatus(status: i32): i32 {
    return status < 0 ? status : -1;
}

export class EventLoop {
    receiverEndpoint: i32 = -1;
    /** Select set watching receiverEndpoint, so poll can block; -1 if unavailable. */
    selectId: i32 = -1;
    nextRequestId: i32 = 1;
    defaultHandler: OnMessage|null = null;
    /* Reused across poll iterations rather than allocated per message. */
    message: IpcMessage = new IpcMessage();
    polling: bool = false;
    intents: StaticArray<Intent> = new StaticArray<Intent>(INTENT_MAX);
    handlers: StaticArray<Handler> = new StaticArray<Handler>(HANDLER_MAX);
    /* Ring of messages nothing claimed, waiting for receive(). */
    deferred: StaticArray<IpcMessage> = new StaticArray<IpcMessage>(DEFERRED_MAX);
    deferredHead: i32 = 0;
    deferredCount: i32 = 0;

    constructor() {
        for (let i = 0; i < INTENT_MAX; ++i)
            unchecked(this.intents[i] = new Intent());
        for (let i = 0; i < HANDLER_MAX; ++i)
            unchecked(this.handlers[i] = new Handler());
        for (let i = 0; i < DEFERRED_MAX; ++i)
            unchecked(this.deferred[i] = new IpcMessage());
    }

    /**
     * Bind to `receiverEndpoint` and seed request ids at `requestIdBase`.
     * Bases must not overlap between loops in one process, since a request id
     * is what routes a reply back to the intent that asked for it.
     */
    init(receiverEndpoint: i32, requestIdBase: i32): void {
        this.receiverEndpoint = receiverEndpoint;
        this.nextRequestId = requestIdBase;
        this.defaultHandler = null;
        this.polling = false;
        this.deferredHead = 0;
        this.deferredCount = 0;
        this.selectId = -1;
        if (receiverEndpoint >= 0) {
            const sel = ipc_select_create();
            if (sel > 0) {
                if (ipc_select_add(sel, receiverEndpoint) == 0) {
                    this.selectId = sel;
                } else {
                    ipc_select_destroy(sel);
                }
            }
        }
        for (let i = 0; i < INTENT_MAX; ++i) {
            const intent = unchecked(this.intents[i]);
            intent.inUse = false;
            intent.requestId = 0;
            intent.onResolve = null;
        }
        for (let i = 0; i < HANDLER_MAX; ++i) {
            const handler = unchecked(this.handlers[i]);
            handler.inUse = false;
            handler.msgType = 0;
            handler.onMessage = null;
        }
    }

    /** Handles every message no typed handler claimed. */
    setDefault(onMessage: OnMessage): i32 {
        this.defaultHandler = onMessage;
        return 0;
    }

    /** Register (or replace) the handler for `msgType`. -1 when the table is full. */
    register(msgType: i32, onMessage: OnMessage): i32 {
        for (let i = 0; i < HANDLER_MAX; ++i) {
            const handler = unchecked(this.handlers[i]);
            if (handler.inUse && handler.msgType == msgType) {
                handler.onMessage = onMessage;
                return 0;
            }
        }
        for (let i = 0; i < HANDLER_MAX; ++i) {
            const handler = unchecked(this.handlers[i]);
            if (!handler.inUse) {
                handler.inUse = true;
                handler.msgType = msgType;
                handler.onMessage = onMessage;
                return 0;
            }
        }
        return -1;
    }

    private claimIntent(requestId: i32, onResolve: OnMessage): i32 {
        for (let i = 0; i < INTENT_MAX; ++i) {
            const intent = unchecked(this.intents[i]);
            if (!intent.inUse) {
                intent.inUse = true;
                intent.requestId = requestId;
                intent.onResolve = onResolve;
                return i;
            }
        }
        return -1;
    }

    private releaseIntent(index: i32): void {
        const intent = unchecked(this.intents[index]);
        intent.inUse = false;
        intent.requestId = 0;
        intent.onResolve = null;
    }

    /**
     * Send a request and route its reply to `onResolve`. Returns the allocated
     * request id, or -1 when the intent table is full or the send fails.
     */
    intentSend(destinationEndpoint: i32, sourceEndpoint: i32, type: i32, arg0: i32, arg1: i32,
               arg2: i32, arg3: i32, onResolve: OnMessage): i32 {
        const requestId = this.nextRequestId++;
        const index = this.claimIntent(requestId, onResolve);
        if (index < 0)
            return -1;
        if (ipc_send(destinationEndpoint, sourceEndpoint, type, requestId, arg0, arg1, arg2,
                     arg3) != 0) {
            this.releaseIntent(index);
            return -1;
        }
        return requestId;
    }

    /**
     * As intentSend, but for a protocol that dictates the request id. A live
     * intent already holding that id is refused rather than shadowed, since two
     * intents on one id would make the reply route ambiguous.
     */
    intentSendWithRequestId(destinationEndpoint: i32, sourceEndpoint: i32, requestId: i32,
                            type: i32, arg0: i32, arg1: i32, arg2: i32, arg3: i32,
                            onResolve: OnMessage): i32 {
        if (requestId <= 0)
            return -1;
        for (let i = 0; i < INTENT_MAX; ++i) {
            const intent = unchecked(this.intents[i]);
            if (intent.inUse && intent.requestId == requestId)
                return -1;
        }
        const index = this.claimIntent(requestId, onResolve);
        if (index < 0)
            return -1;
        if (ipc_send(destinationEndpoint, sourceEndpoint, type, requestId, arg0, arg1, arg2,
                     arg3) != 0) {
            this.releaseIntent(index);
            return -1;
        }
        return 0;
    }

    /**
     * Stop tracking `requestId`. Only local tracking stops: the peer may still
     * reply, and that late reply is then dispatched as an ordinary message.
     */
    intentCancel(requestId: i32): void {
        if (requestId <= 0)
            return;
        for (let i = 0; i < INTENT_MAX; ++i) {
            const intent = unchecked(this.intents[i]);
            if (intent.inUse && intent.requestId == requestId) {
                this.releaseIntent(i);
                return;
            }
        }
    }

    private dispatch(msg: IpcMessage): void {
        for (let i = 0; i < INTENT_MAX; ++i) {
            const intent = unchecked(this.intents[i]);
            if (intent.inUse && intent.requestId == msg.requestId) {
                const callback = intent.onResolve;
                /* Released BEFORE the callback runs, so the callback may start
                 * the next request on this slot. */
                this.releaseIntent(i);
                if (callback !== null)
                    callback.call(msg);
                return;
            }
        }
        for (let i = 0; i < HANDLER_MAX; ++i) {
            const handler = unchecked(this.handlers[i]);
            if (handler.inUse && handler.msgType == msg.type && handler.onMessage !== null) {
                (<OnMessage>handler.onMessage).call(msg);
                return;
            }
        }
        const fallback = this.defaultHandler;
        if (fallback !== null) {
            fallback.call(msg);
            return;
        }
        /* Nothing claimed it. Held for receive() rather than dropped -- silently
         * discarding an unrecognised message is the exact failure this loop
         * exists to remove. */
        const slot =
            unchecked(this.deferred[(this.deferredHead + this.deferredCount) % DEFERRED_MAX]);
        slot.copyFrom(msg);
        this.deferredCount++;
    }

    /** Oldest deferred message, or null. */
    private takeDeferred(): IpcMessage|null {
        if (this.deferredCount == 0)
            return null;
        const msg = unchecked(this.deferred[this.deferredHead]);
        this.deferredHead = (this.deferredHead + 1) % DEFERRED_MAX;
        this.deferredCount--;
        return msg;
    }

    /**
     * Dispatch up to `budget` messages and return how many were handled. With
     * nothing pending it blocks once in ipc_select_wait rather than spinning.
     *
     * NOT re-entrant, which is the one place this diverges from the C loop: C
     * reads each message into a stack local, AS has one loop-owned record so a
     * nested poll would overwrite the message its caller is still dispatching.
     * A nested call returns 0 instead of corrupting it. Handlers that need to
     * drive further traffic should send from the handler and let the outer poll
     * deliver the reply.
     */
    poll(budget: i32): i32 {
        return this.pollTimeout(budget, 0);
    }

    /**
     * As poll, but an idle wait gives up after `timeoutMs` (0 = wait forever).
     * For a device whose events do NOT arrive as IPC -- a polled port, say --
     * this is how its driver stays responsive to IPC without a sched_yield
     * spin: it blocks for the poll interval instead of burning the CPU.
     */
    pollTimeout(budget: i32, timeoutMs: i32): i32 {
        if (this.polling)
            return 0;
        if (budget == 0)
            budget = 1;
        this.polling = true;
        const msg = this.message;
        let handled = 0;
        for (let i = 0; i < budget; ++i) {
            /* Only pull what there is room to hold. A message with nowhere to
             * go stays in the kernel endpoint queue, which is backpressure; the
             * alternative is dropping it. */
            if (this.deferredCount >= DEFERRED_MAX)
                break;
            if (ipc_drain(this.receiverEndpoint) <= 0) {
                if (i != 0 || this.selectId <= 0)
                    break;
                if (timeoutMs > 0) {
                    ipc_select_wait_timeout(this.selectId, timeoutMs);
                } else {
                    ipc_select_wait(this.selectId);
                }
                if (ipc_drain(this.receiverEndpoint) <= 0)
                    break;
            }
            msg.readLast();
            handled++;
            this.dispatch(msg);
        }
        this.polling = false;
        return handled;
    }

    /**
     * Park until a message arrives that nothing else claimed, and return it.
     * This is the sequential face of the same loop: a driver whose main loop is
     * a switch over message types writes
     *
     *     for (;;) {
     *         const msg = loop.receive(0);
     *         if (msg === null) break;
     *         switch on msg.type
     *     }
     *
     * and still gets full demultiplexing -- replies to in-flight requests
     * settle their futures internally and are never returned here.
     *
     * `timeoutMs` 0 parks indefinitely, and null then means the loop has no
     * select set and cannot park at all. A positive timeout bounds the wait,
     * for a driver whose device events do NOT arrive as IPC, and null means
     * nothing turned up in time.
     *
     * The returned record is the loop's, valid until the next receive() or
     * poll(). Copy anything that must outlive that.
     *
     * receive() and a default handler are two spellings of the same slot: with
     * a default handler installed, nothing is ever left for receive() to
     * return. Registered per-type handlers compose with it -- they run, and
     * receive() returns what they did not claim.
     */
    receive(timeoutMs: i32): IpcMessage|null {
        let deferred = this.takeDeferred();
        while (deferred === null) {
            if (this.pollTimeout(1, timeoutMs) == 0) {
                /* Nothing arrived: out of time, or nothing to park on. */
                return null;
            }
            deferred = this.takeDeferred();
        }
        return deferred;
    }

    /** True while poll can park the process instead of returning immediately. */
    canBlock(): bool {
        return this.selectId > 0;
    }

    /**
     * Drive the loop until `future` settles, handling everything else that
     * arrives meanwhile. This is how a driver performs a sequential startup
     * step -- register, look up a peer -- without the old pattern of blocking
     * on the service endpoint and DISCARDING the client requests that raced in.
     *
     * Returns false, rather than spinning, when the loop has no select set and
     * therefore cannot park: a caller that cannot block must report the failure
     * instead of burning the CPU until the reply happens to turn up.
     */
    awaitFuture(future: Future, budget: i32): bool {
        while (future.state == FutureState.Pending) {
            if (this.poll(budget) == 0 && !this.canBlock())
                return false;
        }
        return true;
    }
}

/**
 * One in-flight request as a future. Caller-owned and reusable: send() refuses
 * while the previous round trip is still active, and init() re-arms it.
 *
 * The reply is COPIED into this record before the future settles, so the value
 * a continuation reads is the reply to its own request and not whatever the
 * loop received afterwards.
 */
export class IpcFuture extends OnMessage {
    future: Future = new Future();
    promise: Promise = new Promise();
    reply: IpcMessage = new IpcMessage();
    loop: EventLoop|null = null;
    replyStatus: ReplyStatus|null = null;
    requestId: i32 = 0;
    active: bool = false;

    /* Its own reply callback: a separate resolver object would be one
     * allocation per send, and under a bump allocator with no collector that
     * leaks for the process's life. */
    constructor(replyStatus: ReplyStatus|null = null) {
        super();
        this.init(replyStatus);
    }

    /** Re-arm for another round trip. `replyStatus` null accepts any reply. */
    init(replyStatus: ReplyStatus|null): void {
        this.future.init(this.promise);
        this.loop = null;
        this.replyStatus = replyStatus;
        this.requestId = 0;
        this.active = false;
    }

    /**
     * Send and return the future that settles with the reply. Returns null when
     * the record is already in flight or was not re-armed; a send that FAILS
     * still returns the future, rejected, so a caller that chained onto it sees
     * the failure through the same path as a rejecting reply.
     */
    send(loop: EventLoop, destinationEndpoint: i32, sourceEndpoint: i32, type: i32, arg0: i32,
         arg1: i32, arg2: i32, arg3: i32): Future|null {
        if (this.active || this.future.state != FutureState.Pending)
            return null;
        const requestId = loop.intentSend(destinationEndpoint, sourceEndpoint, type, arg0, arg1,
                                          arg2, arg3, this);
        if (requestId < 0) {
            this.promise.reject(bridgeStatus(-1));
            return this.future;
        }
        this.loop = loop;
        this.requestId = requestId;
        this.active = true;
        return this.future;
    }

    /**
     * Abandon an in-flight request and reject its future with `status`. The
     * peer's reply, if it still comes, is dispatched as an ordinary message.
     */
    cancel(status: i32): void {
        if (!this.active)
            return;
        const loop = this.loop;
        if (loop !== null)
            loop.intentCancel(this.requestId);
        this.active = false;
        this.requestId = 0;
        this.promise.reject(bridgeStatus(status));
    }

    /** The loop's reply callback; not called by application code. */
    call(msg: IpcMessage): void {
        this.reply.copyFrom(msg);
        this.active = false;
        this.requestId = 0;
        const validator = this.replyStatus;
        const status = validator !== null ? validator.call(this.reply) : 0;
        if (status == 0) {
            /* Resolved with a non-zero marker, not the reply itself: a usize
             * value would be a raw pointer. Continuations read `reply`. */
            this.promise.resolve(< usize > 1);
        } else {
            this.promise.reject(bridgeStatus(status));
        }
    }
}
