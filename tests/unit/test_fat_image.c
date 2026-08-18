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

/* Content the external checker compares against byte for byte; keep these in
 * step with scripts/run_fat_image_test.sh, which greps for the same strings. */
#define HELLO_TEXT "written by the wasmos fs_fat driver\n"
#define MODIFIED_TEXT "MODIFIED by fs_fat\n"
#define INNER_TEXT "nested content\n"
/* Larger than one cluster at the fixture's one-sector clusters, so writing and
 * reading it both walk the chain. */
#define BIG_LEN 1500u

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

/* --- File contents. ---
 *
 * fat_op_write cannot be driven from a 64-bit host: it hands the client buffer
 * to wasmos_xfer_buffer_read as `addr_cast(int32_t, stage)`, and a host stack
 * pointer does not survive truncation to 32 bits (on wasm32, where the driver
 * actually runs, a pointer IS 32 bits). So the bytes are placed here instead --
 * but everything around them is the driver's own machinery:
 * fat_ensure_open_file_capacity allocates and links the clusters,
 * fat_reposition_open_file walks the chain to each offset, and
 * fat_store_open_file_size writes the length back into the directory entry.
 * Only the memcpy into the staged sector is the harness's, which is precisely
 * the part fat_op_write does through the truncated pointer.
 *
 * The point of the exercise is downstream anyway: the image is handed to fsck
 * and to the host, which compare the CONTENT byte for byte. */

/* Populate `file` from a resolved entry, deriving capacity from the chain. */
static int open_resolved(fat_block_t* blk, const fat_mount_t* mnt, const fat_dir_entry_info_t* info,
                         fat_open_file_t* file) {
    fat_chainwalk_ctx_t walk;
    uint32_t cluster_bytes = (uint32_t)mnt->sectors_per_cluster * mnt->bytes_per_sector;

    memset(file, 0, sizeof(*file));
    file->in_use = 1;
    file->owner = -1;
    file->first_cluster = info->cluster;
    file->size = info->size;
    file->dir_lba = info->dir_lba;
    file->dir_sector = info->dir_sector;
    file->dir_index = info->dir_index;

    if (info->cluster < 2) {
        file->capacity = 0;
        return 0;
    }
    memset(&walk, 0, sizeof(walk));
    walk.cluster = info->cluster;
    if (fat_chain_walk(&walk, blk, mnt) != FAT_R_DONE) {
        return -1;
    }
    file->capacity = walk.hops * cluster_bytes;
    return 0;
}

/* Write `len` bytes of `data` at offset 0 of `path`, growing the file through
 * the driver's own allocation path and recording the new size. */
static int write_file_content(fat_block_t* blk, const fat_mount_t* mnt, const char* path,
                              const uint8_t* data, uint32_t len) {
    fat_dir_entry_info_t info;
    fat_open_file_t file;
    fat_ensurecap_ctx_t cap;
    fat_reposition_ctx_t repos;
    fat_storesize_ctx_t ss;
    uint32_t done = 0;

    if (!resolve_ok(blk, mnt, path, &info)) {
        return -1;
    }
    if (open_resolved(blk, mnt, &info, &file) != 0) {
        return -1;
    }

    memset(&cap, 0, sizeof(cap));
    cap.file = &file;
    cap.min_size = len;
    if (fat_ensure_open_file_capacity(&cap, blk, mnt) != FAT_R_DONE) {
        return -1;
    }

    while (done < len) {
        uint32_t sector_offset;
        uint32_t chunk;

        memset(&repos, 0, sizeof(repos));
        repos.file = &file;
        repos.offset = done;
        repos.limit = file.capacity;
        if (fat_reposition_open_file(&repos, blk, mnt) != FAT_R_DONE) {
            return -1;
        }
        sector_offset = done % mnt->bytes_per_sector;
        chunk = mnt->bytes_per_sector - sector_offset;
        if (chunk > len - done) {
            chunk = len - done;
        }
        if (fat_need_sector(blk, file.file_lba + file.current_sector) != FAT_R_DONE) {
            return -1;
        }
        memcpy(fat_block_sector(blk) + sector_offset, data + done, chunk);
        if (fat_block_write(blk, file.file_lba + file.current_sector) != FAT_R_DONE) {
            return -1;
        }
        done += chunk;
    }

    memset(&ss, 0, sizeof(ss));
    ss.file = &file;
    ss.size = len;
    if (fat_store_open_file_size(&ss, blk, mnt) != FAT_R_DONE) {
        return -1;
    }
    /* The first cluster is written back by fat_append_cluster_to_file; re-resolve
     * so a caller checking the entry sees the committed state. */
    return 0;
}

