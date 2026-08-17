/* test_fat_dir.c — directory scanning across a cluster chain (fat_dir.c).
 *
 * A FAT directory that is not the root is a cluster CHAIN, not a single run of
 * sectors. fat_find_in_dir is the only scan in fat_dir.c that carries code to
 * follow that chain, and this suite pins that the code actually runs.
 *
 * The volume is built in RAM and served synchronously: the fake block layer
 * below replaces fat_block.c, so fat_need_sector always returns FAT_R_DONE and
 * every coroutine here completes in a single call rather than yielding. That is
 * the only reason a reactor built for one-block-at-a-time I/O can be driven
 * straight from a host test.
 *
 * Geometry is deliberately minimal: 512-byte sectors, ONE sector per cluster,
 * so a cluster holds exactly 16 directory entries and a two-cluster directory
 * needs only 17. The cluster count is what forces FAT16 (the specification's
 * [4085, 65525) window), which is why the image is ~2.5 MB of otherwise unused
 * sectors — fat_alloc.c serves FAT12 and FAT16 only.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_shuffle.h"

#include "fat_alloc.h"
#include "fat_block.h"
#include "fat_dir.h"
#include "fat_geom.h"
#include "fat_types.h"

/* The generated import declarations, so the stubs below are checked rather than
 * merely name-matched at link. WASMOS_WASM_IMPORT is a no-op off wasm32, and
 * the two struct types the declarations reference are needed only as incomplete
 * pointees here -- pulling in wasmos/api.h's full prerequisite chain would drag
 * the guest headers into a host compile for no gain. */
#define WASMOS_WASM_IMPORT(module, name)
typedef struct wasmos_physmem_stats wasmos_physmem_stats_t;
typedef struct wasmos_framebuffer_info wasmos_framebuffer_info_t;
typedef struct wasmos_msi_desc wasmos_msi_desc_t;
#include "wasmos_imports.h"

/* --- Volume geometry (see the header comment for why these values). --- */

#define T_SECTOR 512u
#define T_SECTORS_PER_CLUSTER 1u
#define T_RESERVED 1u
#define T_FAT_COUNT 1u
#define T_ROOT_ENTRIES 16u
#define T_FAT_SECTORS 20u
#define T_TOTAL_SECTORS 5000u

/* Derived, and asserted against the mounted geometry in build_volume(). */
#define T_FAT_LBA T_RESERVED
#define T_ROOT_LBA (T_RESERVED + (T_FAT_COUNT * T_FAT_SECTORS))
#define T_FIRST_DATA_LBA (T_ROOT_LBA + 1u)

#define T_ENTRIES_PER_CLUSTER ((T_SECTOR * T_SECTORS_PER_CLUSTER) / 32u)

/* The directory under test occupies clusters 2 -> 3. Its target entry lives in
 * the SECOND cluster, which is the whole point. */
#define T_DIR_FIRST_CLUSTER 2u
#define T_DIR_SECOND_CLUSTER 3u

#define T_TARGET_CLUSTER 0x0042u
#define T_TARGET_SIZE 1234u

/* --- Fake block layer (replaces fat_block.c). --- */

static uint8_t* g_image;
static int32_t g_last_err;

static uint8_t* image_sector(uint32_t lba) {
    assert(lba < T_TOTAL_SECTORS);
    return g_image + ((size_t)lba * T_SECTOR);
}

uint8_t* fat_block_sector(fat_block_t* blk) {
    return blk->sector;
}

/* Synchronous by construction: the sector is already in memory, so this never
 * reports FAT_R_WAIT and no caller here ever has to be resumed. */
fat_r_t fat_need_sector(fat_block_t* blk, uint32_t lba) {
    if (lba >= T_TOTAL_SECTORS) {
        return FAT_R_ERR;
    }
    memcpy(blk->sector, image_sector(lba), T_SECTOR);
    blk->loaded_lba = lba;
    return FAT_R_DONE;
}

void fat_block_set_err(fat_block_t* blk, int32_t err) {
    (void)blk;
    g_last_err = err;
}

