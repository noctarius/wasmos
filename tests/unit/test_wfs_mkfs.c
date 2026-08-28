/* Host unit test for the WFS formatter (src/tools/mkfs_wfs/).
 *
 * This suite closes the loop the format layer leaves open. test_wfs_format.c
 * assembles superblock images by hand and checks the READER against them; here
 * the formatter writes a whole volume and the reader — the same wfs_super_parse
 * and wfs_checksum_struct the driver will use — is turned on what it produced.
 * A disagreement between the writer and the reader therefore fails, rather than
 * waiting to be found by a driver that cannot mount its own filesystem.
 *
 * Formatting goes to memory, never a file. Two sinks: one that keeps the whole
 * volume, for the 16 MiB cases, and one that keeps only named blocks, so the
 * multi-group case can format 132 MiB without allocating it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_shuffle.h"

#include "wasmos_status.h"
#include "wfs_crc32c.h"
#include "wfs_endian.h"
#include "wfs_format.h"
#include "wfs_mkfs.h"
#include "wfs_super.h"

static int g_failures;
static int g_checks;

static void expect(int cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("[fail] %s\n", what);
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
    0x51, 0x2f, 0x88, 0x0a, 0x77, 0x13, 0x40, 0xc2, 0x9d, 0x64, 0xee, 0x31, 0x05, 0xb8, 0xaa, 0x76};

#define TEST_NOW_NS 1750000000000000000ull

static void base_params(wfs_mkfs_params_t* p, uint64_t size) {
    memset(p, 0, sizeof(*p));
    p->size_bytes = size;
    p->now_ns = TEST_NOW_NS;
    memcpy(p->uuid, k_uuid, WFS_UUID_LEN);
}

/* ---- sinks --------------------------------------------------------------- */

typedef struct {
    uint8_t* image;
    uint32_t blocks;
    uint32_t block_size;
    uint32_t writes;
    uint32_t out_of_order;
    uint32_t last_block;
} full_sink_t;

static int full_write(void* ctx, uint32_t block, const void* data, uint32_t len) {
    full_sink_t* s = (full_sink_t*)ctx;

    if (block >= s->blocks || len != s->block_size) {
        return -1;
    }
    /* The header promises ascending order and exactly one write per block, so a
     * sink may append rather than seek. A formatter that broke that would make
     * every appending consumer silently wrong, so it is checked here. */
    if (s->writes != 0u && block != s->last_block + 1u) {
        s->out_of_order++;
    }
    s->last_block = block;
    s->writes++;
    memcpy(s->image + (size_t)block * len, data, len);
    return 0;
}

#define PROBE_MAX 8

typedef struct {
    uint32_t block_size;
    uint32_t wanted[PROBE_MAX];
    uint8_t* got[PROBE_MAX];
    uint32_t count;
    uint32_t writes;
} probe_sink_t;

static int probe_write(void* ctx, uint32_t block, const void* data, uint32_t len) {
    probe_sink_t* s = (probe_sink_t*)ctx;
    uint32_t i;

    s->writes++;
    for (i = 0; i < s->count; ++i) {
        if (s->wanted[i] == block && !s->got[i]) {
            s->got[i] = (uint8_t*)malloc(len);
            if (!s->got[i]) {
                return -1;
            }
            memcpy(s->got[i], data, len);
        }
    }
    return 0;
}

static uint8_t* probe_block(probe_sink_t* s, uint32_t block) {
    uint32_t i;

    for (i = 0; i < s->count; ++i) {
        if (s->wanted[i] == block) {
            return s->got[i];
        }
    }
    return NULL;
}

