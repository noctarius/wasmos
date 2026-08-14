/* PS/2 keyboard driver.
 *
 * The driver is one coroutine, and it IS the entry point: registration, the
 * ready notification and the event loop read top to bottom, and every point
 * where it waits is a call that suspends. Because initialize() is exported,
 * tools/as_coroutine_transform.mjs keeps its signature and hands the lowered
 * task to libc's pump, which owns the coroutine runtime -- so there is no pump
 * here to get wrong, and nothing in this file knows the machine exists.
 *
 * Every input -- an IRQ delivered as IPC, a client's subscribe request, the
 * service registry's reply to this driver's own registration -- arrives on one
 * endpoint and is demultiplexed by EventLoop. The loop parks the process when
 * there is nothing to do, so an idle IRQ-driven keyboard costs no CPU; the
 * polling fallback wakes once per POLL_INTERVAL_MS instead.
 *
 * Registration goes through that loop, not a blocking ipc_recv on the SERVICE
 * endpoint: a receive-until-the-reply-arrives loop discards every unrelated
 * message, so a client subscribe landing in the handshake window is lost and
 * that client waits forever. The polling fallback is a bounded wait driven by
 * the pump's idle hook for a related reason -- a sched_yield spin pegs a core.
 */

import {io, std, startup} from "./wasmos";
import {defaultLoop, EventLoop, IpcFuture, IpcMessage, OnIdle, ReplyStatus} from "./eventloop";
import {AWAIT_PENDING, Box} from "./coroutine";
import {WASMOS_ERR_DRIVER_ENDPOINT_CREATE, WASMOS_ERR_DRIVER_REGISTER} from "./wasmos_status";
import {ipc_create_endpoint, ipc_send, irq_ack, irq_route_ipc} from "./wasmos_imports";

/* i8042 PS/2 controller. Reading 0x64 gives the status register; 0x60 is the
 * data port shared by the keyboard and the auxiliary (mouse) device.
 *
 * OBF (bit 0, 0x01) means the output buffer holds a byte for the host; every
 * other status bit is meaningless as a "has data" test. AUX (bit 5, 0x20) tags
 * that byte as coming from the mouse rather than the keyboard, which is what
 * lets two drivers share one data port:
 * this driver must LEAVE an AUX byte alone, since reading 0x60 consumes it and
 * would steal the mouse's input. Reading the data port is also what clears OBF,
 * so it must happen before the IRQ is acked. */
const KEYBOARD_STATUS_PORT: i32 = 0x64;
const KEYBOARD_DATA_PORT: i32 = 0x60;
const KEYBOARD_OBF_FLAG: i32 = 0x01;
const KEYBOARD_AUX_FLAG: i32 = 0x20;

const KBD_IPC_SUBSCRIBE_REQ: i32 = 0x800;
const KBD_IPC_SUBSCRIBE_RESP: i32 = 0x880;
const KBD_IPC_KEY_NOTIFY: i32 = 0x801;
const SVC_IPC_REGISTER_REQ: i32 = 0x220;
const SVC_IPC_REGISTER_RESP: i32 = 0x2a0;
const PROC_IPC_NOTIFY_READY: i32 = 0x20c;

const KBD_IPC_IRQ_EVENT: i32 = 0xff00;
const KBD_IRQ: i32 = 1;
const KBD_NAME_PACKED: i32 = 0x0064626b; /* "kbd\0" */

/* Subscriber slots. A subscribe beyond this is REFUSED (reported back as a
 * non-zero status), not queued and not displacing an existing subscriber. */
const MAX_SUBSCRIBERS: i32 = 4;
/* Idle wait when the device is polled rather than IRQ-driven. Long enough not
 * to spin, short enough that a keystroke is not perceptibly late.
 *
 * This is the pump's idleIntervalMs, whose value IS the waiting strategy: it is
 * left at 0 on the IRQ-driven path, meaning the pump parks indefinitely and an
 * idle keyboard costs no CPU because scancodes arrive as IPC. It is set to this
 * bounded value only on the polling fallback, where nothing wakes the process
 * and the idle hook has to go and read the controller itself. */
const POLL_INTERVAL_MS: i32 = 10;

const g_loop: EventLoop = defaultLoop;
let g_kbd_ep: i32 = -1;
let g_extended_pending: i32 = 0;
const g_subscribers: StaticArray<i32> = new StaticArray<i32>(MAX_SUBSCRIBERS);

// --------------------------------------------------------------- suspensions

/**
 * Wait for the next message no handler claimed.
 *
 * The re-arm is not optional, and it belongs here rather than at the call site:
 * a coroutine resumes by re-running this call, so the future must still be
 * settled at that moment, and pending again before the next wait.
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

// -------------------------------------------------------------------- device

/* Returns the scancode, or -1 when no scancode is available. A refused port
 * read joins that case: without the status register there is no way to tell
 * whether a byte is pending at all, let alone whether the keyboard produced
 * it. */