/* Unreached by the read-only cases here; defined because fat_dir.c and
 * fat_alloc.c reference them, and a stub that silently succeeded would let a
 * mutation path appear to work. */
fat_r_t fat_block_write(fat_block_t* blk, uint32_t lba) {
    (void)blk;
    (void)lba;
    assert(0 && "fat_block_write: no case here writes");
    return FAT_R_ERR;
}

fat_r_t fat_block_read_direct(fat_block_t* blk, uint32_t lba, uint32_t count, int32_t buffer_id,
                              int32_t borrow_id, uint32_t dst_offset) {
    (void)blk;
    (void)lba;
    (void)count;
    (void)buffer_id;
    (void)borrow_id;
    (void)dst_offset;
    assert(0 && "fat_block_read_direct: no case here reads a file body");
    return FAT_R_ERR;
}

/* --- Guest API stubs: the three calls fat_dir.c makes, all on the readdir
 * streaming path.
 *
 * wasmos_imports.h is included so the compiler checks these against the real
 * declarations. It is worth the include: an earlier revision of this file gave
 * wasmos_ipc_send seven parameters against the real eight, which compiled
 * because nothing here pulled the declaration in and linked because C matches
 * by name alone. --- */

int32_t wasmos_console_write(int32_t ptr, int32_t len) {
    (void)ptr;
    (void)len;
    return 0;
}

/* READDIR streams a listing one byte per argument word. The bytes are collected
 * here so a case can assert on the listing the client would have received. */
#define T_STREAM_CAP 1024u
static char g_stream[T_STREAM_CAP];
static uint32_t g_stream_len;

static void stream_put(int32_t byte) {
    if (byte != 0 && g_stream_len + 1u < T_STREAM_CAP) {
        g_stream[g_stream_len++] = (char)byte;
        g_stream[g_stream_len] = '\0';
    }
}

int32_t wasmos_ipc_send(int32_t dest, int32_t src, int32_t type, int32_t request_id, int32_t arg0,
                        int32_t arg1, int32_t arg2, int32_t arg3) {
    (void)dest;
    (void)src;
    (void)type;
    (void)request_id;
    stream_put(arg0);
    stream_put(arg1);
    stream_put(arg2);
    stream_put(arg3);
    return 0;
}

int32_t wasmos_sched_yield(void) {
    return 0;
}

/* --- Image construction. --- */

static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Store a FAT16 chain entry: cluster N's successor sits at byte offset N*2 of
 * the table region. */
static void fat_set(uint16_t cluster, uint16_t value) {
    uint32_t offset = (uint32_t)cluster * 2u;
    uint8_t* p = image_sector(T_FAT_LBA + (offset / T_SECTOR)) + (offset % T_SECTOR);
    put16(p, value);
}

/* Address of directory slot `index` within a directory sector. */
static uint8_t* dirent_slot(uint8_t* base, uint32_t index) {
    return base + ((size_t)index * 32u);
}

/* Write one 8.3 short-name directory entry at slot `index`. `name` is the padded
 * 11-byte on-disk form ("TARGET  TXT"), not the display form. */
static void put_dirent(uint8_t* base, uint32_t index, const char* name, uint8_t attr,
                       uint16_t cluster, uint32_t size) {
    uint8_t* slot = dirent_slot(base, index);

    memset(slot, 0, 32);
    memcpy(slot, name, 11);
    slot[11] = attr;
    put16(slot + 26, cluster);
    put32(slot + 28, size);
}

/* Build the volume: BPB, FAT chain 2 -> 3 -> EOC, a root directory, and the
 * two-cluster subdirectory the cases scan.
 *
 * The first cluster is filled to its LAST entry on purpose. A scan stops at the
 * first 0x00 (end-of-directory), so leaving even one free slot there would end
 * the scan legitimately and the case could not distinguish a budget defect from
 * a correct stop.
 *
 * `filler_deleted` chooses what fills it. Real entries (0) suit the lookup and
 * listing cases. Tombstones (1) are what the emptiness case needs: a scan
 * ignores 0xE5, so the first cluster is full yet contains no child, and the
 * directory's only real child is the one in the second cluster. */
