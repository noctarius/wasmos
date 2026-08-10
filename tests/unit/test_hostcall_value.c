/* test_hostcall_value.c — the rule for host calls that return a value.
 *
 * The ABI gives every host call ONE signed i32 on which it must carry both its
 * result and its errors. The error codes are negative, so any success value
 * with bit 31 set is read by the guest as an error. That is not a theoretical
 * hazard: block_buffer_phys returned a physical address that way and an address
 * of 0xFFFFFFFF would have been indistinguishable from a bare -1.
 *
 * Two shapes, because the right answer differs:
 *
 *  - A value that SHOULD always be small (an address from a bounded pool, an
 *    id, a count) is checked. Exceeding the bound means something upstream is
 *    wrong, and the call should say so rather than hand back a number the guest
 *    will misread.
 *
 *  - A monotonic counter has no small bound to enforce; it is consumed as
 *    deltas (lwIP's sys_now, frame timing), so it must stay positive and wrap
 *    cleanly. Erroring at the wrap point would break every caller at ~99 days
 *    of uptime instead of fixing anything.
 */

#include <stdio.h>
#include <stdlib.h>

#include "test_shuffle.h"

#include "hostcall_value.h"

static int g_failures;
static int g_checks;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("  [FAIL] %s (%s:%d)\n", (msg), __FILE__, __LINE__);                            \
        }                                                                                          \
    } while (0)

static void test_values_below_2gib_are_returnable(void) {
    CHECK(hostcall_value_check(0) == WASMOS_OK, "zero");
    CHECK(hostcall_value_check(1) == WASMOS_OK, "one");
    CHECK(hostcall_value_check(0x7FFFFFFEULL) == WASMOS_OK, "one below the boundary");
    CHECK(hostcall_value_check(0x7FFFFFFFULL) == WASMOS_OK, "the largest positive i32");
}

/* Bit 31 set is the whole point: these are the values that come back negative. */
static void test_values_at_or_above_2gib_are_refused(void) {
    CHECK(hostcall_value_check(0x80000000ULL) == WASMOS_ERR_KERNEL_TOO_LARGE,
          "exactly 2 GiB, reads back as INT32_MIN");
    CHECK(hostcall_value_check(0xFFFFFFFFULL) == WASMOS_ERR_KERNEL_TOO_LARGE,
          "would be indistinguishable from a bare -1");
    CHECK(hostcall_value_check(0x100000000ULL) == WASMOS_ERR_KERNEL_TOO_LARGE, "above 4 GiB");
    CHECK(hostcall_value_check(0xFFFFFFFFFFFFFFFFULL) == WASMOS_ERR_KERNEL_TOO_LARGE, "all ones");
}

/* A monotonic counter stays positive across the point where a plain cast would
 * have gone negative, and keeps counting. */
static void test_counter_stays_positive_across_the_wrap(void) {
    CHECK(hostcall_value_counter(0) == 0, "zero");
    CHECK(hostcall_value_counter(1000) == 1000, "an ordinary uptime");
    CHECK(hostcall_value_counter(0x7FFFFFFFULL) == 0x7FFFFFFF, "the last value before the wrap");
    CHECK(hostcall_value_counter(0x80000000ULL) >= 0, "the tick a plain cast turns negative");
    CHECK(hostcall_value_counter(0xFFFFFFFFULL) >= 0, "and the one that reads as -1");
    CHECK(hostcall_value_counter(0xFFFFFFFFFFFFFFFFULL) >= 0, "and a 64-bit counter at its top");
}

/* Deltas are what callers actually compute, so they must survive the wrap:
 * elapsed time across the boundary must come back as the real elapsed time,
 * not as a huge or negative jump. */
static void test_deltas_survive_the_wrap(void) {
    int32_t before = hostcall_value_counter(0x7FFFFFFFULL - 10);
    int32_t after = hostcall_value_counter(0x7FFFFFFFULL + 10);
    int32_t delta = (int32_t)(((uint32_t)after - (uint32_t)before) & 0x7FFFFFFFu);
    CHECK(delta == 20, "20 ticks across the boundary is still 20 ticks");

    int32_t a = hostcall_value_counter(0x100000000ULL - 5);
    int32_t b = hostcall_value_counter(0x100000000ULL + 5);
    int32_t d2 = (int32_t)(((uint32_t)b - (uint32_t)a) & 0x7FFFFFFFu);
    CHECK(d2 == 10, "10 ticks across the 32-bit boundary is still 10 ticks");
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_values_below_2gib_are_returnable),
    WASMOS_TEST_CASE(test_values_at_or_above_2gib_are_refused),
    WASMOS_TEST_CASE(test_counter_stays_positive_across_the_wrap),
    WASMOS_TEST_CASE(test_deltas_survive_the_wrap),
};

int main(void) {
    printf("test_hostcall_value\n");

    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    printf("  %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_hostcall_value: FAIL\n");
        return 1;
    }
    printf("test_hostcall_value: OK\n");
    return 0;
}
