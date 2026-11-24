/* link_rtc.cpp - WARP admission check for the firmware-clock host calls. See
 * link_rtc.h. */
#include "warp/link_rtc.h"
#include "warp/link_ipc.h"

extern "C" {
#include "capability.h"
#include "efi_rtc.h"
#include "wasmos_status.h"
}

uint32_t warp_rtc_efi_admit(uint32_t len) {
    uint32_t context_id = 0;
    /* Compared as the guest's i32: a negative length is short, not huge. */
    if ((int32_t)len < (int32_t)(EFI_RTC_FIELDS * sizeof(int32_t))) {
        return (uint32_t)WASMOS_INVAL;
    }
    if (warp_current_context_id(&context_id) != 0 || !capability_has(context_id, CAP_RTC_EFI)) {
        return (uint32_t)WASMOS_ERR_KERNEL_NOT_AUTHORIZED;
    }
    return 0;
}