/* Read `len` bytes from offset 0 of `path` back through the chain, so the
 * driver's own view is checked before the external tools see the image. */
static int read_file_content(fat_block_t* blk, const fat_mount_t* mnt, const char* path,
                             uint8_t* out, uint32_t len) {
    fat_dir_entry_info_t info;
    fat_open_file_t file;
    fat_reposition_ctx_t repos;
    uint32_t done = 0;

    if (!resolve_ok(blk, mnt, path, &info)) {
        return -1;
    }
    if (open_resolved(blk, mnt, &info, &file) != 0) {
        return -1;
    }
    while (done < len) {
        uint32_t sector_offset;
        uint32_t chunk;

        memset(&repos, 0, sizeof(repos));
        repos.file = &file;
        repos.offset = done;
        repos.limit = file.capacity;
        if (fat_reposition_open_file(&repos, blk, mnt) != FAT_R_DONE) {
            return -1;
        }
        sector_offset = done % mnt->bytes_per_sector;
        chunk = mnt->bytes_per_sector - sector_offset;
        if (chunk > len - done) {
            chunk = len - done;
        }
        if (fat_need_sector(blk, file.file_lba + file.current_sector) != FAT_R_DONE) {
            return -1;
        }
        memcpy(out + done, fat_block_sector(blk) + sector_offset, chunk);
        done += chunk;
    }
    return 0;
}

