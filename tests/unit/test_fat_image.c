/* test_fat_image.c — drive fs_fat against a FAT image built by the PLATFORM's
 * formatter, then hand the modified image back for external checking.
 *
 * Every other FAT test in this tree writes its own BPB, so it can only show
 * that the driver agrees with this repository's reading of the specification.
 * This one is fed mkfs.vfat / newfs_msdos output
 * (tests/unit/fixtures/make_fat_images.sh) and its mutations are checked
 * afterwards by fsck.vfat / fsck_msdos and by mounting the result on the host
 * (scripts/run_fat_image_test.sh). A disagreement between us and a real
 * implementation therefore surfaces as a failure instead of as agreement with
 * ourselves.
 *
 * The image is read into memory, driven synchronously, and written back — so
 * the file this exits with is the one the driver produced, byte for byte.
 *
 * Usage: test_fat_image <image> <fat16|fat32> [lfn]
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fat_alloc.h"
#include "fat_block.h"
#include "fat_dir.h"
#include "fat_file.h"
#include "fat_geom.h"
#include "fat_types.h"

#define WASMOS_WASM_IMPORT(module, name)
typedef struct wasmos_physmem_stats wasmos_physmem_stats_t;
typedef struct wasmos_framebuffer_info wasmos_framebuffer_info_t;
typedef struct wasmos_msi_desc wasmos_msi_desc_t;
#include "wasmos_imports.h"

/* --- The image, in memory. --- */

static uint8_t* g_image;
static uint32_t g_image_sectors;
static uint32_t g_sector_bytes = 512u;

static uint8_t* image_sector(uint32_t lba) {
    assert(lba < g_image_sectors);
    return g_image + ((size_t)lba * g_sector_bytes);
}

uint8_t* fat_block_sector(fat_block_t* blk) {
    return blk->sector;
}

fat_r_t fat_need_sector(fat_block_t* blk, uint32_t lba) {
    if (lba >= g_image_sectors) {
        return FAT_R_ERR;
    }
    memcpy(blk->sector, image_sector(lba), g_sector_bytes);
    blk->loaded_lba = lba;
    return FAT_R_DONE;
}

fat_r_t fat_block_write(fat_block_t* blk, uint32_t lba) {
    if (lba >= g_image_sectors) {
        return FAT_R_ERR;
    }
    memcpy(image_sector(lba), blk->sector, g_sector_bytes);
    blk->loaded_lba = lba;
    return FAT_R_DONE;
}

void fat_block_invalidate(fat_block_t* blk) {
    blk->loaded_lba = FAT_BLOCK_NO_LBA;
}

void fat_block_set_err(fat_block_t* blk, int32_t err) {
    (void)blk;
    (void)err;
}

/* The client data path, which this harness does not model.  Aborting rather
 * than returning a plausible value keeps a stray case from asserting against a
 * fiction. */
uint32_t fat_block_direct_sectors(const fat_block_t* blk) {
    (void)blk;
    assert(0 && "fat_block_direct_sectors: not driven here");
    return 0;
}

int32_t fat_block_server_endpoint(const fat_block_t* blk) {
    (void)blk;
    assert(0 && "fat_block_server_endpoint: not driven here");
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
    assert(0 && "fat_block_read_direct: not driven here");
    return FAT_R_ERR;
}

int32_t wasmos_xfer_buffer_read(int32_t buffer_id, int32_t dst, int32_t len, int32_t offset) {
    (void)buffer_id;
    (void)dst;
    (void)len;
    (void)offset;
    assert(0 && "wasmos_xfer_buffer_read: not driven here");
    return -1;
}

int32_t wasmos_xfer_buffer_write(int32_t buffer_id, int32_t src, int32_t len, int32_t offset) {
    (void)buffer_id;
    (void)src;
    (void)len;
    (void)offset;
    assert(0 && "wasmos_xfer_buffer_write: not driven here");
    return -1;
}