static void build_volume(int filler_deleted) {
    uint8_t* boot;
    uint8_t* cluster1;
    uint8_t* cluster2;
    uint8_t* root;
    char filler[12];
    uint32_t i;

    g_image = calloc(T_TOTAL_SECTORS, T_SECTOR);
    assert(g_image != NULL);

    boot = image_sector(0);
    boot[0] = 0xEB; /* jump */
    boot[1] = 0x3C;
    boot[2] = 0x90;
    memcpy(boot + 3, "WASMOSFT", 8);
    put16(boot + 11, (uint16_t)T_SECTOR);        /* bytes_per_sector */
    boot[13] = (uint8_t)T_SECTORS_PER_CLUSTER;   /* sectors_per_cluster */
    put16(boot + 14, (uint16_t)T_RESERVED);      /* reserved_sectors */
    boot[16] = (uint8_t)T_FAT_COUNT;             /* fat_count */
    put16(boot + 17, (uint16_t)T_ROOT_ENTRIES);  /* root_entry_count */
    put16(boot + 19, (uint16_t)T_TOTAL_SECTORS); /* total_sectors_16 */
    boot[21] = 0xF8;                             /* media */
    put16(boot + 22, (uint16_t)T_FAT_SECTORS);   /* fat_size_16 */
    put16(boot + 510, 0xAA55);                   /* signature */

    /* Reserved entries, then the subdirectory's two-cluster chain. */
    fat_set(0, 0xFFF8u);
    fat_set(1, 0xFFFFu);
    fat_set(T_DIR_FIRST_CLUSTER, (uint16_t)T_DIR_SECOND_CLUSTER);
    fat_set(T_DIR_SECOND_CLUSTER, 0xFFFFu); /* end-of-chain */

    /* Root directory: the subdirectory itself, plus a plain file so a root scan
     * has something to find. */
    root = image_sector(T_ROOT_LBA);
    put_dirent(root, 0, "SUB        ", 0x10, (uint16_t)T_DIR_FIRST_CLUSTER, 0);
    put_dirent(root, 1, "INROOT  TXT", 0x20, 0x0099u, 7u);
    /* Entry 2 stays zeroed: end-of-directory for the root region. */

    /* First cluster: "." and "..", then filler to the last slot. */
    cluster1 = image_sector(T_FIRST_DATA_LBA + (T_DIR_FIRST_CLUSTER - 2u));
    put_dirent(cluster1, 0, ".          ", 0x10, (uint16_t)T_DIR_FIRST_CLUSTER, 0);
    put_dirent(cluster1, 1, "..         ", 0x10, 0, 0);
    for (i = 2; i < T_ENTRIES_PER_CLUSTER; ++i) {
        memcpy(filler, "FILLER00   ", 12);
        filler[6] = (char)('0' + (char)((i - 2u) / 10u));
        filler[7] = (char)('0' + (char)((i - 2u) % 10u));
        put_dirent(cluster1, i, filler, 0x20, (uint16_t)(0x100u + i), 16u);
        if (filler_deleted) {
            dirent_slot(cluster1, i)[0] = 0xE5; /* deleted: occupies a slot, is not a child */
        }
    }

    /* Second cluster: the target, then end-of-directory. */
    cluster2 = image_sector(T_FIRST_DATA_LBA + (T_DIR_SECOND_CLUSTER - 2u));
    put_dirent(cluster2, 0, "TARGET  TXT", 0x20, (uint16_t)T_TARGET_CLUSTER, T_TARGET_SIZE);
}

static void free_volume(void) {
    free(g_image);
    g_image = NULL;
}

/* Mount the image through the real geometry parser rather than hand-filling
 * fat_mount_t, so a case can never assert against geometry the driver would
 * not itself produce. */
