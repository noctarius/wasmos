/* CMOS real-time clock driver.
 *
 * The driver is one coroutine, and it IS the entry point: registration, the
 * ready notification and the request loop read top to bottom, and every point
 * where it waits is a call that suspends. Because initialize() is exported,
 * tools/as_coroutine_transform.mjs keeps its signature and hands the lowered
 * task to libc's pump, which owns the coroutine runtime.
 *
 * Registration goes through the event loop, not a blocking ipc_recv on the
 * SERVICE endpoint: send-then-receive-until-the-reply-arrives discards every
 * unrelated message, so a client read request landing in that window is lost
 * and its sender waits forever. Failure paths return packed driver-domain codes
 * across the entry-point boundary, never a bare -1.
 */

import {io, std, startup} from "./wasmos";
import {defaultLoop, EventLoop, IpcFuture, IpcMessage, ReplyStatus} from "./eventloop";
import {AWAIT_PENDING, Box} from "./coroutine";
import {
    WASMOS_ERR_DRIVER_ENDPOINT_CREATE,
    WASMOS_ERR_DRIVER_REGISTER,
    WASMOS_ERR_NONE,
    WASMOS_ERR_RTC_INVALID,
    WASMOS_ERR_RTC_TIMEOUT,
} from "./wasmos_status";
import {io_out8, io_wait, ipc_create_endpoint, ipc_send} from "./wasmos_imports";

/* MC146818-compatible CMOS/RTC. Write a register index to 0x70, then read or
 * write its value at 0x71; every access is that two-step pair, which is why a
 * read here is not idempotent from the hardware's point of view.
 *
 * Bit 7 of the index port is the NMI-disable bit, so an index must be masked
 * with 0x7F or selecting a register would disable NMI as a side effect.
 *
 * The registers this driver touches: 0x00 seconds, 0x02 minutes, 0x04 hours,
 * 0x07 day of month, 0x08 month, 0x09 year (two digits). Status register A
 * (0x0A) bit 7 is UIP, set while an update is in progress and the time fields
 * are not coherent. Status register B (0x0B) bit 1 selects 24-hour mode, bit 2
 * selects binary rather than BCD encoding, and bit 7 (SET) halts the update
 * cycle so a multi-register write lands as one consistent time. */
const CMOS_INDEX_PORT: i32 = 0x70;
const CMOS_DATA_PORT: i32 = 0x71;

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
/* One scratch record, reused. `--runtime stub` is a bump allocator with no
 * collector, so a StaticArray allocated per request would leak for the life of
 * a driver that runs forever. */
const g_values: StaticArray<i32> = new StaticArray<i32>(6);

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

// ---------------------------------------------------------------------- CMOS

/* Returns the register byte, or a negative WASMOS_ERR_IO_* code. Reporting the
 * refusal matters: a refused read surfacing as 0xFF decodes into a
 * plausible-looking date. */
function rtcReadReg(reg: i32): i32 {
    /* Bit 7 of the index port is the NMI-disable bit; masking it off selects the
     * register without disabling NMI as a side effect. */
    io_out8(CMOS_INDEX_PORT, reg & 0x7f);
    io_wait();
    return io.in8(CMOS_DATA_PORT);
}

function rtcWriteReg(reg: i32, value: i32): void {
    io_out8(CMOS_INDEX_PORT, reg & 0x7f);
    io_wait();
    io_out8(CMOS_DATA_PORT, value & 0xff);
    io_wait();
}

function bcdToBin(v: i32): i32 {
    return (v >> 4) * 10 + (v & 0x0f);
}

function binToBcd(v: i32): i32 {
    return (((v / 10) & 0x0f) << 4) | (v % 10);
}

/* Bounded hardware wait on the update-in-progress flag: there is nothing to
 * park on, and the bound is the timeout. */
function waitNotUpdating(): i32 {
    for (let i = 0; i < 10000; ++i) {
        const a = rtcReadReg(0x0a);
        /* A refused read will be refused every time: report it, do not spin
         * out the bound and then call it a timeout. */
        if (a < 0) {
            return a;
        }
        if ((a & 0x80) == 0) {
            return WASMOS_ERR_NONE;
        }
        io_wait();
    }
    return WASMOS_ERR_RTC_TIMEOUT;
}

function unpackTime(arg0: i32, arg1: i32, outVals: StaticArray<i32>): void {
    unchecked((outVals[0] = arg0 & 0xff));
    unchecked((outVals[1] = (arg0 >> 8) & 0xff));
    unchecked((outVals[2] = (arg0 >> 16) & 0xff));
    unchecked((outVals[3] = (arg0 >> 24) & 0xff));
    unchecked((outVals[4] = arg1 & 0xff));
    unchecked((outVals[5] = (arg1 >> 8) & 0xffff));
}

function packTime(outVals: StaticArray<i32>): i32 {
    return (
        (unchecked(outVals[0]) & 0xff) |
        ((unchecked(outVals[1]) & 0xff) << 8) |
        ((unchecked(outVals[2]) & 0xff) << 16) |
        ((unchecked(outVals[3]) & 0xff) << 24)
    );
}

function packDate(outVals: StaticArray<i32>): i32 {
    return (unchecked(outVals[4]) & 0xff) | ((unchecked(outVals[5]) & 0xffff) << 8);
}

