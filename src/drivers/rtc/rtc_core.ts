/* rtc_core.ts - the clock-independent half of an RTC driver.
 *
 * An RTC driver is this core linked with one backend: cmos.ts (the x86_64
 * MC146818), pl031.ts (the Arm PL031) or rtc_efi.ts (UEFI runtime services).
 * The backend knows its clock and nothing else; the core owns everything the
 * rest of the system sees, so every backend registers as "rtc" and serves the
 * same protocol (RTC_IPC_READ_REQ, RTC_IPC_SET_REQ; abi/opcodes.yaml `rtc`).
 * One image per backend, because an image's manifest declares its
 * capabilities: CMOS needs io.port, the PL031 mmio.map, the EFI clock rtc.efi.
 *
 * The driver is one coroutine (rtcTask), started by the backend's exported
 * initialize() through runRtc: registration, the ready notification and the
 * request loop read top to bottom, and every point where it waits is a call
 * that suspends.
 *
 * Registration goes through the event loop, not a blocking ipc_recv on the
 * SERVICE endpoint: send-then-receive-until-the-reply-arrives discards every
 * unrelated message, so a client read request landing in that window is lost
 * and its sender waits forever. Failure paths return packed driver-domain codes
 * across the entry-point boundary, never a bare -1.
 */

import {std, startup} from "./wasmos";
import {
    coroutineMain,
    defaultLoop,
    EventLoop,
    IpcFuture,
    IpcMessage,
    ReplyStatus,
} from "./eventloop";
import {AWAIT_PENDING, Box} from "./coroutine";
import {
    WASMOS_ERR_DRIVER_DEVICE_INIT,
    WASMOS_ERR_DRIVER_ENDPOINT_CREATE,
    WASMOS_ERR_DRIVER_REGISTER,
    WASMOS_ERR_NONE,
    WASMOS_ERR_RTC_INVALID,
} from "./wasmos_status";
import {ipc_create_endpoint, ipc_send} from "./wasmos_imports";
import {packDate, packTime, RTC_FIELDS, RtcClock, unpackTime, validateTime} from "./rtc_time";

const SVC_IPC_REGISTER_REQ: i32 = 0x220;
const SVC_IPC_REGISTER_RESP: i32 = 0x2a0;
const PROC_IPC_NOTIFY_READY: i32 = 0x20c;

const RTC_IPC_READ_REQ: i32 = 0x820;
const RTC_IPC_SET_REQ: i32 = 0x821;
const RTC_IPC_READ_RESP: i32 = 0x8a0;
const RTC_IPC_SET_RESP: i32 = 0x8a1;
const RTC_IPC_ERROR: i32 = 0x8ff;

const RTC_NAME_PACKED: i32 = 0x00637472; /* "rtc\0" */

const g_loop: EventLoop = defaultLoop;
let g_rtc_ep: i32 = -1;
let g_clock: RtcClock | null = null;
/* One scratch record, reused. `--runtime stub` is a bump allocator with no
 * collector, so a StaticArray allocated per request would leak for the life of
 * a driver that runs forever. */
const g_values: StaticArray<i32> = new StaticArray<i32>(RTC_FIELDS);

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

// ------------------------------------------------------------------ protocol

function serveRequest(msg: IpcMessage): void {
    if (msg.source < 0) {
        return;
    }
    const clock = changetype<RtcClock>(g_clock);

    if (msg.type == RTC_IPC_READ_REQ) {
        const rc = clock.read(g_values);
        if (rc != WASMOS_ERR_NONE) {
            ipc_send(msg.source, g_rtc_ep, RTC_IPC_ERROR, msg.requestId, rc, 0, 0, 0);
            return;
        }
        ipc_send(
            msg.source,
            g_rtc_ep,
            RTC_IPC_READ_RESP,
            msg.requestId,
            packTime(g_values),
            packDate(g_values),
            0,
            0,
        );
        return;
    }

    if (msg.type == RTC_IPC_SET_REQ) {
        unpackTime(msg.arg0, msg.arg1, g_values);
        const rc = validateTime(g_values) ? clock.set(g_values) : WASMOS_ERR_RTC_INVALID;
        if (rc != WASMOS_ERR_NONE) {
            ipc_send(msg.source, g_rtc_ep, RTC_IPC_ERROR, msg.requestId, rc, 0, 0, 0);
            return;
        }
        ipc_send(msg.source, g_rtc_ep, RTC_IPC_SET_RESP, msg.requestId, WASMOS_ERR_NONE, 0, 0, 0);
        return;
    }

    /* Unrecognised type: ignore it, exactly like the keyboard and mouse drivers
     * do. Anything landing here is unsolicited rather than a real RTC request,
     * and bouncing RTC_IPC_ERROR back at the sender would ping-pong forever
     * with the process manager's own error path and peg a CPU. */
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
function rtcTask(): i32 {
    // proc.endpoint comes from the spawn-info contract, not an entry arg.
    let procEndpoint: i32 = startup.procEndpoint();
    let clock: RtcClock = changetype<RtcClock>(g_clock);

    /* A clock that cannot come up is not registered: a client would get an
     * error for every request instead of finding no "rtc" at all. */
    let initStatus: i32 = clock.init();
    if (initStatus != WASMOS_ERR_NONE) {
        std.printf("[rtc] " + clock.label() + " init failed\n");
        return WASMOS_ERR_DRIVER_DEVICE_INIT;
    }

    g_rtc_ep = ipc_create_endpoint();
    if (g_rtc_ep < 0) {
        std.printf("[rtc] endpoint failure\n");
        return WASMOS_ERR_DRIVER_ENDPOINT_CREATE;
    }
    g_loop.init(g_rtc_ep, 1);

    let registration: IpcFuture = new IpcFuture(new RegisterReply());
    if (
        registration.send(
            g_loop,
            procEndpoint,
            g_rtc_ep,
            SVC_IPC_REGISTER_REQ,
            RTC_NAME_PACKED,
            0,
            0,
            0,
        ) === null
    ) {
        std.printf("[rtc] register send failure\n");
        return WASMOS_ERR_DRIVER_REGISTER;
    }
    /* Client traffic racing the handshake is dispatched while this waits, rather
     * than dropped. Reaching the next line IS the success case: a rejected await
     * fails the coroutine rather than returning. */
    let registered: i32 = awaitReply(registration);

    std.printf("[rtc] driver ready (" + clock.label() + ")\n");

    /* The process manager acks this; awaiting the ack consumes it rather than
     * leaving it to arrive later as an unrecognised message. */
    let ready: IpcFuture = new IpcFuture(null);
    if (ready.send(g_loop, procEndpoint, g_rtc_ep, PROC_IPC_NOTIFY_READY, 0, 0, 0, 0) !== null) {
        let acked: i32 = awaitReply(ready);
    }

    for (;;) {
        let msg: IpcMessage = awaitMessage(g_loop);
        serveRequest(msg);
    }
}

export {RtcClock} from "./rtc_time";

/** Run the driver on `clock`, from a backend's exported initialize(). */
export function runRtc(clock: RtcClock): i32 {
    g_clock = clock;
    return coroutineMain(rtcTask());
}
