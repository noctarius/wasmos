/* Host unit test for directory record surgery (wfs_dirent.h, §10).
 *
 * Pure: one block buffer, no device and no runtime. The assertions are made by
 * WALKING the resulting chain the way the driver's scan walks it, not by reading
 * back the bytes the insert wrote — a chain that only the writer can traverse is
 * exactly the failure this format's stride rules exist to prevent.
 */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "wasmos_status.h"
#include "wfs_crc32c.h"
#include "wfs_dirent.h"
#include "wfs_endian.h"
#include "wfs_format.h"

static int g_failures;
static int g_checks;

static void expect(int cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("[fail] %s\n", what);
    }
}

static void expect_u32(uint32_t got, uint32_t want, const char* what) {
    g_checks++;
    if (got != want) {
        g_failures++;
        printf("[fail] %s: got %u, want %u\n", what, (unsigned)got, (unsigned)want);
    }
}

static void expect_rc(wasmos_error_code_t got, wasmos_error_code_t want, const char* what) {
    g_checks++;
    if (got != want) {
        g_failures++;
        printf("[fail] %s: got %s (%d), want %s (%d)\n",
               what,
               wasmos_strerror(got),
               (int)got,
               wasmos_strerror(want),
               (int)want);
    }
}

static const uint8_t k_uuid[WFS_UUID_LEN] = {
    0x30, 0x91, 0x4c, 0x02, 0xbb, 0x77, 0x41, 0x18, 0x8e, 0x5a, 0x22, 0xd9, 0x6f, 0x40, 0x13, 0xc7};

#define BS 4096u
#define LOC 512u

static uint8_t g_block[BS];

/* Walk the chain the way the driver's scan does, counting live records and
 * checking every stride. Returns the number of live entries, or -1 when the
 * chain breaks a §10 rule. */
static int32_t walk(const uint8_t* block, uint32_t* out_covered) {
    uint32_t usable = wfs_dir_usable_bytes(BS);
    uint32_t off = 0u;
    int32_t live = 0;

    while (off + WFS_DIR_ENTRY_HEADER <= usable) {
        uint32_t len = wfs_rd16(block, off + 8u);
        uint32_t nl = block[off + 10u];
        uint64_t id = wfs_rd64(block, off);

        if (len < WFS_DIR_RECORD_MIN || (len & 7u) != 0u || off + len > usable) {
            return -1;
        }
        if (nl > len - WFS_DIR_ENTRY_HEADER) {
            return -1;
        }
        if (id != 0u && nl != 0u) {
            live++;
        }
        off += len;
    }
    if (out_covered) {
        *out_covered = off;
    }
    /* The records must cover the usable area EXACTLY: a scan has to end where
     * the tail begins, so a chain stopping short leaves bytes no scan reaches. */
    return off == usable ? live : -1;
}

/* Is `name` reachable by a walk, with the id the caller expects? */
static int reachable(const uint8_t* block, const char* name, uint32_t id) {
    uint32_t usable = wfs_dir_usable_bytes(BS);
    uint32_t off = 0u;
    uint32_t want = (uint32_t)strlen(name);

    while (off + WFS_DIR_ENTRY_HEADER <= usable) {
        uint32_t len = wfs_rd16(block, off + 8u);
        uint32_t nl = block[off + 10u];
        uint64_t got = wfs_rd64(block, off);

        if (len < WFS_DIR_RECORD_MIN) {
            return 0;
        }
        if (got != 0u && nl == want &&
            memcmp(block + off + WFS_DIR_ENTRY_HEADER, name, want) == 0) {
            return got == (uint64_t)id;
        }
        off += len;
    }
    return 0;
}

static int sealed(const uint8_t* block) {
    uint32_t usable = wfs_dir_usable_bytes(BS);
    uint32_t off = usable + (uint32_t)offsetof(struct wfs_dir_tail, checksum);
    uint32_t stored = (uint32_t)wfs_rd16(block, off) | ((uint32_t)wfs_rd16(block, off + 2u) << 16);

    return stored == wfs_checksum_struct(k_uuid, LOC, block, BS, off);
}

