/* PS/2 mouse driver.
 *
 * The driver is one coroutine, and it IS the entry point: registration, device
 * bring-up, the ready notification and the event loop read top to bottom, and
 * every point where it waits for IPC is a call that suspends. Because
 * initialize() is exported, tools/as_coroutine_transform.mjs keeps its signature
 * and hands the lowered task to libc's pump, which owns the coroutine runtime.
 *
 * What this replaced, and why. Registration was a blocking ipc_recv on the
 * SERVICE endpoint -- send, then receive until the reply turned up, discarding
 * anything else -- so a client's subscribe request landing in that window was
 * dropped and its sender waited forever. The polled fallback yielded three times
 * per iteration and the no-endpoint path yielded forever, both of which peg a
 * core; the loop parks instead, and the polled path is a bounded wait driven by
 * the pump's idle hook.
 *
 * The bounded sched_yield loops in the controller handshake below are NOT that
 * kind of spin: they wait on a hardware status register during bring-up, have a
 * hard iteration limit, and there is nothing to park on.
 */

import {io, std, startup} from "./wasmos";
import {defaultLoop, EventLoop, IpcFuture, IpcMessage, OnIdle, ReplyStatus} from "./eventloop";
import {AWAIT_PENDING, Box} from "./coroutine";
import {WASMOS_ERR_DRIVER_ENDPOINT_CREATE, WASMOS_ERR_DRIVER_REGISTER} from "./wasmos_status";
import {
    io_out8,
    io_wait,
    ipc_create_endpoint,
    ipc_send,
    irq_ack,
    irq_route_ipc,
    sched_yield,
} from "./wasmos_imports";

/* TODO(mouse-startup): wire mouse driver into device-manager startup policy
 * once compositor pointer-event routing is implemented end-to-end. */

const CTRL_STATUS_PORT: i32 = 0x64;
const CTRL_CMD_PORT: i32 = 0x64;
const CTRL_DATA_PORT: i32 = 0x60;

const STATUS_OBF: i32 = 0x01;
const STATUS_IBF: i32 = 0x02;
const STATUS_AUX: i32 = 0x20;

const SVC_IPC_REGISTER_REQ: i32 = 0x220;
const SVC_IPC_REGISTER_RESP: i32 = 0x2a0;
const PROC_IPC_NOTIFY_READY: i32 = 0x20c;

const MOUSE_IPC_SUBSCRIBE_REQ: i32 = 0x810;
const MOUSE_IPC_SUBSCRIBE_RESP: i32 = 0x890;
const MOUSE_IPC_MOVE_NOTIFY: i32 = 0x811;

const MOUSE_IRQ: i32 = 12;
const MOUSE_IPC_IRQ_EVENT: i32 = 0xff00;
/* "mous" + "e": the registry takes a packed name across two argument words. */
const MOUSE_NAME_PACKED: i32 = 0x73756f6d;
const MOUSE_NAME_TAIL: i32 = 0x65;

const MAX_SUBSCRIBERS: i32 = 4;
/* Idle wait when the device is polled rather than IRQ-driven. */
const POLL_INTERVAL_MS: i32 = 10;

const g_loop: EventLoop = defaultLoop;
let g_mouse_ep: i32 = -1;
let g_packet_state: i32 = 0;
let g_packet0: i32 = 0;
let g_packet1: i32 = 0;
const g_subscribers: StaticArray<i32> = new StaticArray<i32>(MAX_SUBSCRIBERS);

// --------------------------------------------------------------- suspensions

/**
 * Wait for the next message no handler claimed. The re-arm belongs here: a
 * coroutine resumes by re-running this call, so the future must still be
 * settled at that moment and pending again before the next wait.
 */
@suspend
function awaitMessage(loop: EventLoop, out: Box): i32 {
    const status = loop.nextMessage().await(out);
    if (status != AWAIT_PENDING) {
        loop.rearmMessage();
    }
    return status;
}

/** Wait for a request's reply. */
@suspend
function awaitReply(request: IpcFuture, out: Box): i32 {
    return request.future.await(out);
}

// ------------------------------------------------------------------ clients

function addSubscriber(ep: i32): i32 {
    for (let i = 0; i < MAX_SUBSCRIBERS; ++i) {
        /* Ignore duplicate registrations. */
        if (unchecked(g_subscribers[i]) == ep) {
            return 0;
        }
    }
    for (let i = 0; i < MAX_SUBSCRIBERS; ++i) {
        if (unchecked(g_subscribers[i]) < 0) {
            unchecked((g_subscribers[i] = ep));
            return 0;
        }
    }
    return -1; /* full */
}

/* One-way notifications: no reply is expected, so they carry no request id and
 * need no intent. */