static void mount_volume(fat_mount_t* mnt, fat_block_t* blk) {
    fat_r_t r;

    memset(blk, 0, sizeof(*blk));
    blk->loaded_lba = FAT_BLOCK_NO_LBA;
    fat_mount_init(mnt);

    r = fat_geom_mount_step(mnt, blk);
    assert(r == FAT_R_DONE && "mount failed: the case cannot say anything about scanning");
    assert(fat_mount_ready(mnt));
    assert(mnt->fat_type == FAT_TYPE_16 && "geometry must land in the FAT16 cluster window");
    assert(mnt->root_dir_lba == T_ROOT_LBA);
    assert(fat_first_data_lba(mnt) == T_FIRST_DATA_LBA);
    assert(fat_dir_entry_limit(mnt, 0, mnt->sectors_per_cluster) == T_ENTRIES_PER_CLUSTER);
}

/* Run a scan of the two-cluster subdirectory for `target` to completion. */
static fat_r_t scan_subdir(fat_dir_scan_ctx_t* s, fat_block_t* blk, const fat_mount_t* mnt,
                           const char* target) {
    memset(s, 0, sizeof(*s));
    s->target = target;
    s->cur_root = 0;
    s->cur_cluster = (uint16_t)T_DIR_FIRST_CLUSTER;
    s->dir_lba = fat_lba_for_cluster(mnt, (uint16_t)T_DIR_FIRST_CLUSTER);
    s->dir_sectors = mnt->sectors_per_cluster;
    s->entry_limit = fat_dir_entry_limit(mnt, 0, s->dir_sectors);
    return fat_find_in_dir(s, blk, mnt);
}

static int g_failures;

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL %s:%d: %s\n", __func__, __LINE__, (what));                              \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

/* Regression: 2026-08-17-fat-dir-multicluster -- fat_dir_entry_limit budgets a
 * non-root scan for ONE cluster's entries, so entries_left hit 0 at the end of
 * the first cluster and fat_find_in_dir broke out before the chain hop that
 * follows. Every entry in a directory's second and later clusters was invisible
 * to lookup, which under Bochs (a real FAT16 image, unlike QEMU's synthesized
 * one) made /boot/apps entries past the first cluster fail to spawn. */
static void test_finds_entry_in_second_cluster(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_dir_scan_ctx_t s;
    fat_r_t r;

    build_volume(0);
    mount_volume(&mnt, &blk);

    r = scan_subdir(&s, &blk, &mnt, "TARGET.TXT");

    CHECK(r == FAT_R_DONE, "scan completed");
    CHECK(s.found.valid == 1, "entry in the directory's SECOND cluster was found");
    CHECK(s.found.cluster == T_TARGET_CLUSTER, "found entry carries the target's start cluster");
    CHECK(s.found.size == T_TARGET_SIZE, "found entry carries the target's size");

    free_volume();
}

/* The fix must not cost the common case: an entry in the first cluster is still
 * found, and still reports the position the caller mutates through. */
static void test_still_finds_entry_in_first_cluster(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_dir_scan_ctx_t s;
    fat_r_t r;

    build_volume(0);
    mount_volume(&mnt, &blk);

    r = scan_subdir(&s, &blk, &mnt, "FILLER00");

    CHECK(r == FAT_R_DONE, "scan completed");
    CHECK(s.found.valid == 1, "entry in the first cluster is still found");
    CHECK(s.found.dir_lba == fat_lba_for_cluster(&mnt, (uint16_t)T_DIR_FIRST_CLUSTER),
          "found entry reports the cluster it actually lives in");
    CHECK(s.found.dir_index == 2u, "found entry reports its slot within that cluster");

    free_volume();
}

/* A name that is in neither cluster must terminate and report a miss. This is
 * what would catch a chain walk that never stops -- the suite would hang rather
 * than fail, which is exactly the failure mode the fix's hop bound prevents. */
static void test_absent_name_terminates_with_a_miss(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_dir_scan_ctx_t s;
    fat_r_t r;

    build_volume(0);
    mount_volume(&mnt, &blk);

    r = scan_subdir(&s, &blk, &mnt, "NOSUCH.TXT");

    CHECK(r == FAT_R_DONE, "scan completed rather than erroring");
    CHECK(s.found.valid == 0, "absent name reports a miss");

    free_volume();
}