function validateTime(vals: StaticArray<i32>): bool {
    const sec = unchecked(vals[0]);
    const min = unchecked(vals[1]);
    const hour = unchecked(vals[2]);
    const day = unchecked(vals[3]);
    const mon = unchecked(vals[4]);
    const year = unchecked(vals[5]);
    return (
        sec >= 0 &&
        sec <= 59 &&
        min >= 0 &&
        min <= 59 &&
        hour >= 0 &&
        hour <= 23 &&
        day >= 1 &&
        day <= 31 &&
        mon >= 1 &&
        mon <= 12 &&
        year >= 1970 &&
        year <= 2099
    );
}

function readTime(outVals: StaticArray<i32>): i32 {
    const waitStatus = waitNotUpdating();
    if (waitStatus != WASMOS_ERR_NONE) {
        return waitStatus;
    }

    let sec = rtcReadReg(0x00);
    let min = rtcReadReg(0x02);
    let hour = rtcReadReg(0x04);
    let day = rtcReadReg(0x07);
    let mon = rtcReadReg(0x08);
    let year = rtcReadReg(0x09);
    const regB = rtcReadReg(0x0b);

    /* One refused read poisons the whole reading, so none of them is decoded
     * until all seven are known good. */
    if (sec < 0) {
        return sec;
    }
    if (min < 0) {
        return min;
    }
    if (hour < 0) {
        return hour;
    }
    if (day < 0) {
        return day;
    }
    if (mon < 0) {
        return mon;
    }
    if (year < 0) {
        return year;
    }
    if (regB < 0) {
        return regB;
    }

    const isBinary = (regB & 0x04) != 0;
    const is24Hour = (regB & 0x02) != 0;

    if (!isBinary) {
        sec = bcdToBin(sec);
        min = bcdToBin(min);
        const hourRaw = hour;
        hour = bcdToBin(hourRaw & 0x7f);
        if (!is24Hour && (hourRaw & 0x80) != 0) {
            hour = (hour + 12) % 24;
        }
        day = bcdToBin(day);
        mon = bcdToBin(mon);
        year = bcdToBin(year);
    } else if (!is24Hour && (hour & 0x80) != 0) {
        hour = ((hour & 0x7f) + 12) % 24;
    }
    /* FIXME: 12-hour mode maps the two noon/midnight hours wrongly in both
     * branches above -- 12 PM decodes as 0 and 12 AM as 12 -- because a stored
     * 12 means 0 in 24-hour terms. Unreached on the RTCs this runs on (register
     * B reports 24-hour mode), so a report would be off by 12 hours only on a
     * machine configured for 12-hour time. */

    let fullYear = 2000 + year;
    if (year >= 70) {
        fullYear = 1900 + year;
    }

    unchecked((outVals[0] = sec));
    unchecked((outVals[1] = min));
    unchecked((outVals[2] = hour));
    unchecked((outVals[3] = day));
    unchecked((outVals[4] = mon));
    unchecked((outVals[5] = fullYear));
    return WASMOS_ERR_NONE;
}

function setTime(vals: StaticArray<i32>): i32 {
    if (!validateTime(vals)) {
        return WASMOS_ERR_RTC_INVALID;
    }
    const waitStatus = waitNotUpdating();
    if (waitStatus != WASMOS_ERR_NONE) {
        return waitStatus;
    }

    let sec = unchecked(vals[0]);
    let min = unchecked(vals[1]);
    let day = unchecked(vals[3]);
    let mon = unchecked(vals[4]);
    const hour = unchecked(vals[2]);
    const fullYear = unchecked(vals[5]);
    let year = fullYear % 100;

    const regB = rtcReadReg(0x0b);
    if (regB < 0) {
        return regB;
    }
    const isBinary = (regB & 0x04) != 0;
    const is24Hour = (regB & 0x02) != 0;

    let hourReg = hour;
    if (!is24Hour) {
        const isPm = hour >= 12;
        let h12 = hour % 12;
        if (h12 == 0) {
            h12 = 12;
        }
        hourReg = h12;
        if (isPm) {
            hourReg |= 0x80;
        }
    }

    if (!isBinary) {
        sec = binToBcd(sec);
        min = binToBcd(min);
        day = binToBcd(day);
        mon = binToBcd(mon);
        year = binToBcd(year);
        if (is24Hour) {
            hourReg = binToBcd(hourReg);
        } else {
            const pmBit = hourReg & 0x80;
            hourReg = binToBcd(hourReg & 0x7f) | pmBit;
        }
    }

    /* Register B bit 7 (SET) halts the update cycle, so the seven writes below
     * land as one consistent time; clearing it again resumes counting. */
    rtcWriteReg(0x0b, regB | 0x80);
    rtcWriteReg(0x00, sec);
    rtcWriteReg(0x02, min);
    rtcWriteReg(0x04, hourReg);
    rtcWriteReg(0x07, day);
    rtcWriteReg(0x08, mon);
    rtcWriteReg(0x09, year);
    rtcWriteReg(0x0b, regB & 0x7f);
    return WASMOS_ERR_NONE;
}

// ------------------------------------------------------------------ protocol

function serveRequest(msg: IpcMessage): void {
    if (msg.source < 0) {
        return;
    }

    if (msg.type == RTC_IPC_READ_REQ) {
        const rc = readTime(g_values);
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
        const rc = setTime(g_values);
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
export function initialize(): i32 {
    // proc.endpoint comes from the spawn-info contract, not an entry arg.
    let procEndpoint: i32 = startup.procEndpoint();

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

    std.printf("[rtc] driver ready\n");

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