function notifySubscribers(dx: i32, dy: i32, buttons: i32): void {
    for (let i = 0; i < MAX_SUBSCRIBERS; ++i) {
        const ep = unchecked(g_subscribers[i]);
        if (ep >= 0) {
            ipc_send(ep, g_mouse_ep, MOUSE_IPC_MOVE_NOTIFY, 0, dx, dy, buttons, 0);
        }
    }
}

// ------------------------------------------------------------------- device

/* A refused status read ends the flush: retrying cannot make the capability
 * appear, and there is no way to tell whether the buffer still holds a byte. */
function flushOutputBuffer(): void {
    for (let i = 0; i < 64; ++i) {
        const st = io.in8(CTRL_STATUS_PORT);
        if (st < 0 || (st & STATUS_OBF) == 0) {
            return;
        }
        io.in8(CTRL_DATA_PORT);
        io_wait();
    }
}

function waitInputReady(limit: i32 = 100000): bool {
    for (let i = 0; i < limit; ++i) {
        const st = io.in8(CTRL_STATUS_PORT);
        /* A refused read will be refused every time: fail now, do not spin. */
        if (st < 0) {
            return false;
        }
        if ((st & STATUS_IBF) == 0) {
            return true;
        }
        if ((i & 0xff) == 0) {
            sched_yield();
        }
        io_wait();
    }
    return false;
}

function sendControllerCommand(cmd: i32): bool {
    if (!waitInputReady()) {
        return false;
    }
    io_out8(CTRL_CMD_PORT, cmd);
    io_wait();
    return true;
}

function sendMouseCommand(cmd: i32): bool {
    if (!sendControllerCommand(0xd4)) {
        return false;
    }
    if (!waitInputReady()) {
        return false;
    }
    io_out8(CTRL_DATA_PORT, cmd);
    io_wait();
    return true;
}

/* Returns the byte, -1 for "nothing pending" -- which a refused status read
 * joins, since without it nothing is known -- or -2 for "not ours". */
function readAuxByte(): i32 {
    const st = io.in8(CTRL_STATUS_PORT);
    if (st < 0 || (st & STATUS_OBF) == 0) {
        return -1;
    }
    if ((st & STATUS_AUX) == 0) {
        /* Not our byte: leave it for the keyboard driver. */
        return -2;
    }
    const byte = io.in8(CTRL_DATA_PORT);
    return byte < 0 ? -1 : byte;
}

function readAuxAck(limit: i32 = 50000): i32 {
    for (let i = 0; i < limit; ++i) {
        const v = readAuxByte();
        if (v >= 0) {
            return v;
        }
        if ((i & 0xff) == 0) {
            sched_yield();
        }
        io_wait();
    }
    return -1;
}

/* Read any byte from port 0x60 regardless of the AUX flag. Used for controller
 * command responses (CCB read etc.) that are not AUX data. */
function readDataByte(limit: i32 = 50000): i32 {
    for (let i = 0; i < limit; ++i) {
        const st = io.in8(CTRL_STATUS_PORT);
        /* A refused read will be refused every time: fail now, do not spin. */
        if (st < 0) {
            return -1;
        }
        if ((st & STATUS_OBF) != 0) {
            const byte = io.in8(CTRL_DATA_PORT);
            return byte < 0 ? -1 : byte;
        }
        if ((i & 0xff) == 0) {
            sched_yield();
        }
        io_wait();
    }
    return -1;
}

function initMouseDevice(): void {
    flushOutputBuffer();

    /* Enable the AUX port (clears the clock-disable bit in the CCB). */
    sendControllerCommand(0xa8);

    /* Read the Controller Command Byte and set bit 1 (AUX interrupt enable).
     * Without this, IRQ 12 never fires even though the AUX port is active. */
    if (waitInputReady()) {
        io_out8(CTRL_CMD_PORT, 0x20);
        io_wait();
        const ccb: i32 = readDataByte(4096);
        if (ccb >= 0) {
            const newCcb: i32 = ccb | 0x02;
            if (waitInputReady()) {
                io_out8(CTRL_CMD_PORT, 0x60);
                io_wait();
                if (waitInputReady()) {
                    io_out8(CTRL_DATA_PORT, newCcb);
                    io_wait();
                }
            }
        }
    }

    /* Request streaming, fail-open: some virtual or slow controllers delay their
     * ACKs, and mouse support must never block system bootstrap. */
    sendMouseCommand(0xf6);
    readAuxAck(4096);
    sendMouseCommand(0xf4);
    readAuxAck(4096);
}

