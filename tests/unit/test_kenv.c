/* test_kenv.c — the kernel environment store (src/kernel/kenv.c).
 *
 * One store serves both runtimes; what stays per runtime is only the
 * guest-memory plumbing around it. The lookup contract is what this file pins:
 * a key is either present exactly, or absent -- never nearly. A copy that
 * refused an over-long key on set but TRUNCATED it to KENV_KEY_MAX-1 on get
 * would answer a 40-character name with the value of the 32-character variable
 * sharing its prefix: a different variable, and one the guest could not have
 * created through env_set in the first place.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_shuffle.h"

#include "kenv.h"

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

/* Each case starts from an empty store; the cases run in a shuffled order and
 * must not depend on each other. */
static void reset(void) {
    kenv_reset();
}

static void test_set_then_get_round_trips(void) {
    char out[KENV_VAL_MAX];
    uint32_t written = 0;

    reset();
    CHECK(kenv_set("PATH", "/bin") == WASMOS_OK, "set");
    CHECK(kenv_get("PATH", out, sizeof(out), &written) == WASMOS_OK, "get");
    CHECK(written == 4, "length is the value's, not the buffer's");
    CHECK(strcmp(out, "/bin") == 0, "value round-trips");
}

static void test_missing_key_is_not_found(void) {
    char out[KENV_VAL_MAX];
    uint32_t written = 0;

    reset();
    CHECK(kenv_get("NOPE", out, sizeof(out), &written) == WASMOS_ERR_ENV_NOT_FOUND, "absent key");
    CHECK(kenv_set("PATH", "/bin") == WASMOS_OK, "set one");
    CHECK(kenv_get("PATHX", out, sizeof(out), &written) == WASMOS_ERR_ENV_NOT_FOUND,
          "a longer key is not the shorter one");
    CHECK(kenv_get("PAT", out, sizeof(out), &written) == WASMOS_ERR_ENV_NOT_FOUND,
          "a prefix is not the key");
}

static void test_set_replaces_rather_than_duplicates(void) {
    char out[KENV_VAL_MAX];
    uint32_t written = 0;

    reset();
    CHECK(kenv_set("K", "one") == WASMOS_OK, "first set");
    CHECK(kenv_set("K", "two") == WASMOS_OK, "second set");
    CHECK(kenv_get("K", out, sizeof(out), &written) == WASMOS_OK, "get");
    CHECK(strcmp(out, "two") == 0, "the later value wins");
    CHECK(kenv_count() == 1, "and it did not consume a second entry");
}

static void test_unset_removes_and_frees_the_entry(void) {
    char out[KENV_VAL_MAX];
    uint32_t written = 0;

    reset();
    CHECK(kenv_set("K", "v") == WASMOS_OK, "set");
    CHECK(kenv_unset("K") == WASMOS_OK, "unset");
    CHECK(kenv_get("K", out, sizeof(out), &written) == WASMOS_ERR_ENV_NOT_FOUND, "gone");
    CHECK(kenv_count() == 0, "entry is reusable");
    CHECK(kenv_unset("K") == WASMOS_OK, "unsetting an absent key is not an error");
}

/* The reason this file exists. A key at or over the limit must be REFUSED on
 * every path, never truncated -- otherwise it resolves to whatever variable
 * happens to share its first KENV_KEY_MAX-1 characters. */
static void test_over_long_key_is_refused_not_truncated(void) {
    char out[KENV_VAL_MAX];
    uint32_t written = 0;
    char prefix[KENV_KEY_MAX];
    char over_long[KENV_KEY_MAX + 16];

    reset();
    /* A legal key that occupies the whole allowance... */
    memset(prefix, 'A', sizeof(prefix) - 1u);
    prefix[sizeof(prefix) - 1u] = '\0';
    CHECK(kenv_set(prefix, "secret") == WASMOS_OK, "the longest legal key is settable");

    /* ...and a longer one that shares it as a prefix. */
    memset(over_long, 'A', sizeof(over_long) - 1u);
    over_long[sizeof(over_long) - 1u] = '\0';

    CHECK(kenv_set(over_long, "x") == WASMOS_ERR_ENV_TOO_LONG, "set refuses it");
    CHECK(kenv_unset(over_long) == WASMOS_ERR_ENV_TOO_LONG, "unset refuses it");
    CHECK(kenv_get(over_long, out, sizeof(out), &written) == WASMOS_ERR_ENV_TOO_LONG,
          "get refuses it rather than returning the prefix's value");
    CHECK(kenv_get(over_long, out, sizeof(out), &written) != WASMOS_OK,
          "and above all does not succeed");
}

static void test_over_long_value_is_refused(void) {
    char big[KENV_VAL_MAX + 16];

    reset();
    memset(big, 'v', sizeof(big) - 1u);
    big[sizeof(big) - 1u] = '\0';
    CHECK(kenv_set("K", big) == WASMOS_ERR_ENV_TOO_LONG, "value over the allowance");
    CHECK(kenv_count() == 0, "and no entry was consumed by the attempt");
}

/* A value longer than the caller's buffer is truncated to fit, and the caller
 * is told the value's real length so it can tell that happened. */
static void test_short_output_buffer_truncates(void) {
    char out[4];
    uint32_t written = 0;

    reset();
    CHECK(kenv_set("K", "abcdefgh") == WASMOS_OK, "set");
    CHECK(kenv_get("K", out, sizeof(out), &written) == WASMOS_OK, "get still succeeds");
    CHECK(strcmp(out, "abc") == 0, "truncated to the buffer, NUL-terminated");
    CHECK(written == 3, "written is what fit");
}

static void test_table_full_is_reported(void) {
    char key[16];

    reset();
    for (int i = 0; i < KENV_MAX_ENTRIES; i++) {
        snprintf(key, sizeof(key), "K%d", i);
        CHECK(kenv_set(key, "v") == WASMOS_OK, "filling the table");
    }
    CHECK(kenv_count() == KENV_MAX_ENTRIES, "table is full");
    CHECK(kenv_set("ONE_MORE", "v") == WASMOS_ERR_ENV_TABLE_FULL, "the next set is refused");
    /* A full table must still accept an update to an existing key: that needs
     * no free entry, and refusing it would be a surprising second failure. */
    CHECK(kenv_set("K0", "changed") == WASMOS_OK, "updating an existing key still works");
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_set_then_get_round_trips),
    WASMOS_TEST_CASE(test_missing_key_is_not_found),
    WASMOS_TEST_CASE(test_set_replaces_rather_than_duplicates),
    WASMOS_TEST_CASE(test_unset_removes_and_frees_the_entry),
    WASMOS_TEST_CASE(test_over_long_key_is_refused_not_truncated),
    WASMOS_TEST_CASE(test_over_long_value_is_refused),
    WASMOS_TEST_CASE(test_short_output_buffer_truncates),
    WASMOS_TEST_CASE(test_table_full_is_reported),
};

int main(void) {
    printf("test_kenv\n");

    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    printf("  %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_kenv: FAIL\n");
        return 1;
    }
    printf("test_kenv: OK\n");
    return 0;
}
