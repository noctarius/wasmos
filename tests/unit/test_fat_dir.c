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
#include "fat_file.h"
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
static uint32_t g_image_sectors; /* size of the volume currently built */
static int32_t g_last_err;

static uint8_t* image_sector(uint32_t lba) {
    assert(lba < g_image_sectors);
    return g_image + ((size_t)lba * T_SECTOR);
}

uint8_t* fat_block_sector(fat_block_t* blk) {
    return blk->sector;
}

/* Synchronous by construction: the sector is already in memory, so this never
 * reports FAT_R_WAIT and no caller here ever has to be resumed. */
fat_r_t fat_need_sector(fat_block_t* blk, uint32_t lba) {
    if (lba >= g_image_sectors) {
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

/* Writes land in the image, so a mutation case can read back what the driver
 * actually stored rather than trusting its return code.  Synchronous for the
 * same reason fat_need_sector is: the coroutine completes in one call. */
fat_r_t fat_block_write(fat_block_t* blk, uint32_t lba) {
    if (lba >= g_image_sectors) {
        return FAT_R_ERR;
    }
    memcpy(image_sector(lba), blk->sector, T_SECTOR);
    blk->loaded_lba = lba;
    return FAT_R_DONE;
}

void fat_block_invalidate(fat_block_t* blk) {
    blk->loaded_lba = FAT_BLOCK_NO_LBA;
}

/* The client data path: reached only by fat_op_read/fat_op_write, which need
 * the transfer-buffer plumbing this harness does not model.  They abort rather
 * than return a plausible value, so a case that strayed onto the data path
 * fails loudly instead of asserting against a fiction. */
uint32_t fat_block_direct_sectors(const fat_block_t* blk) {
    (void)blk;
    assert(0 && "fat_block_direct_sectors: no case here reads file data");
    return 0;
}

int32_t fat_block_server_endpoint(const fat_block_t* blk) {
    (void)blk;
    assert(0 && "fat_block_server_endpoint: no case here reads file data");
    return -1;
}

int32_t wasmos_xfer_buffer_read(int32_t buffer_id, void* dst, int32_t len, int32_t offset) {
    (void)buffer_id;
    (void)dst;
    (void)len;
    (void)offset;
    assert(0 && "wasmos_xfer_buffer_read: no case here touches a client buffer");
    return -1;
}

int32_t wasmos_xfer_buffer_reborrow(int32_t grantee, int32_t borrow_id, int32_t flags) {
    (void)grantee;
    (void)borrow_id;
    (void)flags;
    assert(0 && "wasmos_xfer_buffer_reborrow: no case here touches a client buffer");
    return -1;
}

int32_t wasmos_xfer_buffer_write(int32_t buffer_id, const void* src, int32_t len, int32_t offset) {
    (void)buffer_id;
    (void)src;
    (void)len;
    (void)offset;
    assert(0 && "wasmos_xfer_buffer_write: no case here touches a client buffer");
    return -1;
}

int32_t wasmos_xfer_buffer_size(void) {
    assert(0 && "wasmos_xfer_buffer_size: no case here touches a client buffer");
    return -1;
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
    g_image_sectors = T_TOTAL_SECTORS;

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
    g_image_sectors = 0;
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

/* --- FAT32 ---------------------------------------------------------------
 *
 * A second volume, because FAT32 differs from FAT16 in three ways that the
 * cases below separate deliberately: 32-bit FAT entries (28 significant bits),
 * a start cluster whose high half lives at dirent offset 20-21, and a ROOT
 * directory that is an ordinary cluster chain rather than a fixed region.
 *
 * The geometry is spec-legal rather than merely accepted: fat_parse_boot keys
 * FAT32 off root_entry_count == 0 and would take a tiny volume, but a cluster
 * count below 65525 is a FAT16 volume by the specification and a test that
 * relied on the shortcut would not be testing FAT32. Hence ~69k clusters and a
 * 35 MB image -- calloc leaves it lazily mapped, and the cases touch a handful
 * of sectors. */

#define T32_SECTOR 512u
#define T32_SECTORS_PER_CLUSTER 1u
#define T32_RESERVED 32u
#define T32_FAT_COUNT 1u
#define T32_FAT_SECTORS 550u
#define T32_TOTAL_SECTORS 70000u

#define T32_FAT_LBA T32_RESERVED
#define T32_FIRST_DATA_LBA (T32_RESERVED + (T32_FAT_COUNT * T32_FAT_SECTORS))
#define T32_ENTRIES_PER_CLUSTER ((T32_SECTOR * T32_SECTORS_PER_CLUSTER) / 32u)

#define T32_ROOT_CLUSTER 2u
#define T32_DIR_FIRST_CLUSTER 3u
#define T32_DIR_SECOND_CLUSTER 4u

/* Deliberately above 0xFFFF: a start cluster that a 16-bit field truncates,
 * which is what makes the width of the cluster type observable. */
#define T32_TARGET_CLUSTER 0x12345u

static uint8_t* cluster32_sector(uint32_t cluster) {
    return image_sector(T32_FIRST_DATA_LBA + (cluster - 2u));
}

/* Store a FAT32 chain entry. Only the low 28 bits are the cluster number; the
 * top 4 are reserved and must survive a write, which test_fat32_fatent_write_
 * preserves_reserved_bits checks. */
static void fat32_set(uint32_t cluster, uint32_t value) {
    uint32_t offset = cluster * 4u;
    uint8_t* p = image_sector(T32_FAT_LBA + (offset / T32_SECTOR)) + (offset % T32_SECTOR);
    put32(p, value);
}

/* FAT32 splits a start cluster across two dirent fields: low half at 26-27 (as
 * on FAT16) and high half at 20-21. */
static void put_dirent32(uint8_t* base, uint32_t index, const char* name, uint8_t attr,
                         uint32_t cluster, uint32_t size) {
    uint8_t* slot = dirent_slot(base, index);

    memset(slot, 0, 32);
    memcpy(slot, name, 11);
    slot[11] = attr;
    put16(slot + 20, (uint16_t)((cluster >> 16) & 0xFFFFu));
    put16(slot + 26, (uint16_t)(cluster & 0xFFFFu));
    put32(slot + 28, size);
}

static void build_volume32(void) {
    uint8_t* boot;
    uint8_t* root;
    uint8_t* cluster1;
    uint8_t* cluster2;
    char filler[12];
    uint32_t i;

    g_image = calloc(T32_TOTAL_SECTORS, T32_SECTOR);
    assert(g_image != NULL);
    g_image_sectors = T32_TOTAL_SECTORS;

    boot = image_sector(0);
    boot[0] = 0xEB;
    boot[1] = 0x58;
    boot[2] = 0x90;
    memcpy(boot + 3, "WASMOS32", 8);
    put16(boot + 11, (uint16_t)T32_SECTOR);
    boot[13] = (uint8_t)T32_SECTORS_PER_CLUSTER;
    put16(boot + 14, (uint16_t)T32_RESERVED);
    boot[16] = (uint8_t)T32_FAT_COUNT;
    put16(boot + 17, 0);                 /* root_entry_count: 0 marks FAT32 */
    put16(boot + 19, 0);                 /* total_sectors_16: 0, the 32-bit field is used */
    boot[21] = 0xF8;                     /* media */
    put16(boot + 22, 0);                 /* fat_size_16: 0, FATSz32 below is used */
    put32(boot + 32, T32_TOTAL_SECTORS); /* total_sectors_32 */
    put32(boot + 36, T32_FAT_SECTORS);   /* FATSz32  (bpb->ext[0..3]) */
    put32(boot + 44, T32_ROOT_CLUSTER);  /* BPB_RootClus (bpb->ext[8..11]) */
    put16(boot + 510, 0xAA55);

    fat32_set(0, 0x0FFFFFF8u);
    fat32_set(1, 0x0FFFFFFFu);
    /* The root is a single-cluster chain; the subdirectory spans two. */
    fat32_set(T32_ROOT_CLUSTER, 0x0FFFFFFFu);
    fat32_set(T32_DIR_FIRST_CLUSTER, T32_DIR_SECOND_CLUSTER);
    fat32_set(T32_DIR_SECOND_CLUSTER, 0x0FFFFFFFu);

    root = cluster32_sector(T32_ROOT_CLUSTER);
    put_dirent32(root, 0, "SUB        ", 0x10, T32_DIR_FIRST_CLUSTER, 0);
    put_dirent32(root, 1, "INROOT  TXT", 0x20, 0x0099u, 7u);

    cluster1 = cluster32_sector(T32_DIR_FIRST_CLUSTER);
    put_dirent32(cluster1, 0, ".          ", 0x10, T32_DIR_FIRST_CLUSTER, 0);
    put_dirent32(cluster1, 1, "..         ", 0x10, 0, 0);
    for (i = 2; i < T32_ENTRIES_PER_CLUSTER; ++i) {
        memcpy(filler, "FILLER00   ", 12);
        filler[6] = (char)('0' + (char)((i - 2u) / 10u));
        filler[7] = (char)('0' + (char)((i - 2u) % 10u));
        put_dirent32(cluster1, i, filler, 0x20, 0x100u + i, 16u);
    }

    cluster2 = cluster32_sector(T32_DIR_SECOND_CLUSTER);
    put_dirent32(cluster2, 0, "TARGET  TXT", 0x20, T32_TARGET_CLUSTER, T_TARGET_SIZE);
}

static void mount_volume32(fat_mount_t* mnt, fat_block_t* blk) {
    fat_r_t r;

    memset(blk, 0, sizeof(*blk));
    blk->loaded_lba = FAT_BLOCK_NO_LBA;
    fat_mount_init(mnt);

    r = fat_geom_mount_step(mnt, blk);
    assert(r == FAT_R_DONE && "FAT32 mount failed: the case cannot say anything about scanning");
    assert(fat_mount_ready(mnt));
    assert(mnt->fat_type == FAT_TYPE_32);
    assert(fat_first_data_lba(mnt) == T32_FIRST_DATA_LBA);
}

/* The mount must describe a FAT32 volume rather than merely accept it: the root
 * is a cluster, not a fixed region, and everything else keys off that. */
static void test_fat32_mount_reports_a_chained_root(void) {
    fat_mount_t mnt;
    fat_block_t blk;

    build_volume32();
    mount_volume32(&mnt, &blk);

    CHECK(mnt.fat_type == FAT_TYPE_32, "volume is detected as FAT32");
    CHECK(mnt.root_cluster == T32_ROOT_CLUSTER, "root cluster comes from BPB_RootClus");
    CHECK(mnt.root_entry_count == 0, "FAT32 has no fixed root entry count");
    CHECK(mnt.fat_size == T32_FAT_SECTORS, "FAT size comes from FATSz32");
    CHECK(fat_total_clusters(&mnt) >= 65525u, "cluster count is in the FAT32 range");

    free_volume();
}

/* A FAT32 FAT entry is 32 bits with only the low 28 significant, so a chain hop
 * must mask rather than truncate. */
static void test_fat32_chain_next_reads_a_28_bit_entry(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_chain_ctx_t c;
    fat_r_t r;

    build_volume32();
    /* Top nibble set: reserved bits that must be ignored, not read as part of
     * the cluster number. */
    fat32_set(T32_DIR_FIRST_CLUSTER, 0xF0000000u | T32_DIR_SECOND_CLUSTER);
    mount_volume32(&mnt, &blk);

    memset(&c, 0, sizeof(c));
    c.cluster = T32_DIR_FIRST_CLUSTER;
    r = fat_chain_next(&c, &blk, &mnt);

    CHECK(r == FAT_R_DONE, "chain hop completed");
    CHECK(c.next == T32_DIR_SECOND_CLUSTER, "successor ignores the reserved high nibble");

    free_volume();
}

/* The FAT32 root directory is an ordinary chain, so a lookup in it must work
 * without the fixed-region machinery FAT12/16 uses. */
static void test_fat32_lookup_in_the_root_chain(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_resolve_ctx_t r;
    fat_r_t rc;

    build_volume32();
    mount_volume32(&mnt, &blk);

    memset(&r, 0, sizeof(r));
    r.path = "/INROOT.TXT";
    r.source = -1;
    rc = fat_resolve_path(&r, &blk, &mnt);

    CHECK(rc == FAT_R_DONE, "resolve completed");
    CHECK(r.found.valid == 1, "a name in the FAT32 root chain resolves");
    CHECK(r.found.cluster == 0x0099u, "resolved entry carries its start cluster");

    free_volume();
}

/* A start cluster above 0xFFFF must survive the dirent's split representation
 * (high half at offset 20-21) and every cluster field it passes through. A
 * 16-bit cluster type truncates this to 0x2345 and silently addresses the
 * wrong data. */
static void test_fat32_start_cluster_above_16_bits_round_trips(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_resolve_ctx_t r;
    fat_r_t rc;

    build_volume32();
    mount_volume32(&mnt, &blk);

    memset(&r, 0, sizeof(r));
    r.path = "/SUB/TARGET.TXT";
    r.source = -1;
    rc = fat_resolve_path(&r, &blk, &mnt);

    CHECK(rc == FAT_R_DONE, "resolve completed");
    CHECK(r.found.valid == 1, "entry in the subdirectory's second cluster resolves");
    CHECK(r.found.cluster == T32_TARGET_CLUSTER, "a start cluster above 0xFFFF is not truncated");
    CHECK(r.found.size == T_TARGET_SIZE, "resolved entry carries its size");

    free_volume();
}

/* Listing the FAT32 root exercises the chained-root path in READDIR, which on
 * FAT12/16 is the fixed-region branch. */
static void test_fat32_readdir_lists_the_root_chain(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_op_ctx_t op;
    fat_r_t r;

    build_volume32();
    mount_volume32(&mnt, &blk);

    memset(&op, 0, sizeof(op));
    op.source = 7;
    op.request_id = 1;
    g_stream_len = 0;
    g_stream[0] = '\0';

    r = fat_op_readdir(&op, &blk, &mnt, 9);

    CHECK(r == FAT_R_DONE, "readdir completed");
    CHECK(strstr(g_stream, "INROOT.TXT") != NULL, "FAT32 root listing includes its entries");
    CHECK(strstr(g_stream, "SUB") != NULL, "FAT32 root listing includes its subdirectory");

    free_volume();
}

/* --- FAT32 mutation ------------------------------------------------------
 *
 * These drive the WRITE paths, which the block layer above now lands in the
 * image, so each case reads back what the driver actually stored rather than
 * trusting a return code. Everything here stays inside the root's first
 * cluster: fat_find_free_dir_slots is still single-cluster (see TASKS.md), and
 * a case that needed a second cluster would be testing that gap, not FAT32. */

/* Read a raw FAT32 table entry straight out of the image, reserved nibble and
 * all -- deliberately not through fat_fatent_read, which masks it off. */
static uint32_t fat32_get_raw(uint32_t cluster) {
    uint32_t offset = cluster * 4u;
    const uint8_t* p = image_sector(T32_FAT_LBA + (offset / T32_SECTOR)) + (offset % T32_SECTOR);

    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The top four bits of a FAT32 entry belong to the volume, not to the cluster
 * number being stored. Clearing them on write is a silent corruption of state
 * the driver does not own, which no read-side case can detect. */
static void test_fat32_fatent_write_preserves_reserved_bits(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_fatent_ctx_t e;
    fat_r_t r;

    build_volume32();
    /* Reserved nibble set, value irrelevant. */
    fat32_set(T32_DIR_SECOND_CLUSTER, 0xA0000000u | 0x00000005u);
    mount_volume32(&mnt, &blk);

    memset(&e, 0, sizeof(e));
    e.cluster = T32_DIR_SECOND_CLUSTER;
    e.write_value = 0x0FFFFFFFu; /* end-of-chain */
    r = fat_fatent_write(&e, &blk, &mnt);

    CHECK(r == FAT_R_DONE, "FAT entry written");
    CHECK((fat32_get_raw(T32_DIR_SECOND_CLUSTER) & 0x0FFFFFFFu) == 0x0FFFFFFFu,
          "the 28-bit value is stored");
    CHECK((fat32_get_raw(T32_DIR_SECOND_CLUSTER) & 0xF0000000u) == 0xA0000000u,
          "the reserved high nibble survives the write");

    free_volume();
}

/* A file created in the FAT32 root must be findable afterwards: the create path
 * writes a dirent whose start-cluster field is split, into a root that is a
 * cluster chain rather than a fixed region. */
static void test_fat32_create_file_is_resolvable(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_create_ctx_t c;
    fat_resolve_ctx_t r;
    fat_r_t rc;

    build_volume32();
    mount_volume32(&mnt, &blk);

    memset(&c, 0, sizeof(c));
    c.path = "/NEW.TXT";
    c.source = -1;
    rc = fat_create_empty_file(&c, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "create completed");

    memset(&r, 0, sizeof(r));
    r.path = "/NEW.TXT";
    r.source = -1;
    rc = fat_resolve_path(&r, &blk, &mnt);

    CHECK(rc == FAT_R_DONE, "resolve completed");
    CHECK(r.found.valid == 1, "the created file resolves on a FAT32 volume");
    CHECK(r.found.size == 0, "a new file is empty");

    free_volume();
}

/* A start cluster above 0xFFFF must survive being WRITTEN, not just read: the
 * high half goes to dirent bytes 20..21 and a store that dropped it would round
 * the file's data to a different cluster. */
static void test_fat32_create_stores_a_cluster_above_16_bits(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_create_ctx_t c;
    fat_resolve_ctx_t r;
    fat_r_t rc;

    build_volume32();
    mount_volume32(&mnt, &blk);

    memset(&c, 0, sizeof(c));
    c.path = "/BIG.BIN";
    c.source = -1;
    c.attr = 0x20;
    c.cluster = T32_TARGET_CLUSTER; /* 0x12345 */
    c.size = 4096;
    c.fail_if_exists = 1;
    rc = fat_create_path_entry(&c, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "create completed");

    memset(&r, 0, sizeof(r));
    r.path = "/BIG.BIN";
    r.source = -1;
    rc = fat_resolve_path(&r, &blk, &mnt);

    CHECK(rc == FAT_R_DONE, "resolve completed");
    CHECK(r.found.valid == 1, "the created entry resolves");
    CHECK(r.found.cluster == T32_TARGET_CLUSTER, "a start cluster above 0xFFFF is stored intact");
    CHECK(r.found.size == 4096u, "size is stored");

    free_volume();
}

/* mkdir allocates a cluster, marks it end-of-chain and lays down '.' and '..'.
 * On FAT32 the end-of-chain marker is 28-bit and the dot entries carry split
 * cluster numbers, so this exercises three FAT32 paths at once. */
static void test_fat32_mkdir_initialises_the_new_cluster(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_mkdir_ctx_t m;
    fat_resolve_ctx_t r;
    fat_r_t rc;
    const uint8_t* dir;

    build_volume32();
    mount_volume32(&mnt, &blk);

    memset(&m, 0, sizeof(m));
    m.path = "/NEWDIR";
    m.source = -1;
    rc = fat_create_directory(&m, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "mkdir completed");

    memset(&r, 0, sizeof(r));
    r.path = "/NEWDIR";
    r.source = -1;
    rc = fat_resolve_path(&r, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "resolve completed");
    CHECK(r.found.valid == 1, "the new directory resolves");
    CHECK((r.found.attr & 0x10) != 0, "it is marked a directory");
    CHECK(r.found.cluster >= 2u, "it owns a real cluster");

    if (r.found.valid && r.found.cluster >= 2u) {
        CHECK((fat32_get_raw(r.found.cluster) & 0x0FFFFFFFu) == 0x0FFFFFFFu,
              "its cluster is marked end-of-chain with the 28-bit FAT32 marker");
        dir = cluster32_sector(r.found.cluster);
        CHECK(dir[0] == '.' && dir[1] == ' ', "'.' is the first entry");
        CHECK(dir[32] == '.' && dir[33] == '.', "'..' is the second entry");
        CHECK(fat_dirent_cluster(dir) == r.found.cluster, "'.' points at the directory itself");
        /* The specification requires 0 here when the parent is the root, even on
         * FAT32 where the root has a real cluster number. Other implementations
         * test '..' against 0, so writing BPB_RootClus would be readable by us
         * and wrong to everyone else. */
        CHECK(fat_dirent_cluster(dir + 32) == 0u, "'..' is 0 because the parent is the root");
    }

    free_volume();
}

/* rmdir must see the new directory as empty and then free its cluster: the
 * emptiness walk, the entry tombstone and fat_free_cluster_chain all run
 * against FAT32 here. */
static void test_fat32_rmdir_frees_the_cluster(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_mkdir_ctx_t m;
    fat_remove_ctx_t rm;
    fat_resolve_ctx_t r;
    fat_r_t rc;
    uint32_t dir_cluster;

    build_volume32();
    mount_volume32(&mnt, &blk);

    memset(&m, 0, sizeof(m));
    m.path = "/GONE";
    m.source = -1;
    rc = fat_create_directory(&m, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "mkdir completed");
    dir_cluster = m.cluster;

    memset(&rm, 0, sizeof(rm));
    rm.path = "/GONE";
    rm.source = -1;
    rm.is_rmdir = 1;
    rc = fat_remove_path(&rm, &blk, &mnt, NULL, 0);
    CHECK(rc == FAT_R_DONE, "rmdir completed");

    memset(&r, 0, sizeof(r));
    r.path = "/GONE";
    r.source = -1;
    rc = fat_resolve_path(&r, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "resolve completed");
    CHECK(r.found.valid == 0, "the removed directory no longer resolves");
    if (dir_cluster >= 2u) {
        CHECK((fat32_get_raw(dir_cluster) & 0x0FFFFFFFu) == 0u,
              "its cluster is back on the free list");
    }

    free_volume();
}

/* Growing a file links its chain through fat_fatent_write twice: the new
 * cluster gets the 28-bit end-of-chain marker, and the previous last cluster is
 * repointed at it. On FAT32 both writes go through the 4-byte path, and the
 * first-cluster write-back for a file that had none goes through the split
 * dirent field. This is the chain half of a file round-trip; the data half
 * needs the client transfer-buffer path, which this harness does not model. */
static void test_fat32_append_cluster_links_the_chain(void) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_create_ctx_t c;
    fat_resolve_ctx_t r;
    fat_open_file_t file;
    fat_append_ctx_t ap;
    fat_r_t rc;
    uint32_t first;
    uint32_t second;

    build_volume32();
    mount_volume32(&mnt, &blk);

    memset(&c, 0, sizeof(c));
    c.path = "/GROW.BIN";
    c.source = -1;
    rc = fat_create_empty_file(&c, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "create completed");

    memset(&r, 0, sizeof(r));
    r.path = "/GROW.BIN";
    r.source = -1;
    rc = fat_resolve_path(&r, &blk, &mnt);
    CHECK(rc == FAT_R_DONE && r.found.valid, "the new file resolves");

    memset(&file, 0, sizeof(file));
    file.in_use = 1;
    file.owner = -1;
    file.first_cluster = 0; /* empty: the first append must write the dirent back */
    file.dir_lba = r.found.dir_lba;
    file.dir_sector = r.found.dir_sector;
    file.dir_index = r.found.dir_index;

    memset(&ap, 0, sizeof(ap));
    ap.file = &file;
    rc = fat_append_cluster_to_file(&ap, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "first append completed");
    first = ap.new_cluster;
    CHECK(first >= 2u, "a real cluster was allocated");
    CHECK((fat32_get_raw(first) & 0x0FFFFFFFu) == 0x0FFFFFFFu, "the new cluster is end-of-chain");

    /* The append must have written the start cluster back into the dirent, or
     * the file's data would be unreachable after a reopen. */
    memset(&r, 0, sizeof(r));
    r.path = "/GROW.BIN";
    r.source = -1;
    rc = fat_resolve_path(&r, &blk, &mnt);
    CHECK(rc == FAT_R_DONE && r.found.valid, "the file still resolves");
    CHECK(r.found.cluster == first, "the first cluster is recorded in the directory entry");

    /* A second append links the previous last cluster to the new one. */
    file.first_cluster = first;
    memset(&ap, 0, sizeof(ap));
    ap.file = &file;
    rc = fat_append_cluster_to_file(&ap, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "second append completed");
    second = ap.new_cluster;
    CHECK(second >= 2u && second != first, "a distinct cluster was allocated");
    CHECK((fat32_get_raw(first) & 0x0FFFFFFFu) == second, "the chain is linked first -> second");
    CHECK((fat32_get_raw(second) & 0x0FFFFFFFu) == 0x0FFFFFFFu, "the tail is end-of-chain");

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
    WASMOS_TEST_CASE(test_fat32_mount_reports_a_chained_root),
    WASMOS_TEST_CASE(test_fat32_chain_next_reads_a_28_bit_entry),
    WASMOS_TEST_CASE(test_fat32_lookup_in_the_root_chain),
    WASMOS_TEST_CASE(test_fat32_start_cluster_above_16_bits_round_trips),
    WASMOS_TEST_CASE(test_fat32_readdir_lists_the_root_chain),
    WASMOS_TEST_CASE(test_fat32_fatent_write_preserves_reserved_bits),
    WASMOS_TEST_CASE(test_fat32_create_file_is_resolvable),
    WASMOS_TEST_CASE(test_fat32_create_stores_a_cluster_above_16_bits),
    WASMOS_TEST_CASE(test_fat32_mkdir_initialises_the_new_cluster),
    WASMOS_TEST_CASE(test_fat32_rmdir_frees_the_cluster),
    WASMOS_TEST_CASE(test_fat32_append_cluster_links_the_chain),
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