function readScancode(): i32 {
    const status = io.in8(KEYBOARD_STATUS_PORT);
    if (status < 0) {
        return -1;
    }
    if ((status & KEYBOARD_OBF_FLAG) == 0) {
        return -1;
    }
    if ((status & KEYBOARD_AUX_FLAG) != 0) {
        /* AUX (mouse) byte: leave for mouse driver. */
        return -1;
    }
    const code = io.in8(KEYBOARD_DATA_PORT);
    return code < 0 ? -1 : code;
}

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
function notifySubscribers(scancode: i32, keyup: i32, extended: i32): void {
    for (let i = 0; i < MAX_SUBSCRIBERS; ++i) {
        const ep = unchecked(g_subscribers[i]);
        if (ep >= 0) {
            ipc_send(ep, g_kbd_ep, KBD_IPC_KEY_NOTIFY, 0, scancode, keyup, extended, 0);
        }
    }
}

/* A scancode byte, wherever it came from: 0xE0 is a prefix that qualifies the
 * next byte rather than a key of its own. */
function publishScancode(code: i32): void {
    if (code < 0) {
        return;
    }
    if (code == 0xe0) {
        g_extended_pending = 1;
        return;
    }
    /* PS/2 Set 1: key-up codes have bit 7 set. */
    const keyup: i32 = (code & 0x80) != 0 ? 1 : 0;
    const extended = g_extended_pending;
    g_extended_pending = 0;
    notifySubscribers(code & 0x7f, keyup, extended);
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
class ScancodeDrain extends OnIdle {
    call(): void {
        for (;;) {
            const code = readScancode();
            if (code < 0) {
                return;
            }
            publishScancode(code);
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

    g_kbd_ep = ipc_create_endpoint();
    if (g_kbd_ep < 0) {
        std.printf("[keyboard] no IPC endpoint\n");
        return WASMOS_ERR_DRIVER_ENDPOINT_CREATE;
    }
    g_loop.init(g_kbd_ep, 1);

    let registration: IpcFuture = new IpcFuture(new RegisterReply());
    if (
        registration.send(
            g_loop,
            procEndpoint,
            g_kbd_ep,
            SVC_IPC_REGISTER_REQ,
            KBD_NAME_PACKED,
            0,
            0,
            0,
        ) === null
    ) {
        return WASMOS_ERR_DRIVER_REGISTER;
    }
    /* Client traffic racing the handshake is dispatched while this waits, rather
     * than dropped. Reaching the next line IS the success case: RegisterReply
     * rejects anything that is not a successful registration, and a rejected
     * await fails the coroutine rather than returning. The awaited value is only
     * a marker -- an IpcFuture carries its reply in `.reply`, not the box. */
    let registered: i32 = awaitReply(registration);

    let irqRouted: i32 = irq_route_ipc(KBD_IRQ, g_kbd_ep);
    if (irqRouted == 0) {
        std.printf("[keyboard] driver starting (IRQ-driven)\n");
    } else {
        /* Scancodes do not arrive as IPC, so the pump reads them between bounded
         * waits instead of parking indefinitely. */
        std.printf("[keyboard] IRQ route failed, falling back to polling\n");
        g_loop.idle = new ScancodeDrain();
        g_loop.idleIntervalMs = POLL_INTERVAL_MS;
    }

    /* The process manager acks this; awaiting the ack consumes it rather than
     * leaving it to arrive later as an unrecognised message. */
    let ready: IpcFuture = new IpcFuture(null);
    if (ready.send(g_loop, procEndpoint, g_kbd_ep, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0) !== null) {
        let acked: i32 = awaitReply(ready);
    }

    for (;;) {
        let msg: IpcMessage = awaitMessage(g_loop);
        if (msg.type == KBD_IPC_SUBSCRIBE_REQ) {
            if (msg.source >= 0) {
                let ok: i32 = addSubscriber(msg.source);
                ipc_send(msg.source, g_kbd_ep, KBD_IPC_SUBSCRIBE_RESP, msg.requestId, ok, 0, 0, 0);
            }
        } else if (msg.type == KBD_IPC_IRQ_EVENT) {
            let code: i32 = readScancode();
            /* Ack only after the data-port read: that read clears OBF, which is
             * what lets the i8042 drop the line. The kernel programs ISA IRQs
             * level-triggered in IOAPIC mode (src/kernel/arch/x86_64/ioapic.c),
             * so unmasking with OBF still set re-delivers at once. */
            irq_ack(KBD_IRQ);
            publishScancode(code);
        }
        /* Any other message type is not part of this protocol; dropping it is
         * deliberate. */
    }
}
