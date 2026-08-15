/* test_libc_string.c - unit tests for the wasmos libc memcmp guards.
 * Compiled against the real string.c on the host.
 *
 * memcmp is the entry point in string.c that did NOT guard its arguments: every
 * other one rejects a NULL, while memcmp tested only `lhs == rhs || count == 0`
 * and then dereferenced. NULL/NULL and a zero count were therefore already
 * safe by accident, and a single NULL with a non-zero count was a fault.
 *
 * The ordering cases are here because a guard that returns the wrong sign is
 * the easy way to fix the crash and break the contract: memcmp's sign is what
 * callers sort and bsearch on.
 */
#include <stddef.h>
#include <stdint.h>

#include "test_shuffle.h"

int memcmp(const void* lhs, const void* rhs, size_t count);

/* A NULL operand with a non-zero count must be answered, not dereferenced. The
 * non-NULL side is defined to compare greater, so the result is a usable
 * ordering rather than an arbitrary constant. */
static int test_memcmp_null_operand(void) {
    const char data[] = "abc";
    if (memcmp(NULL, data, sizeof(data)) >= 0) {
        return __LINE__;
    }
    if (memcmp(data, NULL, sizeof(data)) <= 0) {
        return __LINE__;
    }
    return 0;
}

/* The two arms that were already safe, pinned so a rewrite of the guard cannot
 * quietly change them: identical pointers and a zero count are both equal,
 * NULL included, and neither may read through the pointer. */
static int test_memcmp_equal_and_empty(void) {
    const char data[] = "abc";
    if (memcmp(NULL, NULL, 16) != 0) {
        return __LINE__;
    }
    if (memcmp(data, data, sizeof(data)) != 0) {
        return __LINE__;
    }
    if (memcmp(NULL, data, 0) != 0) {
        return __LINE__;
    }
    if (memcmp(data, NULL, 0) != 0) {
        return __LINE__;
    }
    return 0;
}

/* Ordinary comparison still works, and the sign follows the first differing
 * byte compared as unsigned char -- 0x80 is greater than 0x01, not less, which
 * a signed char implementation gets backwards. */
static int test_memcmp_ordering(void) {
    const unsigned char low[] = {0x01, 0x02, 0x03};
    const unsigned char high[] = {0x01, 0x80, 0x03};
    if (memcmp(low, high, sizeof(low)) >= 0) {
        return __LINE__;
    }
    if (memcmp(high, low, sizeof(low)) <= 0) {
        return __LINE__;
    }
    if (memcmp(low, high, 1) != 0) {
        return __LINE__;
    }
    return 0;
}

/* Distinct buffers holding the same bytes are equal: the equality answer must
 * come from the contents, not from the pointer shortcut. */
static int test_memcmp_equal_contents(void) {
    const char a[] = "hello";
    const char b[] = "hello";
    if (memcmp(a, b, sizeof(a)) != 0) {
        return __LINE__;
    }
    return 0;
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_memcmp_null_operand),
        WASMOS_TEST_CASE(test_memcmp_equal_and_empty),
        WASMOS_TEST_CASE(test_memcmp_ordering),
        WASMOS_TEST_CASE(test_memcmp_equal_contents),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    return 0;
}