int main(int argc, char** argv) {
    fat_mount_t mnt;
    fat_block_t blk;
    fat_dir_entry_info_t info;
    fat_create_ctx_t create;
    fat_mkdir_ctx_t mkdir_ctx;
    fat_remove_ctx_t remove_ctx;
    fat_op_ctx_t op;
    uint8_t readback[256];
    static uint8_t big[BIG_LEN];
    static uint8_t bigback[BIG_LEN];
    uint32_t i;
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

    /* (a) A new, empty file. */
    memset(&create, 0, sizeof(create));
    create.path = "/WROTE.TXT";
    create.source = -1;
    rc = fat_create_empty_file(&create, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "creating a file succeeds");
    CHECK(resolve_ok(&blk, &mnt, "/WROTE.TXT", &info), "the created file resolves back");
    CHECK(info.size == 0u, "a new file starts empty");

    /* (b) A new file WITH CONTENT, small enough to stay inside one sector. */
    memset(&create, 0, sizeof(create));
    create.path = "/HELLO.TXT";
    create.source = -1;
    rc = fat_create_empty_file(&create, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "creating the content file succeeds");
    CHECK(write_file_content(
              &blk, &mnt, "/HELLO.TXT", (const uint8_t*)HELLO_TEXT, (uint32_t)strlen(HELLO_TEXT)) ==
              0,
          "writing content succeeds");
    CHECK(resolve_ok(&blk, &mnt, "/HELLO.TXT", &info), "the content file resolves back");
    CHECK(info.size == (uint32_t)strlen(HELLO_TEXT), "its recorded size is the byte count");
    CHECK(info.cluster >= 2u, "it owns a real first cluster");
    memset(readback, 0, sizeof(readback));
    CHECK(read_file_content(&blk, &mnt, "/HELLO.TXT", readback, (uint32_t)strlen(HELLO_TEXT)) == 0,
          "reading the content back succeeds");
    CHECK(memcmp(readback, HELLO_TEXT, strlen(HELLO_TEXT)) == 0,
          "the driver reads back exactly what it wrote");

    /* (c) A file spanning several clusters, so the chain is walked on write and
     *     on read rather than everything landing in one. */
    memset(&create, 0, sizeof(create));
    create.path = "/BIG.BIN";
    create.source = -1;
    rc = fat_create_empty_file(&create, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "creating the multi-cluster file succeeds");
    for (i = 0; i < BIG_LEN; ++i) {
        big[i] = (uint8_t)(i * 7u + 3u); /* position-dependent: a shifted copy fails */
    }
    CHECK(write_file_content(&blk, &mnt, "/BIG.BIN", big, BIG_LEN) == 0,
          "writing a multi-cluster file succeeds");
    CHECK(resolve_ok(&blk, &mnt, "/BIG.BIN", &info), "the multi-cluster file resolves back");
    CHECK(info.size == BIG_LEN, "its size spans more than one cluster");
    memset(bigback, 0, sizeof(bigback));
    CHECK(read_file_content(&blk, &mnt, "/BIG.BIN", bigback, BIG_LEN) == 0,
          "reading the multi-cluster file back succeeds");
    CHECK(memcmp(bigback, big, BIG_LEN) == 0, "every byte survives the chain walk");

    /* (d) OVERWRITE an existing file the formatter wrote, shrinking it. */
    CHECK(resolve_ok(&blk, &mnt, "/README.TXT", &info), "the formatter's file is there to modify");
    CHECK(write_file_content(&blk,
                             &mnt,
                             "/README.TXT",
                             (const uint8_t*)MODIFIED_TEXT,
                             (uint32_t)strlen(MODIFIED_TEXT)) == 0,
          "overwriting an existing file succeeds");
    CHECK(resolve_ok(&blk, &mnt, "/README.TXT", &info), "it still resolves after modification");
    CHECK(info.size == (uint32_t)strlen(MODIFIED_TEXT), "its size is updated to the new content");
    memset(readback, 0, sizeof(readback));
    CHECK(read_file_content(&blk, &mnt, "/README.TXT", readback, (uint32_t)strlen(MODIFIED_TEXT)) ==
              0,
          "reading the modified file back succeeds");
    CHECK(memcmp(readback, MODIFIED_TEXT, strlen(MODIFIED_TEXT)) == 0,
          "the modified content is what was written");

    /* (e) A directory, and a file inside it. */
    memset(&mkdir_ctx, 0, sizeof(mkdir_ctx));
    mkdir_ctx.path = "/MADEDIR";
    mkdir_ctx.source = -1;
    rc = fat_create_directory(&mkdir_ctx, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "mkdir succeeds");
    CHECK(resolve_ok(&blk, &mnt, "/MADEDIR", &info), "the created directory resolves back");
    CHECK((info.attr & 0x10) != 0, "it is marked a directory");

    memset(&create, 0, sizeof(create));
    create.path = "/MADEDIR/INNER.TXT";
    create.source = -1;
    rc = fat_create_empty_file(&create, &blk, &mnt);
    CHECK(rc == FAT_R_DONE, "creating a file inside it succeeds");
    CHECK(write_file_content(&blk,
                             &mnt,
                             "/MADEDIR/INNER.TXT",
                             (const uint8_t*)INNER_TEXT,
                             (uint32_t)strlen(INNER_TEXT)) == 0,
          "writing content inside the new directory succeeds");
    CHECK(resolve_ok(&blk, &mnt, "/MADEDIR/INNER.TXT", &info), "the nested file resolves back");
    CHECK(info.size == (uint32_t)strlen(INNER_TEXT), "the nested file has its content size");

    /* (f) DELETE one of the formatter's files. */
    memset(&remove_ctx, 0, sizeof(remove_ctx));
    remove_ctx.path = "/SIZED.BIN";
    remove_ctx.source = -1;
    remove_ctx.is_rmdir = 0;
    rc = fat_remove_path(&remove_ctx, &blk, &mnt, NULL, 0);
    CHECK(rc == FAT_R_DONE, "unlinking one of the formatter's files succeeds");
    CHECK(!resolve_ok(&blk, &mnt, "/SIZED.BIN", &info), "the unlinked file no longer resolves");
    CHECK(resolve_ok(&blk, &mnt, "/SUBDIR/CHILD.TXT", &info), "an unrelated file survives it");

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
