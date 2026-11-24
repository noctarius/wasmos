/* cmos.ts - the MC146818-compatible CMOS clock backend (x86_64). The driver
 * around it is rtc_core.ts; this file is the chip.
 *
 * Write a register index to 0x70, then read or write its value at 0x71; every
 * access is that two-step pair, which is why a read here is not idempotent from
 * the hardware's point of view.
 *
 * Bit 7 of the index port is the NMI-disable bit, so an index must be masked
 * with 0x7F or selecting a register would disable NMI as a side effect.
 *
 * The registers this backend touches: 0x00 seconds, 0x02 minutes, 0x04 hours,
 * 0x07 day of month, 0x08 month, 0x09 year (two digits). Status register A
 * (0x0A) bit 7 is UIP, set while an update is in progress and the time fields
 * are not coherent. Status register B (0x0B) bit 1 selects 24-hour mode, bit 2
 * selects binary rather than BCD encoding, and bit 7 (SET) halts the update
 * cycle so a multi-register write lands as one consistent time. */

import {io} from "./wasmos";
import {WASMOS_ERR_NONE, WASMOS_ERR_RTC_TIMEOUT} from "./wasmos_status";
import {io_out8, io_wait} from "./wasmos_imports";
import {runRtc, RtcClock} from "./rtc_core";
import {RTC_DAY, RTC_HOUR, RTC_MIN, RTC_MON, RTC_SEC, RTC_YEAR} from "./rtc_time";

const CMOS_INDEX_PORT: i32 = 0x70;
const CMOS_DATA_PORT: i32 = 0x71;

/* Returns the register byte, or a negative WASMOS_ERR_IO_* code. Reporting the
 * refusal matters: a refused read surfacing as 0xFF decodes into a
 * plausible-looking date. */
function rtcReadReg(reg: i32): i32 {
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

class Cmos extends RtcClock {
    label(): string {
        return "cmos";
    }

    init(): i32 {
        return WASMOS_ERR_NONE;
    }

    read(out: StaticArray<i32>): i32 {
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

        unchecked((out[RTC_SEC] = sec));
        unchecked((out[RTC_MIN] = min));
        unchecked((out[RTC_HOUR] = hour));
        unchecked((out[RTC_DAY] = day));
        unchecked((out[RTC_MON] = mon));
        unchecked((out[RTC_YEAR] = fullYear));
        return WASMOS_ERR_NONE;
    }

    set(vals: StaticArray<i32>): i32 {
        const waitStatus = waitNotUpdating();
        if (waitStatus != WASMOS_ERR_NONE) {
            return waitStatus;
        }

        let sec = unchecked(vals[RTC_SEC]);
        let min = unchecked(vals[RTC_MIN]);
        let day = unchecked(vals[RTC_DAY]);
        let mon = unchecked(vals[RTC_MON]);
        const hour = unchecked(vals[RTC_HOUR]);
        const fullYear = unchecked(vals[RTC_YEAR]);
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
}

export function initialize(): i32 {
    return runRtc(new Cmos());
}
