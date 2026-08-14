/* rtc_ipc.h - IPC message types and helpers for the real-time clock service. */
#ifndef WASMOS_LIBC_WASMOS_RTC_IPC_H
#define WASMOS_LIBC_WASMOS_RTC_IPC_H

#include <stdint.h>

/* RTC_IPC_* opcodes come from the generated IPC opcode ABI (abi/opcodes.yaml). */
#include "../../../../../abi/generated/c/wasmos_opcodes.h"
#include "../../../../../abi/generated/c/wasmos_status.h"

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

/* Unpacked wall-clock time as the RTC service exchanges it: 0-59 second and
 * minute, 0-23 hour, 1-31 day, 1-12 month, and a full year (e.g. 2026). No
 * timezone, no sub-second resolution; the helpers below neither validate nor
 * normalise these ranges. */
typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} rtc_ipc_time_t;

/* Pack the time-of-day half (second, minute, hour, day) into arg0 per the v1
 * payload contract above. Returns 0 for a NULL argument, which is also a
 * legal-looking encoding, so check the pointer rather than the result. */
static inline int32_t rtc_ipc_pack_time_arg0(const rtc_ipc_time_t* t) {
    if (!t) {
        return 0;
    }
    return (int32_t)(((uint32_t)t->second & 0xFFu) | (((uint32_t)t->minute & 0xFFu) << 8) |
                     (((uint32_t)t->hour & 0xFFu) << 16) | (((uint32_t)t->day & 0xFFu) << 24));
}

/* Pack the date half (month, year) into arg1. The year occupies 16 bits, so it
 * is truncated modulo 65536; returns 0 for a NULL argument. */
static inline int32_t rtc_ipc_pack_time_arg1(const rtc_ipc_time_t* t) {
    if (!t) {
        return 0;
    }
    return (int32_t)(((uint32_t)t->month & 0xFFu) | (((uint32_t)t->year & 0xFFFFu) << 8));
}

/* Inverse of the two packers; writes every field of *out. A NULL out is
 * ignored. The bit fields are copied verbatim, so a malformed message yields
 * out-of-range values rather than an error. */
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
