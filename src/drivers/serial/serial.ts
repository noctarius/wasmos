/* 16550 UART (COM1) driver.
 *
 * The driver is one coroutine, and it IS the entry point: registration,
 * hardware bring-up, the ready notification and the request loop read top to
 * bottom, and every point where it waits is a call that suspends. Because
 * initialize() is exported, tools/as_coroutine_transform.mjs keeps its signature
 * and hands the lowered task to libc's pump, which owns the coroutine runtime.
 *
 * RX has one hard deadline and one soft one. The hard deadline is emptying the
 * 16-byte UART FIFO before it overruns, so the IRQ handler always drains it into
 * the software ring immediately. Forwarding to the vt has no deadline, so it
 * happens from the ring in the pump's idle hook. When the vt's queue is full the
 * bytes stay in the ring and are retried rather than dropped; dropping them
 * corrupts long command lines under load.
 *
 * That retry is why this driver cannot simply park: it has to wake up again
 * even with no message to service. The idle hook's interval is the mechanism --
 * zero (park indefinitely) once the ring is drained, a bounded wait while
 * backpressured. A sched_yield spin is not an option here; it pegs a core.
 *
 * Registration goes through the event loop, not a blocking ipc_recv on the
 * SERVICE endpoint, so a client request landing during the handshake is not
 * discarded. Failure paths return packed driver-domain codes across the
 * entry-point boundary, never a bare -1.
 */

import {io, std, startup} from "./wasmos";
import {defaultLoop, EventLoop, IpcFuture, IpcMessage, OnIdle, ReplyStatus} from "./eventloop";
import {AWAIT_PENDING, Box} from "./coroutine";
import {
    WASMOS_ERR_DRIVER_DEVICE_INIT,
    WASMOS_ERR_DRIVER_ENDPOINT_CREATE,
    WASMOS_ERR_DRIVER_REGISTER,
} from "./wasmos_status";
import {
    io_out8,
    io_wait,
    ipc_create_endpoint,
    ipc_send,
    irq_ack,
    irq_route_ipc,
    serial_register,
} from "./wasmos_imports";

/* 16550 UART at the fixed ISA COM1 base. Registers are offsets from that base:
 * +0 is the receive/transmit holding register (and, with DLAB set, the low
 * divisor byte), +1 the Interrupt Enable Register (high divisor byte under
 * DLAB), +2 the FIFO control register, +3 the line control register holding
 * DLAB, +4 modem control, +5 the Line Status Register.
 *
 * Only the three used repeatedly are named; the rest appear as COM1_PORT + n in
 * serialInitHw, where the write order matters more than the names. In the line
 * status register bit 0 (0x01) is "receive data available" and bit 5 (0x20) is
 * "transmit holding register empty". */
const COM1_PORT: i32 = 0x3f8;
const COM1_IER: i32 = COM1_PORT + 1;
const COM1_STATUS: i32 = COM1_PORT + 5;

const SERIAL_DRIVER_WRITE_REQ: i32 = 0x500;
const SERIAL_DRIVER_READ_REQ: i32 = 0x501;
const SERIAL_IPC_SUBSCRIBE_REQ: i32 = 0x502;
const SERIAL_IPC_SUBSCRIBE_RESP: i32 = 0x582;
const SERIAL_DRIVER_RESP: i32 = 0x580;
const PROC_IPC_NOTIFY_READY: i32 = 0x20c;
const SVC_IPC_REGISTER_REQ: i32 = 0x220;
const SVC_IPC_REGISTER_RESP: i32 = 0x2a0;

/* vt-facing push: RX bytes for the serial-bound slot, packed like
 * VT_IPC_WRITE_REQ (arg0[27:24]=byte_count, arg0[7:0]=byte). */
const VT_IPC_SERIAL_INPUT_REQ: i32 = 0x707;

const SERIAL_IRQ: i32 = 4;
const SERIAL_IPC_IRQ_EVENT: i32 = 0xff00;

const SERIAL_READ_STATUS_EMPTY: i32 = 0;
const SERIAL_READ_STATUS_ERROR: i32 = -1;

/* "sin\0" -- serial input. */
const SERIAL_NAME_PACKED: i32 = 0x006e6973;

/* Idle wait while polling the UART, and while the vt is backpressured. Short
 * enough that input is not perceptibly late, long enough not to be a spin. */
const POLL_INTERVAL_MS: i32 = 5;
/* A stuck transmitter must not hang the driver forever. */
const TX_WAIT_LIMIT: i32 = 100000;

const g_loop: EventLoop = defaultLoop;
let g_endpoint: i32 = -1;
/* The single RX subscriber (the vt service). -1 = none yet. */
let g_subscriber: i32 = -1;
let g_irqRouted: bool = false;

/* 512 bytes >> any interactive burst, so it effectively never fills. */
const RXBUF_CAP: i32 = 512;
const g_rxbuf: StaticArray<u8> = new StaticArray<u8>(RXBUF_CAP);
let g_rxbufHead: i32 = 0; /* index of the oldest buffered byte */
let g_rxbufCount: i32 = 0; /* number of bytes currently buffered */

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