/* A cyclic FAT chain is a corrupt volume, not an impossible one. The scan must
 * give up instead of walking forever; before the hop bound existed the
 * one-cluster budget was what accidentally provided this. */
static void test_cyclic_chain_terminates(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_dir_scan_ctx_t s;
    fat_r_t r;

    build_volume(0);
    /* Point the second cluster back at the first: 2 -> 3 -> 2 -> ... */
    fat_set((uint16_t)T_DIR_SECOND_CLUSTER, (uint16_t)T_DIR_FIRST_CLUSTER);
    mount_volume(&mnt, &blk);

    r = scan_subdir(&s, &blk, &mnt, "NOSUCH.TXT");

    CHECK(r == FAT_R_DONE || r == FAT_R_ERR, "scan terminated on a cyclic chain");
    CHECK(s.found.valid == 0, "no bogus match from a cyclic chain");

    free_volume();
}

/* The root directory is a fixed contiguous run with no chain, so its scan must
 * stop at root_entry_count and must never hop. */
static void test_root_scan_is_unchanged(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_dir_scan_ctx_t s;
    fat_r_t r;

    build_volume(0);
    mount_volume(&mnt, &blk);

    memset(&s, 0, sizeof(s));
    s.target = "INROOT.TXT";
    s.cur_root = 1;
    s.cur_cluster = 0;
    s.dir_lba = mnt.root_dir_lba;
    s.dir_sectors = mnt.root_dir_sectors;
    s.entry_limit = fat_dir_entry_limit(&mnt, 1, s.dir_sectors);
    r = fat_find_in_dir(&s, &blk, &mnt);

    CHECK(r == FAT_R_DONE, "root scan completed");
    CHECK(s.found.valid == 1, "root entry found");
    CHECK(s.found.cluster == 0x0099u, "root entry carries its start cluster");

    free_volume();
}

/* Regression: 2026-08-17-fat-dir-multicluster -- fat_short_name_exists_in_dir
 * has no chain-hop code, so a colliding 8.3 short name living past the first
 * cluster reported ABSENT and fat_build_short_alias would mint a duplicate
 * alias for a name already on the volume. */
static void test_short_name_collision_seen_in_second_cluster(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_shortscan_ctx_t s;
    fat_r_t r;
    uint32_t i;
    static const char k_target[11] = {'T', 'A', 'R', 'G', 'E', 'T', ' ', ' ', 'T', 'X', 'T'};

    build_volume(0);
    mount_volume(&mnt, &blk);

    memset(&s, 0, sizeof(s));
    s.dir_lba = fat_lba_for_cluster(&mnt, (uint16_t)T_DIR_FIRST_CLUSTER);
    s.dir_sectors = mnt.sectors_per_cluster;
    s.entry_limit = fat_dir_entry_limit(&mnt, 0, s.dir_sectors);
    s.cur_cluster = (uint16_t)T_DIR_FIRST_CLUSTER;
    s.cur_root = 0;
    for (i = 0; i < 11u; ++i) {
        s.short_name[i] = (uint8_t)k_target[i];
    }
    r = fat_short_name_exists_in_dir(&s, &blk, &mnt);

    CHECK(r == FAT_R_DONE, "short-name scan completed");
    CHECK(s.result == 1, "a short name in the SECOND cluster is reported present");

    free_volume();
}

/* Regression: 2026-08-17-fat-dir-multicluster -- fat_dir_is_empty_step has no
 * chain-hop code, so a directory whose only children live past the first
 * cluster reported EMPTY. rmdir believes that answer: it deletes the entry and
 * then fat_free_cluster_chain frees the clusters those children occupy. This
 * is the data-loss member of the family, which is why the case builds the
 * first cluster full of TOMBSTONES -- full, yet genuinely childless. */
