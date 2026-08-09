/* test_libc_stdlib.c - unit tests for wasmos libc strtol */
#include <stdint.h>
#include <stddef.h>

#include "test_shuffle.h"

#include "stdlib.h"

static int test_basic(void) {
    if (strtol("0", NULL, 10) != 0)
        return __LINE__;
    if (strtol("1", NULL, 10) != 1)
        return __LINE__;
    if (strtol("-1", NULL, 10) != -1)
        return __LINE__;
    if (strtol("42", NULL, 10) != 42)
        return __LINE__;
    if (strtol("-42", NULL, 10) != -42)
        return __LINE__;
    return 0;
}

static int test_bases(void) {
    if (strtol("ff", NULL, 16) != 255)
        return __LINE__;
    if (strtol("0xff", NULL, 16) != 255)
        return __LINE__;
    if (strtol("0xFF", NULL, 0) != 255)
        return __LINE__;
    if (strtol("010", NULL, 0) != 8)
        return __LINE__;
    if (strtol("10", NULL, 2) != 2)
        return __LINE__;
    return 0;
}

static int test_endptr(void) {
    char* end;
    long v = strtol("123abc", &end, 10);
    if (v != 123)
        return __LINE__;
    if (*end != 'a')
        return __LINE__;
    return 0;
}

static int test_whitespace_sign(void) {
    if (strtol("  +7", NULL, 10) != 7)
        return __LINE__;
    if (strtol("  -7", NULL, 10) != -7)
        return __LINE__;
    return 0;
}

/* M-8: overflow must saturate, not wrap */
static int test_overflow_positive(void) {
    long v = strtol("99999999999999999999", NULL, 10);
    /* must return LONG_MAX (0x7FFF...F), not a wrapped garbage value */
    if (v != (long)((unsigned long)(-1L) >> 1))
        return __LINE__;
    return 0;
}

static int test_overflow_negative(void) {
    long v = strtol("-99999999999999999999", NULL, 10);
    /* must return LONG_MIN (0x8000...0) */
    if (v != (long)(~((unsigned long)(-1L) >> 1)))
        return __LINE__;
    return 0;
}

static int test_overflow_just_above_max(void) {
    /* LONG_MAX on 64-bit is 9223372036854775807 */
    long v = strtol("9223372036854775808", NULL, 10);
    if (v != (long)((unsigned long)(-1L) >> 1))
        return __LINE__;
    return 0;
}

static int test_max_exact(void) {
    /* exactly LONG_MAX should parse correctly */
    long v = strtol("9223372036854775807", NULL, 10);
    if (v != (long)((unsigned long)(-1L) >> 1))
        return __LINE__;
    return 0;
}

/* M-7: malloc(SIZE_MAX) must return NULL, not a wrapped tiny block */
static int test_malloc_overflow(void) {
    void* p = malloc((size_t)(-1));
    if (p != NULL)
        return __LINE__;
    /* Also check one below the overflow boundary */
    p = malloc((size_t)(-1) - sizeof(void*));
    if (p != NULL)
        return __LINE__;
    return 0;
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_basic),
        WASMOS_TEST_CASE(test_bases),
        WASMOS_TEST_CASE(test_endptr),
        WASMOS_TEST_CASE(test_whitespace_sign),
        WASMOS_TEST_CASE(test_overflow_positive),
        WASMOS_TEST_CASE(test_overflow_negative),
        WASMOS_TEST_CASE(test_overflow_just_above_max),
        WASMOS_TEST_CASE(test_max_exact),
        WASMOS_TEST_CASE(test_malloc_overflow),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    return 0;
}
