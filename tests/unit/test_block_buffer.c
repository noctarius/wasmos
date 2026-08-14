/* test_block_buffer.c — bounds arithmetic for the block bounce buffer.
 *
 * block_buffer_copy/write take (offset, len) from the guest and memcpy that
 * slice of an 8 KiB physical buffer. If the bound is computed in 32-bit
 * arithmetic the sum wraps, an out-of-range access passes the test, and the
 * memcpy lands outside the buffer -- a kernel read for copy, a kernel WRITE of
 * guest-supplied bytes for write. The wrapping cases below are the whole point
 * of the file; the in-range ones are there so a check that simply refuses
 * everything cannot pass.
 */

#include <stdio.h>
#include <stdlib.h>

#include "test_shuffle.h"

#include "block_buffer.h"

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

/* The bound passed as buf_bytes throughout: the deployed bounce buffer is
 * WASM_BLOCK_BUFFER_PAGES (2) pages, so 8 KiB. block_buffer_check_range takes
 * the bound as a parameter, so this is a fixture value and not a limit compiled
 * into the code under test; it is spelled 64-bit so the wrapping cases below
 * are not narrowed on their way in. */
#define BUF 8192ULL

/* A slice wholly inside the buffer is allowed, including the two edges. */
static void test_in_range_is_allowed(void) {
    CHECK(block_buffer_check_range(0, 1, BUF) == WASMOS_OK, "first byte");
    CHECK(block_buffer_check_range(0, BUF, BUF) == WASMOS_OK, "the whole buffer");
    CHECK(block_buffer_check_range(BUF - 1, 1, BUF) == WASMOS_OK, "last byte");
    CHECK(block_buffer_check_range(4096, 4096, BUF) == WASMOS_OK, "second page");
}

/* A zero length touches nothing, so it is a no-op success wherever it sits --
 * consistent with the rest of the transfer surface. */
static void test_zero_length_is_a_noop(void) {
    CHECK(block_buffer_check_range(0, 0, BUF) == WASMOS_OK, "zero at the base");
    CHECK(block_buffer_check_range(BUF, 0, BUF) == WASMOS_OK, "zero at the end");
}

/* Ordinary overruns, no arithmetic subtlety. */
static void test_past_the_end_is_refused(void) {
    CHECK(block_buffer_check_range(0, BUF + 1, BUF) == WASMOS_ERR_BLOCK_RANGE, "one byte too long");
    CHECK(block_buffer_check_range(BUF, 1, BUF) == WASMOS_ERR_BLOCK_RANGE, "starts at the end");
    CHECK(block_buffer_check_range(BUF - 1, 2, BUF) == WASMOS_ERR_BLOCK_RANGE, "straddles the end");
    CHECK(block_buffer_check_range(BUF + 4096, 1, BUF) == WASMOS_ERR_BLOCK_RANGE, "well past");
}

/* The reason this file exists. Each of these sums to something small in 32-bit
 * arithmetic and so passes a 32-bit bound, while naming a byte far outside the
 * buffer. offset is what the memcpy adds to the buffer base, so a pass here is
 * an out-of-bounds kernel access at an offset the guest chose.
 */
static void test_32bit_wrap_is_refused(void) {
    CHECK(block_buffer_check_range(0xFFFFFFFFULL, 1, BUF) == WASMOS_ERR_BLOCK_RANGE,
          "0xFFFFFFFF + 1 wraps to 0");
    CHECK(block_buffer_check_range(0xFFFFFFF0ULL, 0x20, BUF) == WASMOS_ERR_BLOCK_RANGE,
          "wraps to 0x10, inside an 8 KiB bound");
    CHECK(block_buffer_check_range(0x80000000ULL, 0x80000000ULL, BUF) == WASMOS_ERR_BLOCK_RANGE,
          "two halves of the range sum to 0");
    CHECK(block_buffer_check_range(1, 0xFFFFFFFFULL, BUF) == WASMOS_ERR_BLOCK_RANGE,
          "a length that wraps rather than an offset");
}

/* A negative int32 length reaches the check zero-extended, so it must be
 * refused by the same bound rather than by a separate sign test. */
static void test_negative_int32_length_is_refused(void) {
    CHECK(block_buffer_check_range(0, (uint64_t)(uint32_t)(int32_t)-1, BUF) ==
              WASMOS_ERR_BLOCK_RANGE,
          "len = -1");
    CHECK(block_buffer_check_range((uint64_t)(uint32_t)(int32_t)-4096, 1, BUF) ==
              WASMOS_ERR_BLOCK_RANGE,
          "offset = -4096");
}

/* Neither operand may be truncated on its way in: a value whose low 32 bits
 * look harmless must still be refused. */
static void test_high_bits_are_not_truncated(void) {
    CHECK(block_buffer_check_range(0x1000000000ULL, 1, BUF) == WASMOS_ERR_BLOCK_RANGE,
          "offset above 4 GiB with zero low bits");
    CHECK(block_buffer_check_range(0, 0x100000000ULL, BUF) == WASMOS_ERR_BLOCK_RANGE,
          "len of exactly 4 GiB");
}

/* block_buffer_phys returns the buffer's physical address as its success value,
 * on the same i32 that carries the error codes. Anything with bit 31 set is
 * read by the guest as a negative number, i.e. as an error, so the address must
 * fit a POSITIVE i32 -- a stricter bound than "below 4 GiB", which is what the
 * allocation asked for.
 */
static void test_phys_must_fit_a_positive_i32(void) {
    CHECK(block_buffer_check_phys(0x1000) == WASMOS_OK, "a low address");
    CHECK(block_buffer_check_phys(0x7FFFF000ULL) == WASMOS_OK, "just under 2 GiB");
    CHECK(block_buffer_check_phys(0x80000000ULL) == WASMOS_ERR_BLOCK_ABOVE_4G,
          "2 GiB reads back as a negative i32");
    CHECK(block_buffer_check_phys(0x90000000ULL) == WASMOS_ERR_BLOCK_ABOVE_4G,
          "an address a below-4-GiB allocation can return");
    CHECK(block_buffer_check_phys(0xFFFFFFFFULL) == WASMOS_ERR_BLOCK_ABOVE_4G,
          "would be indistinguishable from a bare -1");
    CHECK(block_buffer_check_phys(0x100000000ULL) == WASMOS_ERR_BLOCK_ABOVE_4G, "above 4 GiB");
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_phys_must_fit_a_positive_i32),
    WASMOS_TEST_CASE(test_in_range_is_allowed),
    WASMOS_TEST_CASE(test_zero_length_is_a_noop),
    WASMOS_TEST_CASE(test_past_the_end_is_refused),
    WASMOS_TEST_CASE(test_32bit_wrap_is_refused),
    WASMOS_TEST_CASE(test_negative_int32_length_is_refused),
    WASMOS_TEST_CASE(test_high_bits_are_not_truncated),
};

int main(void) {
    printf("test_block_buffer\n");

    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    printf("  %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        wasmos_test_report_seed(seed);
    }
    if (g_failures) {
        printf("test_block_buffer: FAIL\n");
        return 1;
    }
    printf("test_block_buffer: OK\n");
    return 0;
}