/* Format `size` bytes into a freshly allocated image. Returns NULL on failure. */
static uint8_t* format_image(uint64_t size, wfs_mkfs_layout_t* layout, full_sink_t* sink_out) {
    wfs_mkfs_params_t params;
    wfs_mkfs_sink_t sink;
    full_sink_t fs;
    uint8_t* image;

    base_params(&params, size);
    if (wfs_mkfs_plan(&params, layout) != WASMOS_ERR_NONE) {
        return NULL;
    }

    image = (uint8_t*)calloc(layout->total_blocks, layout->block_size);
    if (!image) {
        return NULL;
    }
    memset(&fs, 0, sizeof(fs));
    fs.image = image;
    fs.blocks = layout->total_blocks;
    fs.block_size = layout->block_size;

    sink.ctx = &fs;
    sink.write_block = full_write;
    if (wfs_mkfs_format(&params, &sink, layout) != WASMOS_ERR_NONE) {
        free(image);
        return NULL;
    }
    if (sink_out) {
        *sink_out = fs;
    }
    return image;
}

#define IMG_16M (16ull * 1024ull * 1024ull)

/* ---- the volume the reader sees ------------------------------------------ */

static void test_a_fresh_volume_mounts(void) {
    wfs_mkfs_layout_t L;
    full_sink_t sink;
    uint8_t* image = format_image(IMG_16M, &L, &sink);
    wfs_super_t sb;

    if (!image) {
        expect(0, "format a 16 MiB volume");
        return;
    }

    expect_rc(wfs_super_parse(image + WFS_SUPER_OFFSET, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_NONE,
              "the formatter's own superblock parses");

    expect(sb.block_size == L.block_size, "block size round-trips");
    expect(sb.total_blocks == L.total_blocks, "block count round-trips");
    expect(sb.total_objects == L.total_objects, "object count round-trips");
    expect(sb.group_count == L.group_count, "the reader derives the same group count");
    expect(sb.root_object_id == WFS_OBJECT_ROOT, "root object id");
    expect(sb.state == WFS_STATE_CLEAN, "a fresh volume is clean");
    expect(sb.needs_replay == 0u, "and needs no replay");
    expect(sb.read_only == 0u, "and is writable");
    expect(sb.generation == 1u, "a fresh volume starts at generation 1");
    expect(memcmp(sb.uuid, k_uuid, WFS_UUID_LEN) == 0, "uuid round-trips");

    expect(sb.journal_start == L.journal_start, "journal start round-trips");
    expect(sb.object_table_start == L.object_table_start, "object table start round-trips");
    expect(sb.bitmap_start == L.bitmap_start, "bitmap start round-trips");

    /* Every block written exactly once, in ascending order. */
    expect(sink.writes == L.total_blocks, "every block is written exactly once");
    expect(sink.out_of_order == 0u, "blocks are emitted in ascending order");

    free(image);
}

/* Regions must tile the volume without overlapping: an overlap silently aliases
 * two structures onto one block, which no checksum would catch because each
 * would be written over the other in turn. */
static void test_the_regions_tile_without_overlap(void) {
    wfs_mkfs_layout_t L;
    uint8_t* image = format_image(IMG_16M, &L, NULL);

    if (!image) {
        expect(0, "format");
        return;
    }

    expect(L.group_table_start == 1u, "the group table follows block 0");
    expect(L.journal_start == L.group_table_start + L.group_table_blocks,
           "the journal follows the group table");
    expect(L.object_table_start == L.journal_start + L.journal_blocks,
           "the object table follows the journal");
    expect(L.bitmap_start == L.object_table_start + L.object_table_blocks,
           "the bitmaps follow the object table");
    expect(L.first_data_block == L.bitmap_start + L.bitmap_blocks, "data follows the bitmaps");
    expect(L.first_data_block < L.total_blocks, "the volume has data blocks");
    expect(L.bitmap_blocks == 2u * L.group_count, "two bitmap blocks per group");
    expect(L.object_table_blocks == L.object_table_blocks_per_group * L.group_count,
           "the object table divides evenly among groups");

    free(image);
}

static void test_the_group_descriptors_verify(void) {
    wfs_mkfs_layout_t L;
    uint8_t* image = format_image(IMG_16M, &L, NULL);
    uint32_t g;

    if (!image) {
        expect(0, "format");
        return;
    }

    for (g = 0; g < L.group_count; ++g) {
        const uint8_t* table = image + (size_t)L.group_table_start * L.block_size;
        uint32_t per_block = wfs_group_descs_per_block(L.block_size);
        const uint8_t* d = table + (size_t)(g / per_block) * L.block_size +
                           (size_t)(g % per_block) * WFS_GROUP_DESC_SIZE;
        uint32_t stored = wfs_rd32(d, (uint32_t)offsetof(struct wfs_group_desc, checksum));
        uint32_t computed = wfs_checksum_struct(
            k_uuid, g, d, WFS_GROUP_DESC_SIZE, (uint32_t)offsetof(struct wfs_group_desc, checksum));

        expect(stored == computed, "a group descriptor verifies under its group index");
        expect(wfs_rd64(d, (uint32_t)offsetof(struct wfs_group_desc, block_bitmap)) ==
                   L.bitmap_start + 2u * g,
               "the descriptor names its block bitmap");
        expect(wfs_rd64(d, (uint32_t)offsetof(struct wfs_group_desc, object_bitmap)) ==
                   L.bitmap_start + 2u * g + 1u,
               "the descriptor names its object bitmap");
    }

    free(image);
}

/* A descriptor is a record inside a shared block, so it is seeded with its group
 * index; one moved to another slot must fail (§13). */
static void test_a_descriptor_is_bound_to_its_slot(void) {
    wfs_mkfs_layout_t L;
    uint8_t* image = format_image(IMG_16M, &L, NULL);
    const uint8_t* d;
    uint32_t stored;

    if (!image) {
        expect(0, "format");
        return;
    }
    d = image + (size_t)L.group_table_start * L.block_size;
    stored = wfs_rd32(d, (uint32_t)offsetof(struct wfs_group_desc, checksum));

    expect(stored != wfs_checksum_struct(k_uuid,
                                         1u,
                                         d,
                                         WFS_GROUP_DESC_SIZE,
                                         (uint32_t)offsetof(struct wfs_group_desc, checksum)),
           "group 0's descriptor does not verify as group 1's");

    free(image);
}

static void test_the_root_object_verifies_and_describes_a_directory(void) {
    wfs_mkfs_layout_t L;
    uint8_t* image = format_image(IMG_16M, &L, NULL);
    const uint8_t* obj;
    uint32_t e;

    if (!image) {
        expect(0, "format");
        return;
    }
    /* Object 1 is the second record of the object table's first block. */
    obj = image + (size_t)L.object_table_start * L.block_size + WFS_OBJECT_SIZE;

    expect(wfs_rd32(obj, (uint32_t)offsetof(struct wfs_object, checksum)) ==
               wfs_checksum_struct(k_uuid,
                                   WFS_OBJECT_ROOT,
                                   obj,
                                   WFS_OBJECT_SIZE,
                                   (uint32_t)offsetof(struct wfs_object, checksum)),
           "the root record verifies under its object id");

    expect(wfs_rd64(obj, (uint32_t)offsetof(struct wfs_object, object_id)) == WFS_OBJECT_ROOT,
           "the root record carries its own id");
    expect((obj[offsetof(struct wfs_object, type)] |
            (obj[offsetof(struct wfs_object, type) + 1] << 8)) == WFS_TYPE_DIR,
           "the root is a directory");
    expect(wfs_rd64(obj, (uint32_t)offsetof(struct wfs_object, size)) == L.block_size,
           "the root's size is its one directory block");
    expect(wfs_rd32(obj, (uint32_t)offsetof(struct wfs_object, link_count)) == 2u,
           "a root's link count is 2: its own . and ..");
    expect(wfs_rd32(obj, (uint32_t)offsetof(struct wfs_object, extent_count)) == 1u,
           "the root has one extent");
    expect(wfs_rd64(obj, (uint32_t)offsetof(struct wfs_object, extent_tree_block)) == 0u,
           "an inline extent needs no tree");
    expect(wfs_rd64(obj, (uint32_t)offsetof(struct wfs_object, mtime)) == TEST_NOW_NS,
           "timestamps are the nanoseconds handed to the formatter");

    e = (uint32_t)offsetof(struct wfs_object, extents);
    expect(wfs_rd64(obj, e + (uint32_t)offsetof(struct wfs_extent, logical_block)) == 0u,
           "the extent starts at logical block 0");
    expect(wfs_rd64(obj, e + (uint32_t)offsetof(struct wfs_extent, physical_block)) ==
               L.root_data_block,
           "the extent points at the root's data block");
    expect(wfs_rd32(obj, e + (uint32_t)offsetof(struct wfs_extent, length)) == 1u,
           "the extent is one block long");

    free(image);
}

static void test_the_root_directory_block_is_well_formed(void) {
    wfs_mkfs_layout_t L;
    uint8_t* image = format_image(IMG_16M, &L, NULL);
    const uint8_t* dir;
    const uint8_t* tail;
    uint32_t dot_len;
    uint32_t dotdot_len;

    if (!image) {
        expect(0, "format");
        return;
    }
    dir = image + (size_t)L.root_data_block * L.block_size;

    dot_len = (uint32_t)dir[8] | ((uint32_t)dir[9] << 8);
    expect(wfs_rd64(dir, 0u) == WFS_OBJECT_ROOT, ". names the root");
    expect(dir[10] == 1u && dir[12] == '.', ". is one character");
    expect(dot_len == wfs_dir_record_length(1u), ". strides by an 8-byte multiple");
    expect((dot_len & 7u) == 0u, "every stride is a multiple of 8");

    dotdot_len = (uint32_t)dir[dot_len + 8u] | ((uint32_t)dir[dot_len + 9u] << 8);
    expect(wfs_rd64(dir, dot_len) == WFS_OBJECT_ROOT, ".. of the root names the root");
    expect(dir[dot_len + 10u] == 2u && dir[dot_len + 12u] == '.' && dir[dot_len + 13u] == '.',
           ".. is two characters");

    /* The last record stretches to meet the tail, so a scan ends exactly where
     * the tail begins rather than running into it. */
    expect(dot_len + dotdot_len == wfs_dir_usable_bytes(L.block_size),
           "the records fill the block up to the tail");

    tail = dir + wfs_dir_usable_bytes(L.block_size);
    expect(wfs_rd64(tail, (uint32_t)offsetof(struct wfs_dir_tail, object_id)) == 0u,
           "the tail reads as free space to a scanner that does not know it");
    expect(tail[offsetof(struct wfs_dir_tail, type)] == WFS_DIR_TAIL_TYPE, "the tail is typed");
    expect(wfs_rd32(tail, (uint32_t)offsetof(struct wfs_dir_tail, checksum)) ==
               wfs_checksum_struct(k_uuid,
                                   L.root_data_block,
                                   dir,
                                   L.block_size,
                                   wfs_dir_usable_bytes(L.block_size) +
                                       (uint32_t)offsetof(struct wfs_dir_tail, checksum)),
           "the tail's checksum covers the block under its own block number");

    free(image);
}

static void test_the_journal_starts_empty_and_verifies(void) {
    wfs_mkfs_layout_t L;
    uint8_t* image = format_image(IMG_16M, &L, NULL);
    const uint8_t* js;

    if (!image) {
        expect(0, "format");
        return;
    }
    js = image + (size_t)L.journal_start * L.block_size;

    expect(wfs_rd32(js, (uint32_t)offsetof(struct wfs_journal_super, magic)) == WFS_JOURNAL_MAGIC,
           "the journal superblock carries its magic");
    expect(wfs_rd32(js, (uint32_t)offsetof(struct wfs_journal_super, block_size)) == L.block_size,
           "the journal agrees with the volume's block size");
    expect(wfs_rd32(js, (uint32_t)offsetof(struct wfs_journal_super, blocks)) == L.journal_blocks,
           "the journal knows its own length");
    expect(wfs_rd64(js, (uint32_t)offsetof(struct wfs_journal_super, first_sequence)) == 1u,
           "the log tail starts at sequence 1");
    expect(wfs_rd32(js, (uint32_t)offsetof(struct wfs_journal_super, checksum)) ==
               wfs_checksum_struct(k_uuid,
                                   L.journal_start,
                                   js,
                                   (uint32_t)sizeof(struct wfs_journal_super),
                                   (uint32_t)offsetof(struct wfs_journal_super, checksum)),
           "the journal superblock verifies");

    /* The first log block is zero, so a recovery scan finds its head there and
     * replays nothing. */
    expect(wfs_rd32(image + (size_t)(L.journal_start + 1u) * L.block_size, 0u) != WFS_JOURNAL_MAGIC,
           "the log behind it is empty");

    free(image);
}

/* The bitmaps are authoritative (§12), so what they say must match both the
 * layout and the counters derived from them. */
static void test_the_bitmaps_reserve_every_metadata_block(void) {
    wfs_mkfs_layout_t L;
    uint8_t* image = format_image(IMG_16M, &L, NULL);
    const uint8_t* bm;
    const uint8_t* om;
    uint32_t i;
    uint32_t counted_free = 0u;

    if (!image) {
        expect(0, "format");
        return;
    }
    bm = image + (size_t)L.bitmap_start * L.block_size;
    om = image + (size_t)(L.bitmap_start + 1u) * L.block_size;

    for (i = 0; i <= L.root_data_block; ++i) {
        if (!(bm[i >> 3] & (1u << (i & 7u)))) {
            expect(0, "a metadata block is left free in the bitmap");
            break;
        }
    }
    expect(i > L.root_data_block, "every block up to and including the root's is reserved");

    expect((bm[(L.root_data_block + 1u) >> 3] & (1u << ((L.root_data_block + 1u) & 7u))) == 0u,
           "the block after the root's is free");

    for (i = 0; i < L.total_blocks; ++i) {
        if (!(bm[i >> 3] & (1u << (i & 7u)))) {
            counted_free++;
        }
    }
    expect(counted_free == L.free_blocks, "the free count matches the bitmap it summarises");

    /* Ids 0..15 are reserved, so an allocator can never return one. */
    for (i = 0; i < WFS_OBJECT_FIRST; ++i) {
        if (!(om[i >> 3] & (1u << (i & 7u)))) {
            expect(0, "a reserved object id is left free");
            break;
        }
    }
    expect(i == WFS_OBJECT_FIRST, "every reserved object id is marked allocated");
    expect((om[WFS_OBJECT_FIRST >> 3] & (1u << (WFS_OBJECT_FIRST & 7u))) == 0u,
           "the first allocatable object id is free");
    expect(L.free_objects == L.total_objects - WFS_OBJECT_FIRST, "the free object count");

    free(image);
}

/* Blocks past the end of a partly populated final group must be marked
 * allocated, or the allocator hands out blocks the device does not have. */
static void test_blocks_past_the_volume_are_not_allocatable(void) {
    wfs_mkfs_layout_t L;
    /* 20 MiB is 5120 blocks: well inside one 32768-block group, so most of the
     * group's bitmap describes blocks that do not exist. */
    uint8_t* image = format_image(20ull * 1024ull * 1024ull, &L, NULL);
    const uint8_t* bm;
    uint32_t i;
    int all_set = 1;

    if (!image) {
        expect(0, "format");
        return;
    }
    expect(L.group_count == 1u, "the volume is a single partial group");
    bm = image + (size_t)L.bitmap_start * L.block_size;

    for (i = L.total_blocks; i < L.blocks_per_group; ++i) {
        if (!(bm[i >> 3] & (1u << (i & 7u)))) {
            all_set = 0;
            break;
        }
    }
    expect(all_set, "every bit past the last real block is marked allocated");

    free(image);
}

/* A backup must be sealed for the location a scan will read it at, not for the
 * primary's. Formatted through a sink that keeps only the blocks of interest,
 * so 132 MiB never has to be allocated. */
static void test_a_backup_superblock_parses_where_a_scan_looks(void) {
    wfs_mkfs_params_t params;
    wfs_mkfs_layout_t L;
    wfs_mkfs_sink_t sink;
    probe_sink_t probe;
    wfs_super_t sb;
    uint64_t size = 132ull * 1024ull * 1024ull;
    uint32_t backup_block;
    uint8_t* got;
    uint32_t i;

    base_params(&params, size);
    if (wfs_mkfs_plan(&params, &L) != WASMOS_ERR_NONE) {
        expect(0, "plan a two-group volume");
        return;
    }
    expect(L.group_count >= 2u, "132 MiB spans more than one group");

    backup_block = L.blocks_per_group; /* group 1's first block */

    memset(&probe, 0, sizeof(probe));
    probe.block_size = L.block_size;
    probe.wanted[0] = 0u;
    probe.wanted[1] = backup_block;
    probe.count = 2u;

    sink.ctx = &probe;
    sink.write_block = probe_write;
    expect_rc(wfs_mkfs_format(&params, &sink, &L), WASMOS_ERR_NONE, "format a two-group volume");
    expect(probe.writes == L.total_blocks, "every block of the larger volume is written");

    /* The offset the reader computes must be the block the writer used. */
    expect(wfs_super_backup_offset(L.block_size, 1u) == (uint64_t)backup_block * L.block_size,
           "the reader's backup offset names the writer's backup block");

    got = probe_block(&probe, backup_block);
    if (!got) {
        expect(0, "the backup block was written");
    } else {
        expect_rc(wfs_super_parse(got, WFS_SUPER_SIZE, backup_block, &sb),
                  WASMOS_ERR_NONE,
                  "the backup parses at its own block number");
        expect(sb.total_blocks == L.total_blocks, "the backup describes the same volume");
        /* Sealed for its own location, so it must NOT verify as the primary. */
        expect_rc(wfs_super_parse(got, WFS_SUPER_SIZE, 0u, &sb),
                  WASMOS_ERR_FS_CHECKSUM,
                  "the backup does not verify as the primary");
    }

    for (i = 0; i < probe.count; ++i) {
        free(probe.got[i]);
    }
}

/* Corruption anywhere in the superblock must be caught by the reader on a
 * volume the writer produced, not only on a hand-built image. */
static void test_a_corrupted_volume_fails_to_mount(void) {
    wfs_mkfs_layout_t L;
    uint8_t* image = format_image(IMG_16M, &L, NULL);
    wfs_super_t sb;

    if (!image) {
        expect(0, "format");
        return;
    }
    image[WFS_SUPER_OFFSET + 40u] ^= 0x08u;
    expect_rc(wfs_super_parse(image + WFS_SUPER_OFFSET, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_FS_CHECKSUM,
              "a flipped bit in a formatted volume");

    free(image);
}

/* ---- planning ------------------------------------------------------------ */

static void test_plan_refuses_what_it_cannot_lay_out(void) {
    wfs_mkfs_params_t p;
    wfs_mkfs_layout_t L;

    base_params(&p, IMG_16M);
    p.block_size = 1024u;
    expect_rc(wfs_mkfs_plan(&p, &L), WASMOS_ERR_FS_GEOMETRY, "a block size outside the three");

    base_params(&p, 4096ull * 8ull);
    expect_rc(wfs_mkfs_plan(&p, &L), WASMOS_ERR_FS_NO_SPACE, "a volume of eight blocks");

    /* Metadata alone outstrips a small volume once the journal is sized up. */
    base_params(&p, IMG_16M);
    p.journal_blocks = 100000u;
    expect_rc(wfs_mkfs_plan(&p, &L), WASMOS_ERR_FS_NO_SPACE, "a journal larger than the volume");

    /* The formatter carries uint32_t block numbers for the same reason the
     * driver does, and refuses rather than truncating (§22). */
    base_params(&p, 0x100000000ull * 4096ull);
    expect_rc(wfs_mkfs_plan(&p, &L),
              WASMOS_ERR_FS_VOLUME_TOO_LARGE,
              "a volume past a uint32_t block number");
}

static void test_plan_honours_every_permitted_block_size(void) {
    static const uint32_t sizes[3] = {4096u, 8192u, 16384u};
    uint32_t i;

    for (i = 0; i < 3u; ++i) {
        wfs_mkfs_params_t p;
        wfs_mkfs_layout_t L;

        base_params(&p, 64ull * 1024ull * 1024ull);
        p.block_size = sizes[i];
        expect_rc(wfs_mkfs_plan(&p, &L), WASMOS_ERR_NONE, "plan at a permitted block size");
        expect(L.blocks_per_group == sizes[i] * 8u, "a bitmap block covers exactly one group");
        expect(L.total_blocks == (64ull * 1024ull * 1024ull) / sizes[i], "block count");
        expect(L.object_table_blocks_per_group * wfs_objects_per_block(sizes[i]) ==
                   L.objects_per_group,
               "the object table slice holds exactly the group's objects");
    }
}

/* The same PARAMETERS must produce the same volume, uuid included. The tool
 * generates a random uuid per volume — two volumes sharing one would accept
 * each other's blocks, which is the transplant the checksum seeding exists to
 * catch — so determinism belongs to the caller that pins one, and this is the
 * property a reproducible fixture rests on. */
static void test_formatting_is_reproducible(void) {
    wfs_mkfs_layout_t a;
    wfs_mkfs_layout_t b;
    uint8_t* first = format_image(IMG_16M, &a, NULL);
    uint8_t* second = format_image(IMG_16M, &b, NULL);

    if (!first || !second) {
        expect(0, "format twice");
        free(first);
        free(second);
        return;
    }
    expect(a.total_blocks == b.total_blocks, "same geometry");
    expect(memcmp(first, second, (size_t)a.total_blocks * a.block_size) == 0,
           "the same arguments produce a byte-identical volume");

    free(first);
    free(second);
}

/* ---- runner -------------------------------------------------------------- */

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_a_fresh_volume_mounts),
    WASMOS_TEST_CASE(test_the_regions_tile_without_overlap),
    WASMOS_TEST_CASE(test_the_group_descriptors_verify),
    WASMOS_TEST_CASE(test_a_descriptor_is_bound_to_its_slot),
    WASMOS_TEST_CASE(test_the_root_object_verifies_and_describes_a_directory),
    WASMOS_TEST_CASE(test_the_root_directory_block_is_well_formed),
    WASMOS_TEST_CASE(test_the_journal_starts_empty_and_verifies),
    WASMOS_TEST_CASE(test_the_bitmaps_reserve_every_metadata_block),
    WASMOS_TEST_CASE(test_blocks_past_the_volume_are_not_allocatable),
    WASMOS_TEST_CASE(test_a_backup_superblock_parses_where_a_scan_looks),
    WASMOS_TEST_CASE(test_a_corrupted_volume_fails_to_mount),
    WASMOS_TEST_CASE(test_plan_refuses_what_it_cannot_lay_out),
    WASMOS_TEST_CASE(test_plan_honours_every_permitted_block_size),
    WASMOS_TEST_CASE(test_formatting_is_reproducible),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_mkfs: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_mkfs: %d checks passed\n", g_checks);
    return 0;
}