/** Accumulates the three-byte PS/2 packet and publishes a completed one. */
function handleAuxByte(byte: i32): void {
    if (g_packet_state == 0) {
        /* Packet sync: bit 3 must be set on the first byte. */
        if ((byte & 0x08) == 0) {
            return;
        }
        g_packet0 = byte;
        g_packet_state = 1;
        return;
    }
    if (g_packet_state == 1) {
        g_packet1 = byte;
        g_packet_state = 2;
        return;
    }

    g_packet_state = 0;
    const p0 = g_packet0;
    const p1 = g_packet1;
    const p2 = byte;

    if ((p0 & 0xc0) != 0) {
        /* Overflow bits set: drop the packet. */
        return;
    }

    const dx: i32 = (p1 << 24) >> 24;
    const dy: i32 = -((p2 << 24) >> 24);
    const buttons: i32 = p0 & 0x07;
    notifySubscribers(dx, dy, buttons);
}

/** The registry's reply is only useful if it reports success. */
class RegisterReply extends ReplyStatus {
    call(reply: IpcMessage): i32 {
        return reply.type == SVC_IPC_REGISTER_RESP && reply.arg0 == 0
            ? 0
            : WASMOS_ERR_DRIVER_REGISTER;
    }
}

/** Drains the controller between polls when the device is not IRQ-driven. */
class AuxDrain extends OnIdle {
    call(): void {
        for (;;) {
            const byte = readAuxByte();
            if (byte < 0) {
                return;
            }
            handleAuxByte(byte);
            io_wait();
        }
    }
}

// ------------------------------------------------------------- the whole job

/**
 * The driver, in order. Each await is a point where it stops and the process
 * parks; between them this is ordinary code.
 */
@coroutine
export function initialize(_proc_endpoint: i32, _arg1: i32, _arg2: i32, _arg3: i32): i32 {
    // proc.endpoint comes from the spawn-info contract, not an entry arg.
    let procEndpoint: i32 = startup.procEndpoint();
    let slot: i32 = 0;
    for (slot = 0; slot < MAX_SUBSCRIBERS; ++slot) {
        unchecked((g_subscribers[slot] = -1));
    }

    g_mouse_ep = ipc_create_endpoint();
    if (g_mouse_ep < 0) {
        std.printf("[mouse] no IPC endpoint\n");
        return WASMOS_ERR_DRIVER_ENDPOINT_CREATE;
    }
    g_loop.init(g_mouse_ep, 1);

    let registration: IpcFuture = new IpcFuture(new RegisterReply());
    if (
        registration.send(
            g_loop,
            procEndpoint,
            g_mouse_ep,
            SVC_IPC_REGISTER_REQ,
            MOUSE_NAME_PACKED,
            MOUSE_NAME_TAIL,
            0,
            0,
        ) === null
    ) {
        return WASMOS_ERR_DRIVER_REGISTER;
    }
    /* Client traffic racing the handshake is dispatched while this waits, rather
     * than dropped. Reaching the next line IS the success case: a rejected await
     * fails the coroutine rather than returning. */
    let registered: i32 = awaitReply(registration);

    initMouseDevice();

    let irqRouted: i32 = irq_route_ipc(MOUSE_IRQ, g_mouse_ep);
    if (irqRouted == 0) {
        std.printf("[mouse] driver starting (IRQ-driven)\n");
    } else {
        /* AUX bytes do not arrive as IPC, so the pump reads them between bounded
         * waits instead of parking indefinitely. */
        std.printf("[mouse] IRQ route failed, falling back to polling\n");
        g_loop.idle = new AuxDrain();
        g_loop.idleIntervalMs = POLL_INTERVAL_MS;
    }

    /* The process manager acks this; awaiting the ack consumes it rather than
     * leaving it to arrive later as an unrecognised message. */
    let ready: IpcFuture = new IpcFuture(null);
    if (ready.send(g_loop, procEndpoint, g_mouse_ep, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0) !== null) {
        let acked: i32 = awaitReply(ready);
    }

    for (;;) {
        let msg: IpcMessage = awaitMessage(g_loop);
        if (msg.type == MOUSE_IPC_SUBSCRIBE_REQ) {
            if (msg.source >= 0) {
                let ok: i32 = addSubscriber(msg.source);
                ipc_send(
                    msg.source,
                    g_mouse_ep,
                    MOUSE_IPC_SUBSCRIBE_RESP,
                    msg.requestId,
                    ok,
                    0,
                    0,
                    0,
                );
            }
        } else if (msg.type == MOUSE_IPC_IRQ_EVENT) {
            let byte: i32 = readAuxByte();
            /* Re-arm after reading OBF: it must be clear before unmasking so the
             * PIC level-trigger does not immediately re-fire. */
            irq_ack(MOUSE_IRQ);
            if (byte >= 0) {
                handleAuxByte(byte);
            }
        }
        /* Anything else is not ours; ignoring it is a decision, not a drop. */
    }
}