// ------------------------------------------------------------------ RX ring

function rxbufEmpty(): bool {
    return g_rxbufCount == 0;
}

function rxbufPush(byte: i32): void {
    if (g_rxbufCount >= RXBUF_CAP) {
        /* Sustained overload with no consumer draining: drop the newest byte.
         * Bounded and effectively unreachable for interactive input. */
        return;
    }
    const tail = (g_rxbufHead + g_rxbufCount) % RXBUF_CAP;
    unchecked((g_rxbuf[tail] = <u8>(byte & 0xff)));
    g_rxbufCount++;
}

function rxbufPeek(offset: i32): i32 {
    return unchecked(g_rxbuf[(g_rxbufHead + offset) % RXBUF_CAP]) & 0xff;
}

// ------------------------------------------------------------------- device

function serialInitHw(): void {
    io_out8(COM1_IER, 0x00); /* disable interrupts during setup */
    io_out8(COM1_PORT + 3, 0x80); /* DLAB on */
    io_out8(COM1_PORT + 0, 0x01); /* divisor low (115200) */
    io_out8(COM1_PORT + 1, 0x00); /* divisor high */
    io_out8(COM1_PORT + 3, 0x03); /* 8N1, DLAB off */
    io_out8(COM1_PORT + 2, 0xc7); /* FIFO enable, clear, 14-byte threshold */
    io_out8(COM1_PORT + 4, 0x0b); /* DTR|RTS|OUT2 (OUT2 gates IRQ to the PIC) */
    io_out8(COM1_IER, 0x01); /* enable "received data available" interrupt */
}

/* A refused status read reports neither ready: the line-status register cannot
 * answer, so claiming either bit would be inventing hardware state. */
function txReady(): bool {
    const status = io.in8(COM1_STATUS);
    return status >= 0 && (status & 0x20) != 0;
}

function rxReady(): bool {
    const status = io.in8(COM1_STATUS);
    return status >= 0 && (status & 0x01) != 0;
}

/* Bounded hardware wait: there is nothing to park on, and the bound stops a
 * wedged transmitter from hanging the driver. */
function writePort(value: i32): void {
    for (let i = 0; i < TX_WAIT_LIMIT; ++i) {
        if (txReady()) {
            io_out8(COM1_PORT, value & 0xff);
            return;
        }
        io_wait();
    }
}

/* Returns the byte, or -1 for "nothing to read" -- which also covers a refused
 * read, since a driver that cannot reach its own UART has no byte. */
function readPort(): i32 {
    if (!rxReady()) {
        return -1;
    }
    const byte = io.in8(COM1_PORT);
    return byte < 0 ? -1 : byte;
}

/** Empty the UART RX FIFO into the software ring, before it can overrun. */
function drainUartToRing(): void {
    for (;;) {
        const c = readPort();
        if (c < 0) {
            return;
        }
        rxbufPush(c);
    }
}

/**
 * Forward buffered RX bytes to the subscribed vt, up to 4 per message (the vt
 * unpacks a count from arg0[27:24]). Stops and leaves the rest in the ring when
 * the vt's queue is full, so the bytes are retried rather than dropped.
 * Returns true once the ring is fully flushed.
 */
function flushRx(): bool {
    if (g_subscriber < 0) {
        /* No vt yet: keep the bytes. */
        return rxbufEmpty();
    }
    while (g_rxbufCount > 0) {
        const n = g_rxbufCount < 4 ? g_rxbufCount : 4;
        const b0 = rxbufPeek(0);
        const b1 = n > 1 ? rxbufPeek(1) : 0;
        const b2 = n > 2 ? rxbufPeek(2) : 0;
        const b3 = n > 3 ? rxbufPeek(3) : 0;
        const arg0 = (n << 24) | b0;
        const rc = ipc_send(g_subscriber, g_endpoint, VT_IPC_SERIAL_INPUT_REQ, 0, arg0, b1, b2, b3);
        if (rc < 0) {
            /* vt queue full: retry these bytes later, do not drop them. */
            return false;
        }
        g_rxbufHead = (g_rxbufHead + n) % RXBUF_CAP;
        g_rxbufCount -= n;
    }
    return true;
}

/**
 * Push what is buffered to the vt and decide how long the pump may park.
 *
 * The interval IS the backpressure strategy. Drained and IRQ-driven means there
 * is nothing to do until the next message, so it goes to zero and the pump
 * parks indefinitely. A vt that is full, or a UART nobody is interrupting for,
 * requires an unprompted retry, so it takes a bounded wait.
 */
function flushAndSchedule(): void {
    const flushed = flushRx();
    g_loop.idleIntervalMs = flushed && g_irqRouted ? 0 : POLL_INTERVAL_MS;
}

/**
 * Runs whenever the pump goes idle. In polled mode this is what empties the
 * UART at all; in IRQ mode it is the retry that gets backpressured bytes moving
 * again once the vt drains its queue.
 */
class RxPump extends OnIdle {
    call(): void {
        if (!g_irqRouted) {
            drainUartToRing();
        }
        flushAndSchedule();
    }
}

