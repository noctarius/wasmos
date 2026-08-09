/* PS/2 keyboard driver.
 *
 * Written as one coroutine: registration, the ready notification and the event
 * loop read top to bottom, and every point where the driver waits is a call
 * that suspends. tools/as_coroutine_transform.mjs lowers @coroutine into the pc
 * state machine that actually runs, so the shape of the logic is not buried in
 * the shape of the machine. Only the pump at the bottom of initialize() is not
 * sequential, and that is the whole of it.
 *
 * Every input -- an IRQ delivered as IPC, a client's subscribe request, the
 * service registry's reply to our own registration -- arrives on one endpoint
 * and is demultiplexed by EventLoop. The loop parks the process when there is
 * nothing to do, so an idle keyboard costs no CPU.
 *
 * What this replaced, and why. Registration used to be a blocking ipc_recv on
 * the SERVICE endpoint: send the register request, then receive until the reply
 * turned up, discarding anything else. A client whose subscribe request landed
 * in that window was silently dropped and waited forever for a response. The
 * loop handles it instead. The polling fallback used to be a sched_yield spin,
 * which pegged a core; it is now a bounded wait.
 */

import {std, startup} from "./wasmos";
import {EventLoop, IpcFuture, IpcMessage, ReplyStatus} from "./eventloop";
import {AWAIT_PENDING, Box, Coroutine, Runtime} from "./coroutine";
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
let g_irq_routed: bool = false;
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
function awaitMessage(loop: EventLoop, out: Box):
    i32 {
        const status = loop.nextMessage().await(out);
        if (status != AWAIT_PENDING) {
            loop.rearmMessage();
        }
        return status;
    }

/** Wait for a request's reply. */
@suspend function awaitReply(request: IpcFuture, out: Box):
    i32 {
        return request.future.await(out);
    }

// -------------------------------------------------------------------- device

function readScancode():
    i32 {
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

function addSubscriber(ep: i32):
    i32 {
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
function notifySubscribers(scancode: i32, keyup: i32, extended: i32):
    void {
        for (let i = 0; i < MAX_SUBSCRIBERS; ++i) {
            const ep = unchecked(g_subscribers[i]);
            if (ep >= 0)
                ipc_send(ep, g_kbd_ep, KBD_IPC_KEY_NOTIFY, 0, scancode, keyup, extended, 0);
        }
    }

/* A scancode byte, wherever it came from: 0xE0 is a prefix that qualifies the
 * next byte rather than a key of its own. */
function publishScancode(code: i32):
    void {
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

/** The registry's reply is only useful if it reports success. */
class RegisterReply extends ReplyStatus {
    call(reply: IpcMessage): i32 {
        return reply.type == SVC_IPC_REGISTER_RESP && reply.arg0 == 0 ? 0
                                                                      : WASMOS_ERR_DRIVER_REGISTER;
    }
}

// ------------------------------------------------------------------ the body

/**
 * The whole driver, in order. Each await is a point where it stops and the
 * process parks; between them this is ordinary code.
 */
@coroutine
function keyboardDriver(procEndpoint: i32):
    i32 {
        let registration: IpcFuture = new IpcFuture(new RegisterReply());
        if (registration.send(g_loop, procEndpoint, g_kbd_ep, SVC_IPC_REGISTER_REQ, KBD_NAME_PACKED,
                              0, 0, 0) === null) {
            return WASMOS_ERR_DRIVER_REGISTER;
        }
        /* Client traffic racing the handshake is dispatched while this waits,
         * rather than dropped. */
        let registered: i32 = awaitReply(registration);
        /* Reaching here IS the success case: RegisterReply rejects a reply that is
         * not a successful registration, and a rejected await fails the coroutine
         * rather than returning. The awaited value is only a marker -- an IpcFuture
         * carries its reply in `.reply`, not through the box. */

        g_irq_routed = irq_route_ipc(KBD_IRQ, g_kbd_ep) == 0;
        std.printf(g_irq_routed ? "[keyboard] driver starting (IRQ-driven)\n"
                                : "[keyboard] IRQ route failed, falling back to polling\n");

        /* The process manager acks this; awaiting the ack consumes it rather than
         * leaving it to arrive later as an unrecognised message. */
        let ready: IpcFuture = new IpcFuture(null);
        if (ready.send(g_loop, procEndpoint, g_kbd_ep, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0) !==
            null) {
            let acked: i32 = awaitReply(ready);
        }

        for (;;) {
            let msg: IpcMessage = awaitMessage(g_loop);
            if (msg.type == KBD_IPC_SUBSCRIBE_REQ) {
                if (msg.source >= 0) {
                    let ok: i32 = addSubscriber(msg.source);
                    ipc_send(msg.source, g_kbd_ep, KBD_IPC_SUBSCRIBE_RESP, msg.requestId, ok, 0, 0,
                             0);
                }
            } else if (msg.type == KBD_IPC_IRQ_EVENT) {
                let code: i32 = readScancode();
                /* Re-arm after reading the hardware register so the next keypress
                 * can fire the interrupt again. Must come after io_in8 so OBF is
                 * clear before unmasking (prevents immediate re-fire on a
                 * level-triggered IRQ). */
                irq_ack(KBD_IRQ);
                publishScancode(code);
            }
            /* Anything else is not ours; ignoring it is a decision, not a drop. */
        }
    }

// -------------------------------------------------------------------- driver

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

    const runtime = new Runtime();
    const coroutine = new Coroutine();
    const completion = runtime.asyncStart(coroutine, keyboardDriver(procEndpoint));
    if (completion === null) {
        std.printf("[keyboard] could not start\n");
        proc_exit(WASMOS_ERR_DRIVER_REGISTER);
        return WASMOS_ERR_DRIVER_REGISTER;
    }

    /* The pump, and the only part of this driver that is not sequential:
     * advance the coroutine as far as it will go, then park in the event loop
     * until something it is waiting for arrives. IRQ-driven, every event is IPC
     * so the wait is indefinite; polled, it is bounded and the hardware is read
     * between waits -- a park per interval, never a spin. */
    while (!completion.poll(null, null)) {
        runtime.run();
        if (g_irq_routed) {
            if (g_loop.poll(POLL_BUDGET) == 0 && !g_loop.canBlock()) {
                break; /* nothing to park on; spinning here would burn a core */
            }
        } else {
            g_loop.pollTimeout(POLL_BUDGET, POLL_INTERVAL_MS);
            for (;;) {
                const code = readScancode();
                if (code < 0)
                    break;
                publishScancode(code);
            }
        }
    }

    const status = new Box();
    const value = new Box();
    completion.poll(status, value);
    /* A rejection arrives as the status; a coroutine that returned an error code
     * arrives as the value. Anything else means the pump gave up because it had
     * nothing left to park on. */
    let failure = <i32>status.value;
    if (failure == 0)
        failure = <i32>value.value;
    if (failure == 0)
        failure = WASMOS_ERR_DRIVER_SELECT_SETUP;
    std.printf("[keyboard] exiting\n");
    proc_exit(failure);
    return failure;
}