static void fresh(void) {
    memset(g_block, 0, sizeof(g_block));
    wfs_dirent_init_block(g_block, BS, k_uuid, LOC);
}

/* ---- an initialised block ------------------------------------------------ */

/* A zeroed block is not a valid directory block: its first record has a stride of
 * 0, which a scan reads as a chain that never advances. */
static void test_a_zeroed_block_is_not_valid(void) {
    memset(g_block, 0, sizeof(g_block));
    expect_rc(wfs_dirent_validate(g_block, BS),
              WASMOS_ERR_FS_CORRUPT,
              "a zeroed block does not validate");
}

static void test_an_initialised_block_is_empty_and_valid(void) {
    uint32_t covered = 0u;

    fresh();
    expect_rc(wfs_dirent_validate(g_block, BS), WASMOS_ERR_NONE, "an initialised block validates");
    expect_u32((uint32_t)walk(g_block, &covered), 0u, "and carries no live record");
    expect_u32(covered, wfs_dir_usable_bytes(BS), "with the records covering the usable area");
    expect(sealed(g_block), "and the tail is sealed");
}

/* ---- insert ------------------------------------------------------------- */

static void test_an_inserted_name_is_reachable(void) {
    fresh();
    expect_rc(wfs_dirent_insert(g_block, BS, k_uuid, LOC, "hello", 5u, 42u, (uint8_t)WFS_TYPE_FILE),
              WASMOS_ERR_NONE,
              "the insert succeeds");
    expect_u32((uint32_t)walk(g_block, 0), 1u, "the chain carries one live record");
    expect(reachable(g_block, "hello", 42u), "and a walk finds it with its id");
    expect(sealed(g_block), "the tail is resealed");
    expect_rc(wfs_dirent_validate(g_block, BS), WASMOS_ERR_NONE, "and the chain still validates");
    expect(wfs_dirent_find(g_block, BS, "hello", 5u) >= 0, "find locates it");
    expect(wfs_dirent_find(g_block, BS, "hell", 4u) < 0, "and does not match a prefix");
}

static void test_several_inserts_all_stay_reachable(void) {
    static const char* names[] = {"a", "bb", "ccc", "dddd", "eeeee"};
    uint32_t i;

    fresh();
    for (i = 0; i < 5u; ++i) {
        expect_rc(wfs_dirent_insert(g_block,
                                    BS,
                                    k_uuid,
                                    LOC,
                                    names[i],
                                    (uint32_t)strlen(names[i]),
                                    100u + i,
                                    (uint8_t)WFS_TYPE_FILE),
                  WASMOS_ERR_NONE,
                  "each insert succeeds");
    }
    expect_u32((uint32_t)walk(g_block, 0), 5u, "all five are live");
    for (i = 0; i < 5u; ++i) {
        expect(reachable(g_block, names[i], 100u + i), "and each is reachable with its own id");
    }
}

/* A duplicate must be refused, and refused BEFORE anything is written: a failed
 * insert that consumed space would leak a record on every retry. */
static void test_a_duplicate_name_is_refused(void) {
    uint8_t before[BS];

    fresh();
    expect_rc(wfs_dirent_insert(g_block, BS, k_uuid, LOC, "dup", 3u, 7u, (uint8_t)WFS_TYPE_FILE),
              WASMOS_ERR_NONE,
              "the first insert succeeds");
    memcpy(before, g_block, BS);
    expect_rc(wfs_dirent_insert(g_block, BS, k_uuid, LOC, "dup", 3u, 9u, (uint8_t)WFS_TYPE_FILE),
              WASMOS_ERR_FS_EXISTS,
              "the duplicate is refused");
    expect_u32((uint32_t)memcmp(before, g_block, BS), 0u, "and the block is untouched");
}

