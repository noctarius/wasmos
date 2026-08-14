/* test_libc_stdio.c - unit tests for the wasmos libc vsnprintf format engine.
 * Compiled against the real stdio.c + string.c sources on the host.
 *
 * Only the buffer-formatting half of stdio.c is covered: snprintf writes into
 * the caller's buffer and never reaches the console, so the specifier set, the
 * length modifiers and width padding are what the cases pin. The output side
 * (printf/puts/putsn, and the 127-byte chunking they do) needs the console
 * host call and is not exercised.
 *
 * Sizes differ from the target: `long` and pointers are 64-bit on the host and
 * 32-bit under wasm32, so the %ld/%lu cases prove the modifier is honoured, not
 * that a given value formats identically on both. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "test_shuffle.h"

/* Link-time stand-in for the console_write host call (abi/hostcalls.yaml id 10),
 * which the real build resolves as a WASM import: it writes `len` bytes of guest
 * memory at `ptr` to the kernel log and returns 0, or a negative packed status
 * for a negative length or an unreadable range. This one discards the bytes and
 * always reports success, so no failure a console write can report is
 * reachable. It exists because stdio.c is compiled whole, not because these
 * cases call it. */
/* Stub wasmos_console_write — not called by vsnprintf/snprintf. */
int32_t wasmos_console_write(int32_t ptr, int32_t len) {
    (void)ptr;
    (void)len;
    return 0;
}

#include "stdio.h"

/* ---- helpers ---- */
static int streq(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}

/* ---- basic specifiers ---- */
static int test_plain_string(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "hello %s", "world");
    if (!streq(buf, "hello world"))
        return __LINE__;
    return 0;
}

static int test_plain_int(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", 42);
    if (!streq(buf, "42"))
        return __LINE__;
    snprintf(buf, sizeof(buf), "%d", -1);
    if (!streq(buf, "-1"))
        return __LINE__;
    return 0;
}

static int test_plain_unsigned_hex(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", 255u);
    if (!streq(buf, "255"))
        return __LINE__;
    snprintf(buf, sizeof(buf), "%x", 0xdeadu);
    if (!streq(buf, "dead"))
        return __LINE__;
    snprintf(buf, sizeof(buf), "%X", 0xBEEFu);
    if (!streq(buf, "BEEF"))
        return __LINE__;
    return 0;
}

static int test_long(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", 100000L);
    if (!streq(buf, "100000"))
        return __LINE__;
    snprintf(buf, sizeof(buf), "%lu", 4294967295UL);
    if (!streq(buf, "4294967295"))
        return __LINE__;
    return 0;
}

/* ---- long long and size_t specifiers ---- */
static int test_lld(void) {
    char buf[64];
    /* value that fits in long long but not int */
    long long val = 9000000000LL;
    snprintf(buf, sizeof(buf), "%lld", val);
    if (!streq(buf, "9000000000"))
        return __LINE__;
    snprintf(buf, sizeof(buf), "%lld", -1LL);
    if (!streq(buf, "-1"))
        return __LINE__;
    return 0;
}

static int test_llu(void) {
    char buf[64];
    unsigned long long val = 18000000000ULL;
    snprintf(buf, sizeof(buf), "%llu", val);
    if (!streq(buf, "18000000000"))
        return __LINE__;
    return 0;
}

static int test_llx(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%llx", 0xdeadbeefcafeULL);
    if (!streq(buf, "deadbeefcafe"))
        return __LINE__;
    return 0;
}

static int test_zu(void) {
    char buf[32];
    size_t val = 12345;
    snprintf(buf, sizeof(buf), "%zu", val);
    if (!streq(buf, "12345"))
        return __LINE__;
    return 0;
}

static int test_zd(void) {
    char buf[32];
    /* %zd — signed size_t (ssize_t) */
    size_t val = 99;
    snprintf(buf, sizeof(buf), "%zd", val);
    if (!streq(buf, "99"))
        return __LINE__;
    return 0;
}

/* ---- width / padding ---- */
static int test_width_padding(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%05d", 42);
    if (!streq(buf, "00042"))
        return __LINE__;
    snprintf(buf, sizeof(buf), "%5d", 42);
    if (!streq(buf, "   42"))
        return __LINE__;
    return 0;
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_plain_string),
        WASMOS_TEST_CASE(test_plain_int),
        WASMOS_TEST_CASE(test_plain_unsigned_hex),
        WASMOS_TEST_CASE(test_long),
        WASMOS_TEST_CASE(test_width_padding),
        WASMOS_TEST_CASE(test_lld),
        WASMOS_TEST_CASE(test_llu),
        WASMOS_TEST_CASE(test_llx),
        WASMOS_TEST_CASE(test_zu),
        WASMOS_TEST_CASE(test_zd),
    };
    if (wasmos_test_run_all(cases, (int)(sizeof(cases) / sizeof(cases[0]))) != 0) {
        return 1;
    }
    return 0;
}
