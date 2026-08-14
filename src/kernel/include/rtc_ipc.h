/* rtc_ipc.h - IPC message types for the real-time clock service (kernel copy). */
#ifndef WASMOS_KERNEL_RTC_IPC_H
#define WASMOS_KERNEL_RTC_IPC_H

#include <stdint.h>

/* RTC_IPC_* opcodes come from the generated IPC opcode ABI (abi/opcodes.yaml). */
#include "../../../abi/generated/c/wasmos_opcodes.h"
#include "../../../abi/generated/c/wasmos_status.h"

/* Status codes are the packed rtc domain in abi/errors.yaml:
 * WASMOS_ERR_NONE (0) on success, else a negative WASMOS_ERR_RTC_*. */

/* RTC v1 payload contract:
 * - RTC_IPC_READ_REQ:
 *   arg0..arg3 reserved (must be zero)
 * - RTC_IPC_SET_REQ:
 *   arg0: [7:0]=sec [15:8]=min [23:16]=hour(24h) [31:24]=day
 *   arg1: [7:0]=month [23:8]=year (full year, e.g. 2026)
 *   arg2/arg3 reserved
 * - RTC_IPC_READ_RESP:
 *   arg0/arg1 same packed format as SET_REQ
 *   arg2/arg3 reserved
 * - RTC_IPC_SET_RESP:
 *   arg0=status (0 on success), arg1..arg3 reserved
 * - RTC_IPC_ERROR:
 *   arg0=status (<0), arg1..arg3 reserved
 */

/* Wall-clock time as the RTC service exchanges it.  second/minute are 0-59, hour is 0-23
 * (24-hour clock, never 12-hour or BCD), day is 1-31, month is 1-12, and year is the full
 * year (2026, not 26).  There is no sub-second field, no timezone and no DST flag: the
 * value is whatever the hardware clock holds. */
typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} rtc_ipc_time_t;

/* Pack an rtc_ipc_time_t into the two IPC arguments described above, and unpack them
 * back.  The pack helpers return 0 for a NULL time and the unpack helper ignores a NULL
 * output, so a NULL is indistinguishable from a genuine all-zero time.  Fields are masked
 * to their widths rather than validated: an out-of-range month or year is truncated, not
 * rejected.  Round-tripping is lossless for in-range values. */
static inline int32_t rtc_ipc_pack_time_arg0(const rtc_ipc_time_t* t) {
    if (!t) {
        return 0;
    }
    return (int32_t)(((uint32_t)t->second & 0xFFu) | (((uint32_t)t->minute & 0xFFu) << 8) |
                     (((uint32_t)t->hour & 0xFFu) << 16) | (((uint32_t)t->day & 0xFFu) << 24));
}

static inline int32_t rtc_ipc_pack_time_arg1(const rtc_ipc_time_t* t) {
    if (!t) {
        return 0;
    }
    return (int32_t)(((uint32_t)t->month & 0xFFu) | (((uint32_t)t->year & 0xFFFFu) << 8));
}

static inline void rtc_ipc_unpack_time(int32_t arg0, int32_t arg1, rtc_ipc_time_t* out) {
    if (!out) {
        return;
    }
    out->second = (uint8_t)((uint32_t)arg0 & 0xFFu);
    out->minute = (uint8_t)(((uint32_t)arg0 >> 8) & 0xFFu);
    out->hour = (uint8_t)(((uint32_t)arg0 >> 16) & 0xFFu);
    out->day = (uint8_t)(((uint32_t)arg0 >> 24) & 0xFFu);
    out->month = (uint8_t)((uint32_t)arg1 & 0xFFu);
    out->year = (uint16_t)(((uint32_t)arg1 >> 8) & 0xFFFFu);
}

#endif
