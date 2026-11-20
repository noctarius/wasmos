/* test_kenv.c — the kernel environment store (src/kernel/kenv.c).
 *
 * One store serves both runtimes; what stays per runtime is only the
 * guest-memory plumbing around it. The lookup contract is what this file pins:
 * a key is either present exactly, or absent -- never nearly. A copy that
 * refused an over-long key on set but TRUNCATED it to KENV_KEY_MAX-1 on get
 * would answer a 40-character name with the value of the 32-character variable
 * sharing its prefix: a different variable, and one the guest could not have
 * created through env_set in the first place.
 *
 * src/kernel/kenv.c is the only source linked in; it needs nothing stubbed
 * because the store is a fixed file-static table of KENV_MAX_ENTRIES slots with
 * no allocator and no lock behind it. It also links against the host libc's
 * strcmp/strlen rather than the kernel's, which is a substitution of equals.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_shuffle.h"

#include "kenv.h"

static int g_failures;
static int g_checks;

/* Record one assertion: counts and CONTINUES on failure, printing the message
 * and its source line, so a failing case runs to its end. The cases return void;
 * main() reports g_failures as the process exit status. */
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

static void test_cmdline_publishes_each_key_value_token(void) {
    char out[KENV_VAL_MAX];
    uint32_t written = 0;

    reset();
    CHECK(kenv_load_cmdline("  log.serial=com2   boot.debug=1 ") == 2, "two tokens published");
    CHECK(kenv_get("log.serial", out, sizeof(out), &written) == WASMOS_OK &&
              strcmp(out, "com2") == 0,
          "first token");
    CHECK(kenv_get("boot.debug", out, sizeof(out), &written) == WASMOS_OK && strcmp(out, "1") == 0,
          "second token, after a run of spaces");
}

static void test_cmdline_bare_word_is_a_flag(void) {
    char out[KENV_VAL_MAX];
    uint32_t written = 0;

    reset();
    CHECK(kenv_load_cmdline("quiet") == 1, "a bare word is published");
    CHECK(kenv_get("quiet", out, sizeof(out), &written) == WASMOS_OK && strcmp(out, "1") == 0,
          "with the value 1");
}

static void test_cmdline_value_keeps_later_equals_signs(void) {
    char out[KENV_VAL_MAX];
    uint32_t written = 0;

    reset();
    CHECK(kenv_load_cmdline("opt=a=b") == 1, "one token");
    CHECK(kenv_get("opt", out, sizeof(out), &written) == WASMOS_OK && strcmp(out, "a=b") == 0,
          "the key ends at the first '='");
}

static void test_cmdline_skips_unusable_tokens(void) {
    char out[KENV_VAL_MAX];
    char line[KENV_KEY_MAX + 16];
    uint32_t written = 0;

    reset();
    memset(line, 'k', KENV_KEY_MAX + 4);
    memcpy(line + KENV_KEY_MAX + 4, "=v ok=1", 8);
    CHECK(kenv_load_cmdline(line) == 1, "the over-long key is skipped, the next token is not");
    CHECK(kenv_count() == 1, "nothing truncated was published");
    CHECK(kenv_get("ok", out, sizeof(out), &written) == WASMOS_OK, "the valid token");
    CHECK(kenv_load_cmdline("=nokey") == 0, "an empty key is skipped");
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

/* Writers of the table from several CPUs at once, each with keys of its own. */
#define KENV_RACE_WRITERS 8
#define KENV_RACE_KEYS (KENV_MAX_ENTRIES / KENV_RACE_WRITERS)
#define KENV_RACE_ROUNDS 400

/* Released together, so the writers enter kenv_set at the same time. */
static int g_kenv_race_go;

static void* kenv_race_writer(void* arg) {
    const int writer = (int)(intptr_t)arg;
    char key[16];
    while (!__atomic_load_n(&g_kenv_race_go, __ATOMIC_ACQUIRE)) {
    }
    for (int k = 0; k < KENV_RACE_KEYS; ++k) {
        (void)snprintf(key, sizeof(key), "w%d.k%d", writer, k);
        (void)kenv_set(key, "1");
    }
    return 0;
}

/* Regression: 2026-11-20-kenv-concurrent-set -- kenv_set picked a free slot
 * and filled it with no lock, so two guests setting new keys at once on
 * different CPUs could claim the same slot and both report success, one of
 * them losing its variable. The logger keeps its restart position there. */
static void test_concurrent_writers_lose_no_key(void) {
    int lost_rounds = 0;
    for (int round = 0; round < KENV_RACE_ROUNDS; ++round) {
        pthread_t threads[KENV_RACE_WRITERS];
        kenv_reset();
        __atomic_store_n(&g_kenv_race_go, 0, __ATOMIC_RELEASE);
        for (int w = 0; w < KENV_RACE_WRITERS; ++w) {
            (void)pthread_create(&threads[w], 0, kenv_race_writer, (void*)(intptr_t)w);
        }
        __atomic_store_n(&g_kenv_race_go, 1, __ATOMIC_RELEASE);
        for (int w = 0; w < KENV_RACE_WRITERS; ++w) {
            (void)pthread_join(threads[w], 0);
        }
        if (kenv_count() != KENV_MAX_ENTRIES) {
            lost_rounds++;
        }
    }
    kenv_reset();
    CHECK(lost_rounds == 0, "every writer's keys are in the table after every round");
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
    WASMOS_TEST_CASE(test_cmdline_publishes_each_key_value_token),
    WASMOS_TEST_CASE(test_cmdline_bare_word_is_a_flag),
    WASMOS_TEST_CASE(test_cmdline_value_keeps_later_equals_signs),
    WASMOS_TEST_CASE(test_cmdline_skips_unusable_tokens),
    WASMOS_TEST_CASE(test_concurrent_writers_lose_no_key),
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
