#ifndef WASMOS_KERNEL_RTC_IPC_H
#define WASMOS_KERNEL_RTC_IPC_H

#include <stdint.h>

enum {
    RTC_IPC_READ_REQ  = 0x820,
    RTC_IPC_SET_REQ   = 0x821,
    RTC_IPC_READ_RESP = 0x8A0,
    RTC_IPC_SET_RESP  = 0x8A1,
    RTC_IPC_ERROR     = 0x8FF
};

enum {
    RTC_STATUS_OK = 0,
    RTC_STATUS_INVALID = -1,
    RTC_STATUS_IO = -2,
    RTC_STATUS_TIMEOUT = -3,
    RTC_STATUS_DENIED = -4
};

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

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} rtc_ipc_time_t;

static inline int32_t
rtc_ipc_pack_time_arg0(const rtc_ipc_time_t *t)
{
    if (!t) {
        return 0;
    }
    return (int32_t)(((uint32_t)t->second & 0xFFu) |
                     (((uint32_t)t->minute & 0xFFu) << 8) |
                     (((uint32_t)t->hour & 0xFFu) << 16) |
                     (((uint32_t)t->day & 0xFFu) << 24));
}

static inline int32_t
rtc_ipc_pack_time_arg1(const rtc_ipc_time_t *t)
{
    if (!t) {
        return 0;
    }
    return (int32_t)(((uint32_t)t->month & 0xFFu) |
                     (((uint32_t)t->year & 0xFFFFu) << 8));
}

static inline void
rtc_ipc_unpack_time(int32_t arg0, int32_t arg1, rtc_ipc_time_t *out)
{
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
