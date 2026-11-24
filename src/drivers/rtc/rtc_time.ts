/* rtc_time.ts - the clock interface an RTC backend implements, and the date
 * arithmetic every backend shares. No host calls: tests/unit/test_as_rtc_time.ts
 * exercises it on the host.
 *
 * A time is six i32 fields in one record, in this order: seconds (0-59),
 * minutes (0-59), hours (0-23), day of month (1-31), month (1-12) and the full
 * year. It is UTC: no backend applies a time zone. On the wire (abi/opcodes.yaml
 * `rtc`) it travels as two words, packTime and packDate.
 */

export const RTC_SEC: i32 = 0;
export const RTC_MIN: i32 = 1;
export const RTC_HOUR: i32 = 2;
export const RTC_DAY: i32 = 3;
export const RTC_MON: i32 = 4;
export const RTC_YEAR: i32 = 5;
export const RTC_FIELDS: i32 = 6;

/* The years a time may name. The CMOS clock stores two digits and is read as
 * 1970-2069 or 2000-2099; every backend accepts the same range so a time set
 * through one reads back through any other. */
export const RTC_YEAR_MIN: i32 = 1970;
export const RTC_YEAR_MAX: i32 = 2099;

/**
 * One clock chip. Both calls return WASMOS_ERR_NONE or a negative packed
 * error code; read() fills all six fields only on success, and set() is called
 * only with a time validateTime() accepted.
 */
export abstract class RtcClock {
    /** How the boot log names the clock ("cmos", "pl031"). */
    abstract label(): string;
    /** Bring the clock up before the first request; WASMOS_ERR_NONE or a code. */
    abstract init(): i32;
    abstract read(out: StaticArray<i32>): i32;
    abstract set(vals: StaticArray<i32>): i32;
}

export function unpackTime(arg0: i32, arg1: i32, out: StaticArray<i32>): void {
    unchecked((out[RTC_SEC] = arg0 & 0xff));
    unchecked((out[RTC_MIN] = (arg0 >> 8) & 0xff));
    unchecked((out[RTC_HOUR] = (arg0 >> 16) & 0xff));
    unchecked((out[RTC_DAY] = (arg0 >> 24) & 0xff));
    unchecked((out[RTC_MON] = arg1 & 0xff));
    unchecked((out[RTC_YEAR] = (arg1 >> 8) & 0xffff));
}

export function packTime(vals: StaticArray<i32>): i32 {
    return (
        (unchecked(vals[RTC_SEC]) & 0xff) |
        ((unchecked(vals[RTC_MIN]) & 0xff) << 8) |
        ((unchecked(vals[RTC_HOUR]) & 0xff) << 16) |
        ((unchecked(vals[RTC_DAY]) & 0xff) << 24)
    );
}

export function packDate(vals: StaticArray<i32>): i32 {
    return (unchecked(vals[RTC_MON]) & 0xff) | ((unchecked(vals[RTC_YEAR]) & 0xffff) << 8);
}

function isLeapYear(year: i32): bool {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

/** Days in `month` (1-12) of `year`. */
export function daysInMonth(year: i32, month: i32): i32 {
    if (month == 2) {
        return isLeapYear(year) ? 29 : 28;
    }
    return month == 4 || month == 6 || month == 9 || month == 11 ? 30 : 31;
}

/** Whether every field is in range, the day included for its month and year. */
export function validateTime(vals: StaticArray<i32>): bool {
    const sec = unchecked(vals[RTC_SEC]);
    const min = unchecked(vals[RTC_MIN]);
    const hour = unchecked(vals[RTC_HOUR]);
    const day = unchecked(vals[RTC_DAY]);
    const mon = unchecked(vals[RTC_MON]);
    const year = unchecked(vals[RTC_YEAR]);
    if (sec < 0 || sec > 59 || min < 0 || min > 59 || hour < 0 || hour > 23) {
        return false;
    }
    if (mon < 1 || mon > 12 || year < RTC_YEAR_MIN || year > RTC_YEAR_MAX) {
        return false;
    }
    return day >= 1 && day <= daysInMonth(year, mon);
}

/* Days since 1970-01-01 of a proleptic Gregorian date, counted in 400-year
 * eras that start on March 1st so the leap day ends each era's year (H. Hinnant,
 * "chrono-Compatible Low-Level Date Algorithms", days_from_civil). Valid for
 * every year this file accepts. */
function daysFromCivil(year: i32, month: i32, day: i32): i64 {
    const y: i64 = <i64>(month <= 2 ? year - 1 : year);
    const era: i64 = (y >= 0 ? y : y - 399) / 400;
    const yoe: i64 = y - era * 400;
    const mp: i64 = <i64>(month > 2 ? month - 3 : month + 9);
    const doy: i64 = (153 * mp + 2) / 5 + <i64>day - 1;
    const doe: i64 = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/** Seconds since 1970-01-01 00:00:00 UTC of a time validateTime() accepts. */
export function civilToSeconds(vals: StaticArray<i32>): i64 {
    const days = daysFromCivil(
        unchecked(vals[RTC_YEAR]),
        unchecked(vals[RTC_MON]),
        unchecked(vals[RTC_DAY]),
    );
    return (
        days * 86400 +
        <i64>unchecked(vals[RTC_HOUR]) * 3600 +
        <i64>unchecked(vals[RTC_MIN]) * 60 +
        <i64>unchecked(vals[RTC_SEC])
    );
}

/**
 * The time `seconds` after 1970-01-01 00:00:00 UTC, into `out`: the inverse of
 * civilToSeconds (civil_from_days in the same paper). `seconds` must be
 * non-negative. The year is not range-checked: validateTime() decides whether
 * a backend may report it.
 */
export function secondsToCivil(seconds: i64, out: StaticArray<i32>): void {
    const days: i64 = seconds / 86400;
    const rem: i64 = seconds % 86400;
    const z: i64 = days + 719468;
    const era: i64 = z / 146097;
    const doe: i64 = z - era * 146097;
    const yoe: i64 = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const doy: i64 = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const mp: i64 = (5 * doy + 2) / 153;
    const day: i64 = doy - (153 * mp + 2) / 5 + 1;
    const month: i64 = mp < 10 ? mp + 3 : mp - 9;
    const year: i64 = yoe + era * 400 + (month <= 2 ? 1 : 0);
    unchecked((out[RTC_SEC] = <i32>(rem % 60)));
    unchecked((out[RTC_MIN] = <i32>((rem / 60) % 60)));
    unchecked((out[RTC_HOUR] = <i32>(rem / 3600)));
    unchecked((out[RTC_DAY] = <i32>day));
    unchecked((out[RTC_MON] = <i32>month));
    unchecked((out[RTC_YEAR] = <i32>year));
}