static void test_a_bad_name_is_refused(void) {
    static char too_long[WFS_NAME_MAX + 2];

    memset(too_long, 'x', sizeof(too_long));
    fresh();
    expect_rc(wfs_dirent_insert(g_block, BS, k_uuid, LOC, "", 0u, 5u, (uint8_t)WFS_TYPE_FILE),
              WASMOS_ERR_FS_NAME,
              "an empty name is refused");
    expect_rc(
        wfs_dirent_insert(
            g_block, BS, k_uuid, LOC, too_long, WFS_NAME_MAX + 1u, 5u, (uint8_t)WFS_TYPE_FILE),
        WASMOS_ERR_FS_NAME,
        "a name past WFS_NAME_MAX is refused");
    /* The boundary itself must be accepted: name_length is one byte and 255 fits
     * it, so refusing 255 would cap names one short of the format. */
    expect_rc(wfs_dirent_insert(
                  g_block, BS, k_uuid, LOC, too_long, WFS_NAME_MAX, 5u, (uint8_t)WFS_TYPE_FILE),
              WASMOS_ERR_NONE,
              "a name of exactly WFS_NAME_MAX is accepted");
}

/* A block with no room refuses rather than overrunning into the tail. */
static void test_a_full_block_refuses(void) {
    uint32_t i;
    char name[8];
    int refused = 0;

    fresh();
    for (i = 0; i < 1000u; ++i) {
        wasmos_error_code_t rc;

        snprintf(name, sizeof(name), "n%04u", (unsigned)i);
        rc = wfs_dirent_insert(g_block,
                               BS,
                               k_uuid,
                               LOC,
                               name,
                               (uint32_t)strlen(name),
                               200u + i,
                               (uint8_t)WFS_TYPE_FILE);
        if (rc == WASMOS_ERR_FS_NO_SPACE) {
            refused = 1;
            break;
        }
        expect_rc(rc, WASMOS_ERR_NONE, "inserts succeed until the block is full");
    }
    expect(refused, "a full block reports no space");
    expect_rc(wfs_dirent_validate(g_block, BS), WASMOS_ERR_NONE, "and is still a valid chain");
    expect(walk(g_block, 0) > 0, "with its records intact");
}

/* ---- remove and merge --------------------------------------------------- */

static void test_a_removed_name_is_gone(void) {
    fresh();
    expect_rc(wfs_dirent_insert(g_block, BS, k_uuid, LOC, "keep", 4u, 1u, (uint8_t)WFS_TYPE_FILE),
              WASMOS_ERR_NONE,
              "insert keep");
    expect_rc(wfs_dirent_insert(g_block, BS, k_uuid, LOC, "drop", 4u, 2u, (uint8_t)WFS_TYPE_FILE),
              WASMOS_ERR_NONE,
              "insert drop");
    expect_rc(wfs_dirent_remove(g_block, BS, k_uuid, LOC, "drop", 4u),
              WASMOS_ERR_NONE,
              "the remove succeeds");
    expect_u32((uint32_t)walk(g_block, 0), 1u, "one live record remains");
    expect(reachable(g_block, "keep", 1u), "the survivor is still reachable");
    expect(wfs_dirent_find(g_block, BS, "drop", 4u) < 0, "and the removed name is not found");
    expect(sealed(g_block), "the tail is resealed");
    expect_rc(wfs_dirent_validate(g_block, BS), WASMOS_ERR_NONE, "the chain validates");
}

static void test_removing_an_absent_name_changes_nothing(void) {
    uint8_t before[BS];

    fresh();
    expect_rc(wfs_dirent_insert(g_block, BS, k_uuid, LOC, "here", 4u, 1u, (uint8_t)WFS_TYPE_FILE),
              WASMOS_ERR_NONE,
              "insert here");
    memcpy(before, g_block, BS);
    expect_rc(wfs_dirent_remove(g_block, BS, k_uuid, LOC, "gone", 4u),
              WASMOS_ERR_FS_NOT_FOUND,
              "removing an absent name reports not found");
    expect_u32((uint32_t)memcmp(before, g_block, BS), 0u, "and changes nothing");
}

/* The point of merging: two adjacent removals must come back as ONE gap, not two
 * holes. Without it a directory fragments into records too small to reuse and a
 * long name can never be inserted again even though the space exists. */
