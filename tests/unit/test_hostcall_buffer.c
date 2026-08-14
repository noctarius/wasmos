/* test_hostcall_buffer.c — filling a caller-supplied name buffer.
 *
 * The kernel holds a bounded, possibly unterminated string; the guest supplies
 * a buffer. Getting this wrong is not a cosmetic matter: `nlen = out_len - 1`
 * computed without first checking out_len underflows a zero-sized buffer to
 * 0xFFFFFFFF and turns the copy into a 4 GiB memcpy.
 *
 * The other half of the contract is which length gets REPORTED. Returning the
 * clamped length loses the only signal a caller has that its buffer was too
 * small -- fs_init.c skips an entry whose reported length does not fit, a test
 * that cannot fire if the reported length was made to fit by construction.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_shuffle.h"

#include "hostcall_buffer.h"

static int g_failures;
static int g_checks;

/* Count-and-continue assertion: tallies every check in g_checks, and on failure
 * bumps g_failures and prints the message with this file and line. It does not
 * return or abort, so a failing check leaves the rest of its case running and
 * the suite reports the total number of failing checks rather than stopping at
 * the first. Cases return void; main's exit status comes from g_failures. */
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("  [FAIL] %s (%s:%d)\n", (msg), __FILE__, __LINE__);                            \
        }                                                                                          \
    } while (0)

/* The name_max argument for cases that are about the OUT buffer rather than the
 * source field: wider than any name used here, so the field width is never the
 * binding bound and out_size is what each case exercises. Real call sites pass
 * their own width (the initfs entry path field is 96 bytes; a boot module name
 * passes strlen + 1); the one case that is about the field width passes its own
 * sizeof instead. */
#define NAME_MAX_FIELD 112u

static void test_name_that_fits_is_copied_whole(void) {
    uint32_t true_len = 0;
    uint32_t copy_len = 0;

    CHECK(hostcall_name_clamp("abc", NAME_MAX_FIELD, 64, &true_len, &copy_len) == WASMOS_OK, "ok");
    CHECK(true_len == 3, "true length");
    CHECK(copy_len == 3, "copied whole");
}

/* The buffer holds the name and its terminator exactly. */
static void test_exact_fit_leaves_room_for_the_nul(void) {
    uint32_t true_len = 0;
    uint32_t copy_len = 0;

    CHECK(hostcall_name_clamp("abc", NAME_MAX_FIELD, 4, &true_len, &copy_len) == WASMOS_OK, "ok");
    CHECK(true_len == 3, "true length");
    CHECK(copy_len == 3, "all three bytes fit alongside the NUL");
}

/* One byte short: the name must be cut, and the caller must still be told how
 * long it really was. */
static void test_truncation_reports_the_true_length(void) {
    uint32_t true_len = 0;
    uint32_t copy_len = 0;

    CHECK(hostcall_name_clamp("abcdef", NAME_MAX_FIELD, 4, &true_len, &copy_len) == WASMOS_OK,
          "ok");
    CHECK(copy_len == 3, "only three bytes fit");
    CHECK(true_len == 6, "but the caller is told the name is six long");
    CHECK(true_len != copy_len, "which is how truncation is detectable at all");
}

/* A single-byte buffer can hold nothing but the terminator. */
static void test_one_byte_buffer_copies_nothing(void) {
    uint32_t true_len = 0;
    uint32_t copy_len = 0;

    CHECK(hostcall_name_clamp("abc", NAME_MAX_FIELD, 1, &true_len, &copy_len) == WASMOS_OK, "ok");
    CHECK(copy_len == 0, "no room for any character");
    CHECK(true_len == 3, "true length still reported");
}

/* The reason this file exists: out_size == 0 has no room even for the
 * terminator, and must be refused rather than wrapped into a huge copy. */
static void test_zero_sized_buffer_is_refused(void) {
    uint32_t true_len = 12345;
    uint32_t copy_len = 12345;

    CHECK(hostcall_name_clamp("abc", NAME_MAX_FIELD, 0, &true_len, &copy_len) == WASMOS_INVAL,
          "zero-sized buffer is refused");
    CHECK(copy_len < 4096, "and copy_len is not an underflowed length");
    CHECK(copy_len == 0, "specifically, it is zero");
}

/* The source is a fixed-width field that need not be terminated, so the scan
 * must stop at the field width. */
static void test_unterminated_source_stops_at_the_field_width(void) {
    char field[8];
    uint32_t true_len = 0;
    uint32_t copy_len = 0;

    memset(field, 'x', sizeof(field)); /* deliberately no NUL */
    CHECK(hostcall_name_clamp(field, (uint32_t)sizeof(field), 64, &true_len, &copy_len) ==
              WASMOS_OK,
          "ok");
    CHECK(true_len == 8, "stops at the field width, does not run off the end");
    CHECK(copy_len == 8, "and all of it fits the roomy buffer");
}

static void test_empty_name(void) {
    uint32_t true_len = 0;
    uint32_t copy_len = 0;

    CHECK(hostcall_name_clamp("", NAME_MAX_FIELD, 64, &true_len, &copy_len) == WASMOS_OK, "ok");
    CHECK(true_len == 0, "empty");
    CHECK(copy_len == 0, "nothing to copy");
}

static void test_null_arguments_are_refused(void) {
    uint32_t true_len = 0;
    uint32_t copy_len = 0;

    CHECK(hostcall_name_clamp(0, NAME_MAX_FIELD, 64, &true_len, &copy_len) == WASMOS_INVAL,
          "no name");
    CHECK(hostcall_name_clamp("a", NAME_MAX_FIELD, 64, 0, &copy_len) == WASMOS_INVAL,
          "nowhere to report the true length");
    CHECK(hostcall_name_clamp("a", NAME_MAX_FIELD, 64, &true_len, 0) == WASMOS_INVAL,
          "nowhere to report the copy length");
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_name_that_fits_is_copied_whole),
    WASMOS_TEST_CASE(test_exact_fit_leaves_room_for_the_nul),
    WASMOS_TEST_CASE(test_truncation_reports_the_true_length),
    WASMOS_TEST_CASE(test_one_byte_buffer_copies_nothing),
    WASMOS_TEST_CASE(test_zero_sized_buffer_is_refused),
    WASMOS_TEST_CASE(test_unterminated_source_stops_at_the_field_width),
    WASMOS_TEST_CASE(test_empty_name),
    WASMOS_TEST_CASE(test_null_arguments_are_refused),
};

int main(void) {
    printf("test_hostcall_buffer\n");

    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    printf("  %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_hostcall_buffer: FAIL\n");
        return 1;
    }
    printf("test_hostcall_buffer: OK\n");
    return 0;
}