// ------------------------------------------------------------------ protocol

function sendResponse(destination: i32, requestId: i32, value: i32, status: i32): void {
    if (destination < 0) {
        return;
    }
    ipc_send(destination, g_endpoint, SERIAL_DRIVER_RESP, requestId, value, status, 0, 0);
}

function serveRequest(msg: IpcMessage): void {
    if (msg.type == SERIAL_IPC_IRQ_EVENT) {
        drainUartToRing();
        /* Ack after emptying the FIFO so the line deasserts before unmask. */
        irq_ack(SERIAL_IRQ);
    } else if (msg.type == SERIAL_DRIVER_WRITE_REQ) {
        writePort(msg.arg0);
        sendResponse(msg.source, msg.requestId, 0, 0);
    } else if (msg.type == SERIAL_IPC_SUBSCRIBE_REQ) {
        if (msg.source >= 0) {
            g_subscriber = msg.source;
            ipc_send(msg.source, g_endpoint, SERIAL_IPC_SUBSCRIBE_RESP, msg.requestId, 0, 0, 0, 0);
        }
    } else if (msg.type == SERIAL_DRIVER_READ_REQ) {
        /* Dead pull path. The vt owns all serial RX, so the legacy console_read
         * route must never consume input -- doing so would steal bytes from the
         * vt's stream. TODO: report empty until this path is removed entirely. */
        sendResponse(msg.source, msg.requestId, 0, SERIAL_READ_STATUS_EMPTY);
    } else {
        sendResponse(msg.source, msg.requestId, 0, SERIAL_READ_STATUS_ERROR);
    }
}

/** The registry's reply is only useful if it reports success. */
class RegisterReply extends ReplyStatus {
    call(reply: IpcMessage): i32 {
        return reply.type == SVC_IPC_REGISTER_RESP && reply.arg0 == 0
            ? 0
            : WASMOS_ERR_DRIVER_REGISTER;
    }
}

// ------------------------------------------------------------- the whole job

/**
 * The driver, in order. Each await is a point where it stops and the process
 * parks; between them this is ordinary code.
 */
@coroutine
export function initialize(_proc_endpoint: i32, _module_count: i32, _arg2: i32, _arg3: i32): i32 {
    // proc.endpoint comes from the spawn-info contract, not an entry arg.
    let procEndpoint: i32 = startup.procEndpoint();

    g_endpoint = ipc_create_endpoint();
    if (g_endpoint < 0) {
        std.printf("[serial] endpoint failure\n");
        return WASMOS_ERR_DRIVER_ENDPOINT_CREATE;
    }
    /* Kernel remote-driver hook (keeps console_read's fallback path wired). */
    if (serial_register(g_endpoint) != 0) {
        std.printf("[serial] register failure\n");
        return WASMOS_ERR_DRIVER_DEVICE_INIT;
    }
    g_loop.init(g_endpoint, 1);

    /* Discoverable name, so the vt can look this driver up and subscribe. */
    let registration: IpcFuture = new IpcFuture(new RegisterReply());
    if (
        registration.send(
            g_loop,
            procEndpoint,
            g_endpoint,
            SVC_IPC_REGISTER_REQ,
            SERIAL_NAME_PACKED,
            0,
            0,
            0,
        ) === null
    ) {
        std.printf("[serial] svc register failure\n");
        return WASMOS_ERR_DRIVER_REGISTER;
    }
    /* Client traffic racing the handshake is dispatched while this waits, rather
     * than dropped. Reaching the next line IS the success case: a rejected await
     * fails the coroutine rather than returning. */
    let registered: i32 = awaitReply(registration);

    serialInitHw();

    let irqRouted: i32 = irq_route_ipc(SERIAL_IRQ, g_endpoint);
    g_irqRouted = irqRouted == 0;
    if (g_irqRouted) {
        std.printf("[serial] driver ready (IRQ-driven)\n");
    } else {
        std.printf("[serial] IRQ route failed, falling back to polling\n");
        io_out8(COM1_IER, 0x00); /* no interrupts; the idle hook polls */
    }
    /* RX forwarding runs from the pump, not from this loop: draining the FIFO
     * has a deadline, delivering to the vt does not. */
    g_loop.idle = new RxPump();
    g_loop.idleIntervalMs = g_irqRouted ? 0 : POLL_INTERVAL_MS;

    /* The process manager acks this; awaiting the ack consumes it rather than
     * leaving it to arrive later and be answered with an error response. */
    let ready: IpcFuture = new IpcFuture(null);
    if (ready.send(g_loop, procEndpoint, g_endpoint, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0) !== null) {
        let acked: i32 = awaitReply(ready);
    }

    for (;;) {
        let msg: IpcMessage = awaitMessage(g_loop);
        serveRequest(msg);
        /* Flush HERE, not only from the idle hook. serveRequest is what drains
         * the FIFO on an IRQ, and the hook already ran for this pump iteration
         * -- before that drain -- so leaving it to the hook would hold the bytes
         * until the next message arrived, and with nothing else pending the
         * pump would park indefinitely holding them. */
        flushAndSchedule();
    }
}