static void test_adjacent_removals_merge_into_one_gap(void) {
    /* Twenty characters, so its record is 32 bytes: it fits the 40 bytes the two
     * removals free TOGETHER (16 for "two", 24 for "three") and fits neither of
     * them alone. That arithmetic is the whole test -- a longer name would fail
     * for lack of space rather than for lack of merging. */
    static const char* long_name = "merged-gap-name-ok!!";
    uint32_t i;

    fresh();
    /* Three short names, then remove the middle two: their records are adjacent,
     * so the freed space is contiguous. */
    expect_rc(wfs_dirent_insert(g_block, BS, k_uuid, LOC, "one", 3u, 1u, (uint8_t)WFS_TYPE_FILE),
              WASMOS_ERR_NONE,
              "insert one");
    expect_rc(wfs_dirent_insert(g_block, BS, k_uuid, LOC, "two", 3u, 2u, (uint8_t)WFS_TYPE_FILE),
              WASMOS_ERR_NONE,
              "insert two");
    expect_rc(wfs_dirent_insert(g_block, BS, k_uuid, LOC, "three", 5u, 3u, (uint8_t)WFS_TYPE_FILE),
              WASMOS_ERR_NONE,
              "insert three");
    /* Fill the rest so the only space left is what the removals free. */
    for (i = 0; i < 1000u; ++i) {
        char name[8];

        snprintf(name, sizeof(name), "f%04u", (unsigned)i);
        if (wfs_dirent_insert(g_block,
                              BS,
                              k_uuid,
                              LOC,
                              name,
                              (uint32_t)strlen(name),
                              500u + i,
                              (uint8_t)WFS_TYPE_FILE) != WASMOS_ERR_NONE) {
            break;
        }
    }
    expect_rc(
        wfs_dirent_remove(g_block, BS, k_uuid, LOC, "two", 3u), WASMOS_ERR_NONE, "remove two");
    expect_rc(
        wfs_dirent_remove(g_block, BS, k_uuid, LOC, "three", 5u), WASMOS_ERR_NONE, "remove three");

    /* Merged, the two gaps hold a name neither could hold alone. */
    expect_rc(wfs_dirent_insert(g_block,
                                BS,
                                k_uuid,
                                LOC,
                                long_name,
                                (uint32_t)strlen(long_name),
                                99u,
                                (uint8_t)WFS_TYPE_FILE),
              WASMOS_ERR_NONE,
              "a name too long for either gap fits the merged one");
    expect(reachable(g_block, long_name, 99u), "and is reachable");
    expect_rc(wfs_dirent_validate(g_block, BS), WASMOS_ERR_NONE, "the chain validates");
}

/* Insert must be able to split a record that is IN USE. A directory mkfs_wfs
 * wrote has no free record at all -- its last entry's stride reaches the tail --
 * so an implementation that only reused free records could never insert into one. */
static void test_insert_splits_the_slack_of_a_used_record(void) {
    uint32_t usable = wfs_dir_usable_bytes(BS);

    memset(g_block, 0, sizeof(g_block));
    /* One live record whose stride spans the whole usable area, which is exactly
     * what a freshly formatted directory looks like. */
    g_block[0] = 5u; /* object_id = 5 */
    g_block[8] = (uint8_t)(usable & 0xFFu);
    g_block[9] = (uint8_t)((usable >> 8) & 0xFFu);
    g_block[10] = 4u; /* name_length */
    g_block[11] = (uint8_t)WFS_TYPE_FILE;
    memcpy(g_block + WFS_DIR_ENTRY_HEADER, "solo", 4u);
    g_block[usable + 8u] = (uint8_t)WFS_DIR_TAIL_SIZE;
    g_block[usable + 11u] = (uint8_t)WFS_DIR_TAIL_TYPE;
    wfs_dirent_seal(g_block, BS, k_uuid, LOC);

    expect_rc(wfs_dirent_validate(g_block, BS), WASMOS_ERR_NONE, "the one-record block validates");
    expect_rc(wfs_dirent_insert(g_block, BS, k_uuid, LOC, "next", 4u, 6u, (uint8_t)WFS_TYPE_FILE),
              WASMOS_ERR_NONE,
              "an insert takes the slack from the used record");
    expect_u32((uint32_t)walk(g_block, 0), 2u, "both records are live");
    expect(reachable(g_block, "solo", 5u), "the original is still reachable");
    expect(reachable(g_block, "next", 6u), "and so is the new one");
}

