/* test_cli_ls_order.c — how `ls` orders a directory listing
 * (src/services/cli/cli_ls_order.c).
 *
 * The filesystem returns entries in on-disk slot order, which is not even
 * insertion order: a freed slot is reused, so a file created after a deletion
 * appears in the hole. FAT specifies no ordering and POSIX readdir() guarantees
 * none, so `ls` is what imposes one. These cases pin what "ordered" means here.
 *
 * Only cli_ls_order.c is linked: it touches no IPC, no console and no global
 * state, which is why it is a separate translation unit.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "cli_ls_order.h"

static int g_failures;

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL %s:%d: %s\n", __func__, __LINE__, (what));                              \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

/* Sort `names` and report the resulting order as one comma-joined string, which
 * makes an assertion read like the listing a user would see. */
static void order_of(const char* const* names, uint32_t count, char* out, uint32_t out_cap) {
    char pool[512];
    uint16_t offsets[32];
    uint32_t used = 0;
    uint32_t i;
    uint32_t pos = 0;

    assert(count <= 32u);
    for (i = 0; i < count; ++i) {
        uint32_t len = (uint32_t)strlen(names[i]);
        assert(used + len + 1u <= sizeof(pool));
        offsets[i] = (uint16_t)used;
        memcpy(&pool[used], names[i], len + 1u);
        used += len + 1u;
    }
    cli_ls_sort(pool, offsets, count);
    out[0] = '\0';
    for (i = 0; i < count; ++i) {
        const char* n = &pool[offsets[i]];
        uint32_t len = (uint32_t)strlen(n);
        if (pos + len + 2u >= out_cap) {
            break;
        }
        if (i > 0) {
            out[pos++] = ',';
        }
        memcpy(&out[pos], n, len);
        pos += len;
        out[pos] = '\0';
    }
}

/* The ordinary case: a listing comes out alphabetically regardless of the order
 * the filesystem happened to hand it over in. */
static void test_sorts_alphabetically(void) {
    static const char* const names[] = {"serial.wap", "ata.wap", "mouse.wap", "keyboard.wap"};
    char out[256];

    order_of(names, 4, out, sizeof(out));
    CHECK(strcmp(out, "ata.wap,keyboard.wap,mouse.wap,serial.wap") == 0, out);
}

/* Digit runs compare by value. This is the case plain strcmp gets wrong: it
 * puts f10 before f9, because '1' < '9'. */
static void test_orders_numbers_by_value_not_by_digit(void) {
    static const char* const names[] = {"f10", "f9", "f1", "f20", "f2"};
    char out[256];

    order_of(names, 5, out, sizeof(out));
    CHECK(strcmp(out, "f1,f2,f9,f10,f20") == 0, out);
}

/* Zero padding must not change the answer, so a padded and an unpadded listing
 * of the same files read the same way. */
static void test_zero_padding_does_not_change_the_order(void) {
    static const char* const names[] = {"f007", "f10", "f0002"};
    char out[256];

    order_of(names, 3, out, sizeof(out));
    CHECK(strcmp(out, "f0002,f007,f10") == 0, out);
}

/* FAT is case-insensitive, so ordering has to be too -- otherwise every
 * uppercase 8.3 name would sort ahead of every long name. */
static void test_ordering_is_case_insensitive(void) {
    static const char* const names[] = {"Zebra.txt", "apple.TXT", "Banana.txt"};
    char out[256];

    order_of(names, 3, out, sizeof(out));
    CHECK(strcmp(out, "apple.TXT,Banana.txt,Zebra.txt") == 0, out);
}

/* The trailing '/' marks a directory and is not part of the name: a directory
 * sorts where its name says, not where '/' would put it. */
static void test_directory_marker_is_not_part_of_the_name(void) {
    static const char* const names[] = {"beta", "alpha/", "alphabet"};
    char out[256];

    order_of(names, 3, out, sizeof(out));
    CHECK(strcmp(out, "alpha/,alphabet,beta") == 0, out);
    CHECK(cli_ls_name_cmp("ab/", "ab") == 0, "a directory and a file of one name compare equal");
}

/* '.' and '..' land at the top, which is where every other listing tool puts
 * them. */
static void test_dot_entries_come_first(void) {
    static const char* const names[] = {"ata.wap", "../", "./"};
    char out[256];

    order_of(names, 3, out, sizeof(out));
    CHECK(strcmp(out, "./,../,ata.wap") == 0, out);
}

/* An empty or single-entry listing must not be disturbed, and the comparator
 * must survive a NULL rather than fault on it. */
static void test_degenerate_inputs(void) {
    char pool[8] = "a";
    uint16_t offsets[1] = {0};

    cli_ls_sort(pool, offsets, 0);
    CHECK(offsets[0] == 0, "an empty sort changes nothing");
    cli_ls_sort(pool, offsets, 1);
    CHECK(offsets[0] == 0, "a single entry is already sorted");
    cli_ls_sort(NULL, offsets, 1);
    cli_ls_sort(pool, NULL, 1);
    CHECK(cli_ls_name_cmp(NULL, NULL) == 0, "NULL compares equal to NULL");
    CHECK(cli_ls_name_cmp("a", NULL) > 0, "a name sorts after NULL");
}

/* Equal names keep their arrival order, so a listing does not reshuffle between
 * runs when two entries compare the same (a name and its directory form). */
static void test_sort_is_stable(void) {
    char pool[16];
    uint16_t offsets[2];

    memcpy(&pool[0], "ab/", 4);
    memcpy(&pool[4], "ab", 3);
    offsets[0] = 0;
    offsets[1] = 4;
    cli_ls_sort(pool, offsets, 2);
    CHECK(offsets[0] == 0 && offsets[1] == 4, "equal names keep their arrival order");
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_sorts_alphabetically),
    WASMOS_TEST_CASE(test_orders_numbers_by_value_not_by_digit),
    WASMOS_TEST_CASE(test_zero_padding_does_not_change_the_order),
    WASMOS_TEST_CASE(test_ordering_is_case_insensitive),
    WASMOS_TEST_CASE(test_directory_marker_is_not_part_of_the_name),
    WASMOS_TEST_CASE(test_dot_entries_come_first),
    WASMOS_TEST_CASE(test_degenerate_inputs),
    WASMOS_TEST_CASE(test_sort_is_stable),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, sizeof(k_cases) / sizeof(k_cases[0]));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_cli_ls_order: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_cli_ls_order: ok\n");
    return 0;
}
