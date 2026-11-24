/* test_as_rtc_time.ts - the date arithmetic every RTC backend shares
 * (src/drivers/rtc/core/rtc_time.ts): the PL031 counts seconds since the epoch,
 * the rtc protocol carries a date, and the two must convert exactly both ways.
 * Reference values are from Python's calendar.timegm.
 */

import {
    civilToSeconds,
    packDate,
    packTime,
    RTC_DAY,
    RTC_FIELDS,
    RTC_HOUR,
    RTC_MIN,
    RTC_MON,
    RTC_SEC,
    RTC_YEAR,
    secondsToCivil,
    unpackTime,
    validateTime,
} from "./rtc_time";
import {runShuffled, TestCase} from "./shuffle";

const g_t: StaticArray<i32> = new StaticArray<i32>(RTC_FIELDS);
const g_u: StaticArray<i32> = new StaticArray<i32>(RTC_FIELDS);

function setTime(year: i32, mon: i32, day: i32, hour: i32, min: i32, sec: i32): void {
    g_t[RTC_YEAR] = year;
    g_t[RTC_MON] = mon;
    g_t[RTC_DAY] = day;
    g_t[RTC_HOUR] = hour;
    g_t[RTC_MIN] = min;
    g_t[RTC_SEC] = sec;
}

function isTime(
    t: StaticArray<i32>,
    year: i32,
    mon: i32,
    day: i32,
    hour: i32,
    min: i32,
    sec: i32,
): bool {
    return (
        t[RTC_YEAR] == year &&
        t[RTC_MON] == mon &&
        t[RTC_DAY] == day &&
        t[RTC_HOUR] == hour &&
        t[RTC_MIN] == min &&
        t[RTC_SEC] == sec
    );
}

function testKnownInstantsConvertBothWays(): i32 {
    secondsToCivil(0, g_u);
    if (!isTime(g_u, 1970, 1, 1, 0, 0, 0)) return 101;
    secondsToCivil(951782400, g_u);
    if (!isTime(g_u, 2000, 2, 29, 0, 0, 0)) return 102;
    secondsToCivil(1735689599, g_u);
    if (!isTime(g_u, 2024, 12, 31, 23, 59, 59)) return 103;
    secondsToCivil(2147483648, g_u);
    if (!isTime(g_u, 2038, 1, 19, 3, 14, 8)) return 104;
    secondsToCivil(4294967295, g_u);
    if (!isTime(g_u, 2106, 2, 7, 6, 28, 15)) return 105;

    setTime(2000, 2, 29, 0, 0, 0);
    if (civilToSeconds(g_t) != 951782400) return 111;
    setTime(2099, 12, 31, 23, 59, 59);
    if (civilToSeconds(g_t) != 4102444799) return 112;
    return 0;
}

/* Every day the protocol can name, at a time that exercises every field. */
function testEveryAcceptedDayRoundTrips(): i32 {
    for (let day: i64 = 0; day < 47482; ++day) {
        const seconds: i64 = day * 86400 + 13 * 3600 + 37 * 60 + 42;
        secondsToCivil(seconds, g_u);
        if (!validateTime(g_u)) return 201;
        if (civilToSeconds(g_u) != seconds) return 202;
    }
    return 0;
}

function testTheYearRangeIsEnforced(): i32 {
    secondsToCivil(4102444800, g_u);
    if (!isTime(g_u, 2100, 1, 1, 0, 0, 0)) return 301;
    if (validateTime(g_u)) return 302;
    setTime(1969, 12, 31, 23, 59, 59);
    if (validateTime(g_t)) return 303;
    return 0;
}

function testADayMustExistInItsMonth(): i32 {
    setTime(2024, 2, 29, 12, 0, 0);
    if (!validateTime(g_t)) return 401;
    setTime(2023, 2, 29, 12, 0, 0);
    if (validateTime(g_t)) return 402;
    setTime(2000, 2, 29, 12, 0, 0);
    if (!validateTime(g_t)) return 403;
    setTime(2024, 4, 31, 12, 0, 0);
    if (validateTime(g_t)) return 404;
    setTime(2024, 1, 0, 12, 0, 0);
    if (validateTime(g_t)) return 405;
    setTime(2024, 13, 1, 12, 0, 0);
    if (validateTime(g_t)) return 406;
    setTime(2024, 1, 1, 24, 0, 0);
    if (validateTime(g_t)) return 407;
    return 0;
}

function testTheWireWordsRoundTrip(): i32 {
    setTime(2099, 12, 31, 23, 59, 58);
    unpackTime(packTime(g_t), packDate(g_t), g_u);
    if (!isTime(g_u, 2099, 12, 31, 23, 59, 58)) return 501;
    return 0;
}

/** 0 if every case passed, else the failing case's marker. */
export function runTests(): i32 {
    const cases: StaticArray<TestCase> = [
        testKnownInstantsConvertBothWays,
        testEveryAcceptedDayRoundTrips,
        testTheYearRangeIsEnforced,
        testADayMustExistInItsMonth,
        testTheWireWordsRoundTrip,
    ];
    return runShuffled(cases);
}