int32_t wasmos_xfer_buffer_reborrow(int32_t grantee, int32_t borrow_id, int32_t flags) {
    (void)grantee;
    (void)borrow_id;
    (void)flags;
    assert(0 && "wasmos_xfer_buffer_reborrow: not driven here");
    return -1;
}

int32_t wasmos_xfer_buffer_size(void) {
    assert(0 && "wasmos_xfer_buffer_size: not driven here");
    return -1;
}

int32_t wasmos_console_write(int32_t ptr, int32_t len) {
    (void)ptr;
    (void)len;
    return 0;
}

/* READDIR streams one byte per argument word; collected so a case can assert on
 * the listing the client would have received. */
#define STREAM_CAP 8192u
static char g_stream[STREAM_CAP];
static uint32_t g_stream_len;

static void stream_put(int32_t byte) {
    if (byte != 0 && g_stream_len + 1u < STREAM_CAP) {
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

/* --- Harness. --- */

static int g_failures;
static const char* g_image_path;

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL %s: %s\n", g_image_path, (what));                                       \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static int load_image(const char* path) {
    FILE* f = fopen(path, "rb");
    long len;

    if (!f) {
        printf("  FAIL cannot open %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        printf("  FAIL empty image %s\n", path);
        return -1;
    }
    g_image = malloc((size_t)len);
    assert(g_image != NULL);
    if (fread(g_image, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        printf("  FAIL short read on %s\n", path);
        return -1;
    }
    fclose(f);
    g_image_sectors = (uint32_t)((size_t)len / g_sector_bytes);
    return 0;
}

static int store_image(const char* path) {
    FILE* f = fopen(path, "r+b");
    size_t bytes = (size_t)g_image_sectors * g_sector_bytes;

    if (!f) {
        printf("  FAIL cannot reopen %s for write\n", path);
        return -1;
    }
    if (fwrite(g_image, 1, bytes, f) != bytes) {
        fclose(f);
        printf("  FAIL short write on %s\n", path);
        return -1;
    }
    fclose(f);
    return 0;
}

/* Resolve `path`, reporting the outcome under `what`. */
static int resolve_ok(fat_block_t* blk, const fat_mount_t* mnt, const char* path,
                      fat_dir_entry_info_t* out) {
    fat_resolve_ctx_t r;
    fat_r_t rc;

    memset(&r, 0, sizeof(r));
    r.path = path;
    r.source = -1;
    rc = fat_resolve_path(&r, blk, mnt);
    if (rc != FAT_R_DONE || !r.found.valid) {
        return 0;
    }
    if (out) {
        *out = r.found;
    }
    return 1;
}

int main(int argc, char** argv) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_dir_entry_info_t info;
    fat_create_ctx_t create;
    fat_mkdir_ctx_t mkdir_ctx;
    fat_op_ctx_t op;
    fat_r_t rc;
    const char* want_type;
    int want_lfn;

    if (argc < 3) {
        printf("usage: %s <image> <fat16|fat32> [lfn]\n", argv[0]);
        return 2;
    }
    g_image_path = argv[1];
    want_type = argv[2];
    want_lfn = (argc > 3 && strcmp(argv[3], "lfn") == 0);

    if (load_image(g_image_path) != 0) {
        return 1;
    }

    memset(&blk, 0, sizeof(blk));
    blk.loaded_lba = FAT_BLOCK_NO_LBA;
    fat_mount_init(&mnt);
    rc = fat_geom_mount_step(&mnt, &blk);
    CHECK(rc == FAT_R_DONE, "the formatter's volume mounts");
    if (rc != FAT_R_DONE) {
        return 1;
    }
    CHECK(fat_mount_ready(&mnt), "mount reports ready");

    if (strcmp(want_type, "fat32") == 0) {
        CHECK(mnt.fat_type == FAT_TYPE_32, "detected as FAT32");
        CHECK(mnt.root_cluster >= 2u, "FAT32 root cluster is usable");
    } else {
        CHECK(mnt.fat_type == FAT_TYPE_16, "detected as FAT16");
        CHECK(mnt.root_entry_count > 0u, "FAT16 has a fixed root region");
    }
    /* A real formatter writes two FAT copies; the write path has to keep both
     * in step, which fsck checks after this runs. */
    CHECK(mnt.fat_count >= 1u, "FAT copy count parsed");

    /* --- READ what the formatter wrote. --- */
    CHECK(resolve_ok(&blk, &mnt, "/README.TXT", &info), "/README.TXT resolves");
    CHECK(info.size == 20u, "/README.TXT has the size the host wrote");
    CHECK(resolve_ok(&blk, &mnt, "/SUBDIR/CHILD.TXT", &info), "/SUBDIR/CHILD.TXT resolves");
    CHECK(resolve_ok(&blk, &mnt, "/SIZED.BIN", &info), "/SIZED.BIN resolves");
    CHECK(info.size == 64u, "/SIZED.BIN is 64 bytes");

    /* MANYFILES/ spans several clusters at these cluster sizes, so its last
     * entries are only reachable through the chain-walking scan -- against a
     * real formatter's allocation rather than one this repository chose. */
    CHECK(resolve_ok(&blk, &mnt, "/MANYFILES/F00.TXT", &info), "first entry of a big directory");
    CHECK(resolve_ok(&blk, &mnt, "/MANYFILES/F63.TXT", &info), "LAST entry of a big directory");

    if (want_lfn) {
        CHECK(resolve_ok(&blk, &mnt, "/a-long-file-name.txt", &info), "a long file name resolves");
        CHECK(resolve_ok(&blk, &mnt, "/Mixed Case Name.txt", &info),
              "a long name with spaces and case resolves");
    }

    /* Listing the root must reach the formatter's entries. */
    memset(&op, 0, sizeof(op));
    op.source = 7;
    op.request_id = 1;
    g_stream_len = 0;
    g_stream[0] = '\0';
    rc = fat_op_readdir(&op, &blk, &mnt, 9);
    CHECK(rc == FAT_R_DONE, "readdir of the root completes");
    CHECK(strstr(g_stream, "README.TXT") != NULL, "listing includes README.TXT");
    CHECK(strstr(g_stream, "SUBDIR") != NULL, "listing includes SUBDIR");

    /* --- MODIFY, for the external tools to judge. --- */
    memset(&create, 0, sizeof(create));
    create.path = "/WROTE.TXT";
    create.source = -1;
    rc = fat_create_empty_file(&create, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "creating a file succeeds");
    CHECK(resolve_ok(&blk, &mnt, "/WROTE.TXT", &info), "the created file resolves back");

    memset(&mkdir_ctx, 0, sizeof(mkdir_ctx));
    mkdir_ctx.path = "/MADEDIR";
    mkdir_ctx.source = -1;
    rc = fat_create_directory(&mkdir_ctx, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "mkdir succeeds");
    CHECK(resolve_ok(&blk, &mnt, "/MADEDIR", &info), "the created directory resolves back");
    CHECK((info.attr & 0x10) != 0, "it is marked a directory");

    /* Create inside the directory we just made, so the external check sees a
     * populated subdirectory rather than an empty one. */
    memset(&create, 0, sizeof(create));
    create.path = "/MADEDIR/INNER.TXT";
    create.source = -1;
    rc = fat_create_empty_file(&create, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "creating a file inside it succeeds");
    CHECK(resolve_ok(&blk, &mnt, "/MADEDIR/INNER.TXT", &info), "the nested file resolves back");

    if (store_image(g_image_path) != 0) {
        return 1;
    }

    if (g_failures) {
        printf("test_fat_image %s: %d failure(s)\n", g_image_path, g_failures);
        return 1;
    }
    printf("test_fat_image %s: ok\n", g_image_path);
    return 0;
}
