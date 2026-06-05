/* test_libc_stdio.c - unit tests for the wasmos libc vsnprintf format engine.
 * Compiled against the real stdio.c + string.c sources on the host. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Stub wasmos_console_write — not called by vsnprintf/snprintf. */
int32_t wasmos_console_write(int32_t ptr, int32_t len) { (void)ptr; (void)len; return 0; }

#include "stdio.h"

/* ---- helpers ---- */
static int
streq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

/* ---- basic specifiers ---- */
static int
test_plain_string(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "hello %s", "world");
    if (!streq(buf, "hello world")) return __LINE__;
    return 0;
}

static int
test_plain_int(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", 42);
    if (!streq(buf, "42")) return __LINE__;
    snprintf(buf, sizeof(buf), "%d", -1);
    if (!streq(buf, "-1")) return __LINE__;
    return 0;
}

static int
test_plain_unsigned_hex(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", 255u);
    if (!streq(buf, "255")) return __LINE__;
    snprintf(buf, sizeof(buf), "%x", 0xdeadu);
    if (!streq(buf, "dead")) return __LINE__;
    snprintf(buf, sizeof(buf), "%X", 0xBEEFu);
    if (!streq(buf, "BEEF")) return __LINE__;
    return 0;
}

static int
test_long(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", 100000L);
    if (!streq(buf, "100000")) return __LINE__;
    snprintf(buf, sizeof(buf), "%lu", 4294967295UL);
    if (!streq(buf, "4294967295")) return __LINE__;
    return 0;
}

/* ---- M-10: %lld and %zu ---- */
static int
test_lld(void)
{
    char buf[64];
    /* value that fits in long long but not int */
    long long val = 9000000000LL;
    snprintf(buf, sizeof(buf), "%lld", val);
    if (!streq(buf, "9000000000")) return __LINE__;
    snprintf(buf, sizeof(buf), "%lld", -1LL);
    if (!streq(buf, "-1")) return __LINE__;
    return 0;
}

static int
test_llu(void)
{
    char buf[64];
    unsigned long long val = 18000000000ULL;
    snprintf(buf, sizeof(buf), "%llu", val);
    if (!streq(buf, "18000000000")) return __LINE__;
    return 0;
}

static int
test_llx(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%llx", 0xdeadbeefcafeULL);
    if (!streq(buf, "deadbeefcafe")) return __LINE__;
    return 0;
}

static int
test_zu(void)
{
    char buf[32];
    size_t val = 12345;
    snprintf(buf, sizeof(buf), "%zu", val);
    if (!streq(buf, "12345")) return __LINE__;
    return 0;
}

static int
test_zd(void)
{
    char buf[32];
    /* %zd — signed size_t (ssize_t) */
    size_t val = 99;
    snprintf(buf, sizeof(buf), "%zd", val);
    if (!streq(buf, "99")) return __LINE__;
    return 0;
}

/* ---- width / padding ---- */
static int
test_width_padding(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%05d", 42);
    if (!streq(buf, "00042")) return __LINE__;
    snprintf(buf, sizeof(buf), "%5d", 42);
    if (!streq(buf, "   42")) return __LINE__;
    return 0;
}

int
main(void)
{
    int rc;

    rc = test_plain_string();    if (rc) return rc;
    rc = test_plain_int();       if (rc) return rc;
    rc = test_plain_unsigned_hex(); if (rc) return rc;
    rc = test_long();            if (rc) return rc;
    rc = test_width_padding();   if (rc) return rc;

    /* M-10 tests — these fail before the fix */
    rc = test_lld();  if (rc) return rc;
    rc = test_llu();  if (rc) return rc;
    rc = test_llx();  if (rc) return rc;
    rc = test_zu();   if (rc) return rc;
    rc = test_zd();   if (rc) return rc;

    return 0;
}
