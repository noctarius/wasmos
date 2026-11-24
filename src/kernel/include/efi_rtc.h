/* efi_rtc.h - the clock through UEFI runtime services (GetTime/SetTime).
 *
 * Available only when the boot loader handed the runtime services table over
 * (BOOT_INFO_FLAG_EFI_RUNTIME_PRESENT, Kconfig WASMOS_RTC_EFI): it has called
 * SetVirtualAddressMap so the firmware's runtime regions run at their kernel
 * direct-map addresses. The rtc_efi driver reaches these through the
 * rtc_efi_get/rtc_efi_set host calls (abi/hostcalls.yaml), gated on the rtc.efi
 * capability.
 *
 * A time is six int32 fields in the rtc protocol's order: seconds, minutes,
 * hours, day of month, month, full year (src/drivers/rtc/core/rtc_time.ts). */
#ifndef WASMOS_EFI_RTC_H
#define WASMOS_EFI_RTC_H

#include <stdint.h>

#include "boot.h"

#define EFI_RTC_FIELDS 6u

/* Record the runtime services table from boot_info. Without
 * BOOT_INFO_FLAG_EFI_RUNTIME_PRESENT every call below answers
 * WASMOS_ERR_RTC_UNAVAILABLE. */
void efi_rtc_init(const boot_info_t* info);

/* Read the firmware clock into out[EFI_RTC_FIELDS]. Returns 0,
 * WASMOS_ERR_RTC_UNAVAILABLE, or WASMOS_ERR_RTC_FIRMWARE when GetTime fails;
 * `out` is written only on success. */
int32_t efi_rtc_get(int32_t* out);

/* Set the firmware clock from in[EFI_RTC_FIELDS]. Returns 0,
 * WASMOS_ERR_RTC_INVALID for a field out of range (years 1970-2099, the day
 * within 1-31), WASMOS_ERR_RTC_UNAVAILABLE, or WASMOS_ERR_RTC_FIRMWARE when
 * SetTime fails. */
int32_t efi_rtc_set(const int32_t* in);

#endif
