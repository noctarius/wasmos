/* efi_rtc.c - the clock through UEFI runtime services. See efi_rtc.h.
 *
 * The runtime services are not reentrant (UEFI 2.10, 8.1): one call at a time,
 * serialised by g_efi_rtc_lock, which also masks interrupts and preemption for
 * the call's duration. The firmware code runs at EL1 / ring 0 in the caller's
 * context, on the caller's kernel stack, through the direct map the loader
 * named in SetVirtualAddressMap. */
#include "efi_rtc.h"

#include "klog.h"
#include "sync/spinlock.h"
#include "wasmos_status.h"

/* The firmware's calling convention: Microsoft x64 on x86_64, AAPCS64 on
 * aarch64 (UEFI 2.10, 2.3.4 and 2.3.6), as the loader's uefi.h declares it. */
#if defined(WASMOS_ARCH_AARCH64)
#define EFIAPI
#else
#define EFIAPI __attribute__((ms_abi))
#endif

#define EFI_UNSPECIFIED_TIMEZONE 0x07FF

/* EFI_TIME (UEFI 2.10, 8.3.1). */
typedef struct {
    uint16_t year; /* 1900-9999 */
    uint8_t month; /* 1-12 */
    uint8_t day;   /* 1-31 */
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t pad1;
    uint32_t nanosecond;
    int16_t time_zone; /* minutes from UTC, or EFI_UNSPECIFIED_TIMEZONE */
    uint8_t daylight;
    uint8_t pad2;
} efi_time_t;

typedef uint64_t efi_status_t;

/* The head of EFI_RUNTIME_SERVICES (UEFI 2.10, 4.5): the 24-byte table header,
 * then GetTime and SetTime. Nothing past them is called. */
typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
    efi_status_t(EFIAPI* get_time)(efi_time_t* time, void* capabilities);
    efi_status_t(EFIAPI* set_time)(efi_time_t* time);
} efi_runtime_services_t;

#define EFI_RUNTIME_SERVICES_SIGNATURE 0x56524553544e5552ULL /* "RUNTSERV" */

static const efi_runtime_services_t* g_efi_rt;
static spinlock_t g_efi_rtc_lock;

void efi_rtc_init(const boot_info_t* info) {
    spinlock_init(&g_efi_rtc_lock);
    g_efi_rt = 0;
    if (!info || (info->flags & BOOT_INFO_FLAG_EFI_RUNTIME_PRESENT) == 0u ||
        info->efi_runtime_services == 0) {
        return;
    }
    const efi_runtime_services_t* rt =
        (const efi_runtime_services_t*)(uintptr_t)info->efi_runtime_services;
    if (rt->signature != EFI_RUNTIME_SERVICES_SIGNATURE || !rt->get_time || !rt->set_time) {
        klog_write("[efi-rtc] runtime services table malformed\n");
        return;
    }
    g_efi_rt = rt;
    klog_printf("[efi-rtc] runtime services at 0x%llx\n",
                (unsigned long long)info->efi_runtime_services);
}

int32_t efi_rtc_get(int32_t* out) {
    efi_time_t t;
    if (!out) {
        return WASMOS_ERR_RTC_INVALID;
    }
    if (!g_efi_rt) {
        return WASMOS_ERR_RTC_UNAVAILABLE;
    }
    __builtin_memset(&t, 0, sizeof(t));
    spinlock_lock(&g_efi_rtc_lock);
    efi_status_t status = g_efi_rt->get_time(&t, 0);
    spinlock_unlock(&g_efi_rtc_lock);
    if (status != 0) {
        return WASMOS_ERR_RTC_FIRMWARE;
    }
    /* TODO(efi-rtc-timezone): the fields are the firmware's wall time. With
     * time_zone unspecified (edk2 on QEMU virt) that is the PL031 count, UTC;
     * a firmware configured with a zone reports local time, which is passed
     * on unconverted. */
    out[0] = t.second;
    out[1] = t.minute;
    out[2] = t.hour;
    out[3] = t.day;
    out[4] = t.month;
    out[5] = t.year;
    return WASMOS_ERR_NONE;
}

int32_t efi_rtc_set(const int32_t* in) {
    efi_time_t t;
    if (!in) {
        return WASMOS_ERR_RTC_INVALID;
    }
    if (in[0] < 0 || in[0] > 59 || in[1] < 0 || in[1] > 59 || in[2] < 0 || in[2] > 23 ||
        in[3] < 1 || in[3] > 31 || in[4] < 1 || in[4] > 12 || in[5] < 1970 || in[5] > 2099) {
        return WASMOS_ERR_RTC_INVALID;
    }
    if (!g_efi_rt) {
        return WASMOS_ERR_RTC_UNAVAILABLE;
    }
    __builtin_memset(&t, 0, sizeof(t));
    t.second = (uint8_t)in[0];
    t.minute = (uint8_t)in[1];
    t.hour = (uint8_t)in[2];
    t.day = (uint8_t)in[3];
    t.month = (uint8_t)in[4];
    t.year = (uint16_t)in[5];
    t.time_zone = EFI_UNSPECIFIED_TIMEZONE;
    spinlock_lock(&g_efi_rtc_lock);
    efi_status_t status = g_efi_rt->set_time(&t);
    spinlock_unlock(&g_efi_rtc_lock);
    return status == 0 ? WASMOS_ERR_NONE : WASMOS_ERR_RTC_FIRMWARE;
}
