/* rtc_efi.ts - the firmware clock backend: UEFI GetTime/SetTime through the
 * kernel (src/kernel/efi_rtc.c), reached with the rtc_efi_get/rtc_efi_set host
 * calls the rtc.efi capability gates. The driver around it is
 * src/drivers/rtc/core/rtc_core.ts; this file only moves the six fields.
 *
 * Selected in Kconfig (WASMOS_RTC_EFI): only then does the boot loader hand
 * the runtime services over, and only then does a boot rule start this
 * driver. Without the handover every call answers rtc.UNAVAILABLE, and init
 * fails so no "rtc" is registered. */

import {rtc_efi_get, rtc_efi_set} from "./wasmos_imports";
import {WASMOS_ERR_NONE} from "./wasmos_status";
import {runRtc, RtcClock} from "./rtc_core";
import {RTC_FIELDS} from "./rtc_time";

/* The host calls read and write linear memory; a StaticArray's data does not
 * move, and one buffer serves every call. */
const g_buf: StaticArray<i32> = new StaticArray<i32>(RTC_FIELDS);
const BUF_BYTES: i32 = RTC_FIELDS * 4;

class EfiClock extends RtcClock {
    label(): string {
        return "efi";
    }

    /* A read is the only way to learn whether the firmware clock is there. */
    init(): i32 {
        return rtc_efi_get(<i32>changetype<usize>(g_buf), BUF_BYTES);
    }

    read(out: StaticArray<i32>): i32 {
        const rc = rtc_efi_get(<i32>changetype<usize>(g_buf), BUF_BYTES);
        if (rc != WASMOS_ERR_NONE) {
            return rc;
        }
        for (let i = 0; i < RTC_FIELDS; ++i) {
            unchecked((out[i] = g_buf[i]));
        }
        return WASMOS_ERR_NONE;
    }

    set(vals: StaticArray<i32>): i32 {
        for (let i = 0; i < RTC_FIELDS; ++i) {
            unchecked((g_buf[i] = vals[i]));
        }
        return rtc_efi_set(<i32>changetype<usize>(g_buf), BUF_BYTES);
    }
}

export function initialize(): i32 {
    return runRtc(new EfiClock());
}
