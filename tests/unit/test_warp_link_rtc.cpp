/* test_warp_link_rtc.cpp - the WARP admission check of the firmware-clock host
 * calls (src/kernel/warp/link_rtc.cpp).
 *
 * Regression: 2026-11-24-warp-rtc-efi-negative-len -- the length was compared
 * as the uint32_t WARP widens a wasm i32 to, so a negative length (-1 arrives
 * as 0xFFFFFFFF) passed the size check that the wasm3 host calls refuse with
 * WASMOS_INVAL. The same guest call answered differently under the two
 * runtimes, against the host-call contract for a length below 24 bytes.
 *
 * The capability table and the current context are stubbed: what is under test
 * is the order and the codes of the decision, for lengths on both sides of the
 * 24-byte minimum and across the sign boundary, with and without rtc.efi. */

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "test_shuffle.h"

extern "C" {
#include "capability.h"
#include "wasmos_status.h"
}

#include "warp/link_rtc.h"

static int g_have_context = 1;
static int g_has_cap = 1;

extern "C" int warp_current_context_id(uint32_t* out) {
    if (!g_have_context) {
        return -1;
    }
    *out = 42u;
    return 0;
}

extern "C" int capability_has(uint32_t context_id, capability_kind_t kind) {
    return g_has_cap && context_id == 42u && kind == CAP_RTC_EFI;
}

static void expect(uint32_t got, int32_t want, const char* what) {
    if ((int32_t)got != want) {
        std::fprintf(stderr, "%s: got %d, want %d\n", what, (int32_t)got, want);
        std::abort();
    }
}

static void reset(void) {
    g_have_context = 1;
    g_has_cap = 1;
}

static void test_the_minimum_length_is_admitted(void) {
    reset();
    expect(warp_rtc_efi_admit(24u), 0, "len 24");
    expect(warp_rtc_efi_admit(4096u), 0, "len 4096");
}

static void test_a_short_length_is_invalid(void) {
    reset();
    expect(warp_rtc_efi_admit(0u), WASMOS_INVAL, "len 0");
    expect(warp_rtc_efi_admit(23u), WASMOS_INVAL, "len 23");
}

static void test_a_negative_length_is_invalid(void) {
    reset();
    expect(warp_rtc_efi_admit((uint32_t)(int32_t)-1), WASMOS_INVAL, "len -1");
    expect(warp_rtc_efi_admit((uint32_t)INT32_MIN), WASMOS_INVAL, "len INT32_MIN");
}

static void test_the_length_is_checked_before_the_capability(void) {
    reset();
    g_has_cap = 0;
    expect(warp_rtc_efi_admit((uint32_t)(int32_t)-1), WASMOS_INVAL, "len -1 without cap");
}

static void test_without_rtc_efi_the_call_is_not_authorized(void) {
    reset();
    g_has_cap = 0;
    expect(warp_rtc_efi_admit(24u), WASMOS_ERR_KERNEL_NOT_AUTHORIZED, "no cap");
    reset();
    g_have_context = 0;
    expect(warp_rtc_efi_admit(24u), WASMOS_ERR_KERNEL_NOT_AUTHORIZED, "no context");
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_the_minimum_length_is_admitted),
        WASMOS_TEST_CASE(test_a_short_length_is_invalid),
        WASMOS_TEST_CASE(test_a_negative_length_is_invalid),
        WASMOS_TEST_CASE(test_the_length_is_checked_before_the_capability),
        WASMOS_TEST_CASE(test_without_rtc_efi_the_call_is_not_authorized),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    std::printf("test_warp_link_rtc: ok\n");
    return 0;
}
