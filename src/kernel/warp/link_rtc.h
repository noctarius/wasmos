/* link_rtc.h - Internal seam between link.cpp and link_rtc.cpp.
 *
 * The admission check of the rtc_efi_get / rtc_efi_set host calls
 * (hostcalls.yaml), split out because link.cpp carries the whole WARP engine
 * and cannot be built for a host test (tests/unit/test_warp_link_rtc.cpp).
 * link.cpp resolves the guest buffer once a call is admitted.
 */
#ifndef WASMOS_WARP_LINK_RTC_H
#define WASMOS_WARP_LINK_RTC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Whether the calling process may call the firmware clock with a buffer of
 * `len` bytes. `len` is the guest's i32 widened to uint32_t. Returns 0,
 * WASMOS_INVAL for a length below EFI_RTC_FIELDS int32 fields, or
 * WASMOS_ERR_KERNEL_NOT_AUTHORIZED without the rtc.efi capability -- the codes
 * the wasm3 host calls return for the same arguments. */
uint32_t warp_rtc_efi_admit(uint32_t len);

#ifdef __cplusplus
}
#endif

#endif
