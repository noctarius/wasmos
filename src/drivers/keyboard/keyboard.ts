/* PS/2 keyboard driver.
 *
 * Event-loop driven: every input -- an IRQ delivered as IPC, a client's
 * subscribe request, the service registry's reply to our own registration --
 * arrives on one endpoint and is demultiplexed by EventLoop. The loop parks the
 * process when there is nothing to do, so an idle keyboard costs no CPU.
 *
 * What this replaced, and why. Registration used to be a blocking ipc_recv on
 * the SERVICE endpoint: send the register request, then receive until the reply
 * turned up, discarding anything else. A client whose subscribe request landed
 * in that window was silently dropped and waited forever for a response. The
 * loop handles it instead. The polling fallback used to be a sched_yield spin,
 * which pegged a core; it is now a bounded wait.
 */

import {std, startup} from "./wasmos";
import {EventLoop, IpcFuture, IpcMessage, OnMessage, ReplyStatus} from "./eventloop";
import {FutureState} from "./coroutine";
import {
    WASMOS_ERR_DRIVER_ENDPOINT_CREATE,
    WASMOS_ERR_DRIVER_REGISTER,
    WASMOS_ERR_DRIVER_SELECT_SETUP,
} from "./wasmos_status";
import {
    io_in8,
    ipc_create_endpoint,
    ipc_send,
    irq_ack,
    irq_route_ipc,
    proc_exit,
} from "./wasmos_imports";

const KEYBOARD_STATUS_PORT: i32 = 0x64;
const KEYBOARD_DATA_PORT: i32 = 0x60;
const KEYBOARD_OBF_FLAG: i32 = 0x01;
const KEYBOARD_AUX_FLAG: i32 = 0x20;

const KBD_IPC_SUBSCRIBE_REQ: i32 = 0x800;
const KBD_IPC_SUBSCRIBE_RESP: i32 = 0x880;
const KBD_IPC_KEY_NOTIFY: i32 = 0x801;
const SVC_IPC_REGISTER_REQ: i32 = 0x220;
const SVC_IPC_REGISTER_RESP: i32 = 0x2A0;
const PROC_IPC_NOTIFY_READY: i32 = 0x20C;

const KBD_IPC_IRQ_EVENT: i32 = 0xFF00;
const KBD_IRQ: i32 = 1;
const KBD_NAME_PACKED: i32 = 0x0064626B; /* "kbd\0" */

const MAX_SUBSCRIBERS: i32 = 4;
/* Messages dispatched per poll before the loop is re-entered. Bounded so a
 * burst of client traffic cannot starve the scancode read in polled mode. */
const POLL_BUDGET: i32 = 8;
/* Idle wait in polled mode. Long enough not to spin, short enough that a
 * keystroke is not perceptibly late. */
const POLL_INTERVAL_MS: i32 = 10;

let g_loop: EventLoop = new EventLoop();
let g_kbd_ep: i32 = -1;
let g_extended_pending: i32 = 0;
const g_subscribers: StaticArray<i32> = new StaticArray<i32>(MAX_SUBSCRIBERS);

function readScancode(): i32 {
    const status = io_in8(KEYBOARD_STATUS_PORT);
    if ((status & KEYBOARD_OBF_FLAG) == 0) {
        return -1;
    }
    if ((status & KEYBOARD_AUX_FLAG) != 0) {
        /* AUX (mouse) byte: leave for mouse driver. */
        return -1;
    }
    return io_in8(KEYBOARD_DATA_PORT) & 0xFF;
}