/* ---- path splitting ----------------------------------------------------- */

static void expect_split(const char* path, uint32_t want_parent, const char* want_name) {
    uint32_t parent_len = 0xFFFFFFFFu;
    const char* name = 0;
    uint32_t name_len = 0u;

    expect_rc(wfs_dirent_split_path(path, (uint32_t)strlen(path), &parent_len, &name, &name_len),
              WASMOS_ERR_NONE,
              "the split succeeds");
    expect_u32(parent_len, want_parent, "the parent length");
    expect_u32(name_len, (uint32_t)strlen(want_name), "the name length");
    expect(name && memcmp(name, want_name, strlen(want_name)) == 0, "and the name itself");
}

static void test_a_path_splits_into_parent_and_name(void) {
    expect_split("/wfs/docs/big.txt", 9u, "big.txt");
    expect_split("/hello", 1u, "hello");
    /* No separator: the parent length is 0, which a caller reads as "the
     * directory the client already stands in". Returning 1 here would turn a
     * relative name into an absolute one. */
    expect_split("hello", 0u, "hello");
    expect_split("docs/big.txt", 4u, "big.txt");
    /* Trailing separators name the same thing without them. */
    expect_split("docs/", 0u, "docs");
    expect_split("/wfs/docs//", 4u, "docs");
}

static void test_a_path_with_no_component_is_refused(void) {
    uint32_t parent_len = 0u;
    const char* name = 0;
    uint32_t name_len = 0u;
    static char too_long[WFS_NAME_MAX + 3];

    expect_rc(wfs_dirent_split_path("", 0u, &parent_len, &name, &name_len),
              WASMOS_ERR_FS_NAME,
              "an empty path has no component");
    expect_rc(wfs_dirent_split_path("/", 1u, &parent_len, &name, &name_len),
              WASMOS_ERR_FS_NAME,
              "the root names nothing to create");
    expect_rc(wfs_dirent_split_path("///", 3u, &parent_len, &name, &name_len),
              WASMOS_ERR_FS_NAME,
              "nor do separators alone");

    memset(too_long, 'x', sizeof(too_long));
    too_long[0] = '/';
    expect_rc(wfs_dirent_split_path(too_long, WFS_NAME_MAX + 2u, &parent_len, &name, &name_len),
              WASMOS_ERR_FS_NAME,
              "a component past WFS_NAME_MAX is refused");
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_a_zeroed_block_is_not_valid),
    WASMOS_TEST_CASE(test_an_initialised_block_is_empty_and_valid),
    WASMOS_TEST_CASE(test_an_inserted_name_is_reachable),
    WASMOS_TEST_CASE(test_several_inserts_all_stay_reachable),
    WASMOS_TEST_CASE(test_a_duplicate_name_is_refused),
    WASMOS_TEST_CASE(test_a_bad_name_is_refused),
    WASMOS_TEST_CASE(test_a_full_block_refuses),
    WASMOS_TEST_CASE(test_a_removed_name_is_gone),
    WASMOS_TEST_CASE(test_removing_an_absent_name_changes_nothing),
    WASMOS_TEST_CASE(test_adjacent_removals_merge_into_one_gap),
    WASMOS_TEST_CASE(test_insert_splits_the_slack_of_a_used_record),
    WASMOS_TEST_CASE(test_a_path_splits_into_parent_and_name),
    WASMOS_TEST_CASE(test_a_path_with_no_component_is_refused),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_dirent: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_dirent: %d checks passed\n", g_checks);
    return 0;
}