static void test_child_in_second_cluster_makes_directory_not_empty(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_dirempty_ctx_t e;
    fat_r_t r;

    build_volume(1);
    mount_volume(&mnt, &blk);

    memset(&e, 0, sizeof(e));
    e.dir_lba = fat_lba_for_cluster(&mnt, (uint16_t)T_DIR_FIRST_CLUSTER);
    e.dir_sectors = mnt.sectors_per_cluster;
    e.cur_cluster = (uint16_t)T_DIR_FIRST_CLUSTER;
    r = fat_dir_is_empty_step(&e, &blk, &mnt);

    CHECK(r == FAT_R_DONE, "emptiness check completed");
    CHECK(e.result == 0, "a child in the SECOND cluster makes the directory non-empty");

    free_volume();
}

/* A directory that really is empty must still report empty once the walk covers
 * the whole chain -- otherwise the fix above would simply make rmdir refuse
 * everything, which the case above alone could not detect. */
static void test_directory_with_no_children_is_still_empty(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_dirempty_ctx_t e;
    fat_r_t r;
    uint8_t* cluster2;

    build_volume(1);
    /* Drop the second cluster's only real child, leaving the whole chain
     * childless. */
    cluster2 = image_sector(T_FIRST_DATA_LBA + (T_DIR_SECOND_CLUSTER - 2u));
    memset(cluster2, 0, T_SECTOR);
    mount_volume(&mnt, &blk);

    memset(&e, 0, sizeof(e));
    e.dir_lba = fat_lba_for_cluster(&mnt, (uint16_t)T_DIR_FIRST_CLUSTER);
    e.dir_sectors = mnt.sectors_per_cluster;
    e.cur_cluster = (uint16_t)T_DIR_FIRST_CLUSTER;
    r = fat_dir_is_empty_step(&e, &blk, &mnt);

    CHECK(r == FAT_R_DONE, "emptiness check completed");
    CHECK(e.result == 1, "a directory with no children anywhere in its chain is empty");

    free_volume();
}

/* Regression: 2026-08-17-fat-dir-multicluster -- the READDIR scan sizes its
 * budget from mnt->dir_sectors (one cluster) and never hops, so a listing was
 * truncated at the first cluster and entries beyond it never reached the
 * client. */
static void test_readdir_lists_entries_in_second_cluster(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_op_ctx_t op;
    fat_r_t r;

    build_volume(0);
    mount_volume(&mnt, &blk);

    /* Make the two-cluster subdirectory the cwd, which is what READDIR lists. */
    mnt.cwd_root = 0;
    mnt.cwd_source = 7;
    mnt.cwd_cluster = (uint16_t)T_DIR_FIRST_CLUSTER;
    mnt.dir_lba = fat_lba_for_cluster(&mnt, (uint16_t)T_DIR_FIRST_CLUSTER);
    mnt.dir_sectors = mnt.sectors_per_cluster;

    memset(&op, 0, sizeof(op));
    op.source = 7;
    op.request_id = 1;
    g_stream_len = 0;
    g_stream[0] = '\0';

    r = fat_op_readdir(&op, &blk, &mnt, 9);

    CHECK(r == FAT_R_DONE, "readdir completed");
    CHECK(strstr(g_stream, "FILLER00") != NULL, "listing includes a first-cluster entry");
    CHECK(strstr(g_stream, "TARGET.TXT") != NULL, "listing includes the SECOND cluster's entry");

    free_volume();
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_finds_entry_in_second_cluster),
    WASMOS_TEST_CASE(test_still_finds_entry_in_first_cluster),
    WASMOS_TEST_CASE(test_absent_name_terminates_with_a_miss),
    WASMOS_TEST_CASE(test_cyclic_chain_terminates),
    WASMOS_TEST_CASE(test_root_scan_is_unchanged),
    WASMOS_TEST_CASE(test_short_name_collision_seen_in_second_cluster),
    WASMOS_TEST_CASE(test_child_in_second_cluster_makes_directory_not_empty),
    WASMOS_TEST_CASE(test_directory_with_no_children_is_still_empty),
    WASMOS_TEST_CASE(test_readdir_lists_entries_in_second_cluster),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, sizeof(k_cases) / sizeof(k_cases[0]));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_fat_dir: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_fat_dir: ok\n");
    return 0;
}