function addSubscriber(ep: i32): i32 {
    for (let i = 0; i < MAX_SUBSCRIBERS; ++i) {
        /* Ignore duplicate registrations. */
        if (unchecked(g_subscribers[i]) == ep)
            return 0;
    }
    for (let i = 0; i < MAX_SUBSCRIBERS; ++i) {
        if (unchecked(g_subscribers[i]) < 0) {
            unchecked(g_subscribers[i] = ep);
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
        if (ep >= 0)
            ipc_send(ep, g_kbd_ep, KBD_IPC_KEY_NOTIFY, 0, scancode, keyup, extended, 0);
    }
}

/* A scancode byte, wherever it came from: 0xE0 is a prefix that qualifies the
 * next byte rather than a key of its own. */
function publishScancode(code: i32): void {
    if (code < 0)
        return;
    if (code == 0xE0) {
        g_extended_pending = 1;
        return;
    }
    /* PS/2 Set 1: key-up codes have bit 7 set. */
    const keyup: i32 = (code & 0x80) != 0 ? 1 : 0;
    const extended = g_extended_pending;
    g_extended_pending = 0;
    notifySubscribers(code & 0x7F, keyup, extended);
}

function handleMessage(msg: IpcMessage): void {
    if (msg.type == KBD_IPC_SUBSCRIBE_REQ) {
        if (msg.source < 0)
            return;
        const ok = addSubscriber(msg.source);
        ipc_send(msg.source, g_kbd_ep, KBD_IPC_SUBSCRIBE_RESP, msg.requestId, ok, 0, 0, 0);
    } else if (msg.type == KBD_IPC_IRQ_EVENT) {
        const code = readScancode();
        /* Re-arm after reading the hardware register so the next keypress can
         * fire the interrupt again. Must come after io_in8 so OBF is clear
         * before unmasking (prevents immediate re-fire on a level-triggered
         * IRQ). */
        irq_ack(KBD_IRQ);
        publishScancode(code);
    }
    /* Anything else is not ours; ignoring it here is a decision, not a drop. */
}

/** The registry's reply is only useful if it reports success. */
class RegisterReply extends ReplyStatus {
    call(reply: IpcMessage): i32 {
        return reply.type == SVC_IPC_REGISTER_RESP && reply.arg0 == 0 ? 0
                                                                      : WASMOS_ERR_DRIVER_REGISTER;
    }
}

/* The process manager acks PROC_IPC_NOTIFY_READY. Routing it through an intent
 * consumes the ack rather than leaving it to fall through as an unknown
 * message. */
class ReadyAck extends OnMessage {
    call(msg: IpcMessage): void {}
}

/** Publish this endpoint to the service registry. */
function registerService(procEndpoint: i32): bool {
    const request = new IpcFuture(new RegisterReply());
    const future = request.send(g_loop, procEndpoint, g_kbd_ep, SVC_IPC_REGISTER_REQ,
                                KBD_NAME_PACKED, 0, 0, 0);
    if (future === null)
        return false;
    /* Client traffic that races the handshake is dispatched here, not dropped. */
    if (!g_loop.awaitFuture(future, POLL_BUDGET))
        return false;
    return future.state == FutureState.Ready;
}

export function initialize(_proc_endpoint: i32, _arg1: i32, _arg2: i32, _arg3: i32): i32 {
    // proc.endpoint now comes from the spawn-info contract, not an entry arg.
    const procEndpoint = startup.procEndpoint();
    for (let i = 0; i < MAX_SUBSCRIBERS; ++i)
        unchecked(g_subscribers[i] = -1);

    g_kbd_ep = ipc_create_endpoint();
    if (g_kbd_ep < 0) {
        std.printf("[keyboard] no IPC endpoint\n");
        proc_exit(WASMOS_ERR_DRIVER_ENDPOINT_CREATE);
        return WASMOS_ERR_DRIVER_ENDPOINT_CREATE;
    }

    g_loop.init(g_kbd_ep, 1);

    if (!registerService(procEndpoint)) {
        /* Unreachable to clients: without a registry entry nobody can find this
         * endpoint, so spinning here would burn a core for nothing. */
        std.printf("[keyboard] service registration failed\n");
        proc_exit(WASMOS_ERR_DRIVER_REGISTER);
        return WASMOS_ERR_DRIVER_REGISTER;
    }

    const irqRouted: bool = irq_route_ipc(KBD_IRQ, g_kbd_ep) == 0;
    std.printf(irqRouted ? "[keyboard] driver starting (IRQ-driven)\n"
                         : "[keyboard] IRQ route failed, falling back to polling\n");
    g_loop.intentSend(procEndpoint, g_kbd_ep, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0, new ReadyAck());

    /* IRQ-driven: every event is IPC, so receive parks until one arrives.
     * Polled: scancodes do not arrive as IPC, so the wait is bounded and the
     * hardware is read between messages -- a park per interval, not a spin. */
    const waitMs = irqRouted ? 0 : POLL_INTERVAL_MS;
    for (;;) {
        const msg = g_loop.receive(waitMs);
        if (msg !== null) {
            handleMessage(msg);
        } else if (irqRouted) {
            /* Nothing to park on: without a select set this loop would spin. */
            break;
        }
        if (!irqRouted) {
            for (;;) {
                const code = readScancode();
                if (code < 0)
                    break;
                publishScancode(code);
            }
        }
    }

    std.printf("[keyboard] cannot wait for events, exiting\n");
    proc_exit(WASMOS_ERR_DRIVER_SELECT_SETUP);
    return WASMOS_ERR_DRIVER_SELECT_SETUP;
}
