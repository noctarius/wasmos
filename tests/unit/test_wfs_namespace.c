/* Host unit test for the WFS namespace operations (wfs_namespace.h, §10):
 * create, mkdir, unlink, rmdir and rename, against a volume mkfs_wfs built.
 *
 * These are testable at all because the ops are plain sequences over
 * wfs_ops_run rather than coroutine state machines: the pump needs only the bound
 * runtime and block client, both of which this harness binds.
 *
 * Results are checked by RESOLVING the path afterwards, the way any other client
 * reaches it -- an entry only its writer can find is the failure §10's stride
 * rules exist to prevent -- and freed space is checked against the bitmap, which
 * is the authority the counters derive from.
 */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "stubs_wfs_block_server.h"
#include "wasmos_status.h"
#include "wfs_bitmap.h"
#include "wfs_format.h"
#include "wfs_mount.h"
#include "wfs_namespace.h"
#include "wfs_path.h"
#include "wfs_alloc.h"
#include "wfs_crc32c.h"
#include "wfs_dirent.h"
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
#define NOW_NS 1750000000000000000ull
#define OP_NS 1780000000000000000ull
#define VOL_16M (16ull * 1024ull * 1024ull)

static wfs_mkfs_layout_t g_layout;
static wfs_volume_t g_vol;
static wfs_path_ctx_t g_path;

static int read_zeros(void* c, uint64_t off, void* dst, uint32_t len) {
    (void)c;
    (void)off;
    memset(dst, 0x5Au, len);
    return 0;
}

/* Build the same shape the guest volume has: two directories, a multi-block file
 * and an inline one. Removing objects MKFS created is worth covering separately
 * from removing ones these ops created, because their records and extents were
 * written by different code.
 *
 * BIG_BYTES spans blocks, which is what makes the block-release assertion mean
 * something. */
#define BIG_BYTES 9000u

static int setup(void) {
    static wfs_mkfs_entry_t entries[5];
    static wfs_mkfs_node_t plan[5];
    wfs_mkfs_params_t params;
    wfs_mkfs_sink_t sink;
    wfs_mount_ctx_t m;
    wasmos_wasm_coroutine_t task;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, NOW_NS, &g_layout) != 0) {
        expect(0, "build a volume");
        return -1;
    }
    memset(entries, 0, sizeof(entries));
    memset(plan, 0, sizeof(plan));
    /* Parent before child, which wfs_mkfs_format_tree requires. */
    entries[0].name = "docs";
    entries[0].name_len = 4u;
    entries[0].parent = WFS_MKFS_ROOT;
    entries[0].is_dir = 1u;
    entries[0].mode = 0755u;
    entries[1].name = "etc";
    entries[1].name_len = 3u;
    entries[1].parent = WFS_MKFS_ROOT;
    entries[1].is_dir = 1u;
    entries[1].mode = 0755u;
    entries[2].name = "hello.txt";
    entries[2].name_len = 9u;
    entries[2].parent = WFS_MKFS_ROOT;
    entries[2].mode = 0644u;
    entries[2].size = 26u;
    entries[2].read = read_zeros;
    entries[3].name = "big.txt";
    entries[3].name_len = 7u;
    entries[3].parent = 0u; /* docs */
    entries[3].mode = 0644u;
    entries[3].size = BIG_BYTES;
    entries[3].read = read_zeros;
    entries[4].name = "README";
    entries[4].name_len = 6u;
    entries[4].parent = 0u; /* docs */
    entries[4].mode = 0644u;
    entries[4].size = 12u;
    entries[4].read = read_zeros;

    memset(&params, 0, sizeof(params));
    params.size_bytes = VOL_16M;
    params.block_size = 4096u;
    params.now_ns = NOW_NS;
    memcpy(params.uuid, k_uuid, WFS_UUID_LEN);
    sink.ctx = 0;
    sink.write_block = wfs_stub_sink_write;
    if (wfs_mkfs_format_tree(&params, entries, 5u, plan, &sink, &g_layout) != WASMOS_ERR_NONE) {
        expect(0, "format a populated volume");
        return -1;
    }

    memset(&m, 0, sizeof(m));
    memset(&g_vol, 0, sizeof(g_vol));
    m.vol = &g_vol;
    if (wfs_stub_run_task(&task, wfs_mount_task, &m) != 0) {
        expect(0, "mount the volume");
        return -1;
    }
    return 0;
}

/* Resolve a path the way a client does. Returns the object id, or 0 when absent. */
static uint32_t resolve(const char* path) {
    wasmos_wasm_coroutine_t task;

    if (wfs_path_init_from(&g_path, &g_vol, WFS_OBJECT_ROOT, path, (uint32_t)strlen(path)) !=
        WASMOS_ERR_NONE) {
        return 0u;
    }
    if (wfs_stub_run_task(&task, wfs_path_task, &g_path) != 0) {
        return 0u;
    }
    return g_path.found ? g_path.object_id : 0u;
}

static uint32_t root_links(void) {
    wasmos_wasm_coroutine_t task;
    wfs_object_ctx_t o;

    memset(&o, 0, sizeof(o));
    o.vol = &g_vol;
    o.object_id = WFS_OBJECT_ROOT;
    if (wfs_stub_run_task(&task, wfs_object_task, &o) != 0) {
        return 0u;
    }
    return o.out.link_count;
}

/* ---- create ------------------------------------------------------------- */

static void test_a_created_file_resolves(void) {
    uint32_t id = 0u;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect_rc(
        wfs_ns_create(&g_vol, WFS_OBJECT_ROOT, "made.txt", 8u, WFS_TYPE_FILE, 0644u, OP_NS, &id),
        WASMOS_ERR_NONE,
        "the create succeeds");
    expect(id >= WFS_OBJECT_FIRST, "it got a real object id");
    /* The assertion that matters: a normal path resolution finds it. */
    expect_u32(resolve("/made.txt"), id, "and the path resolves to it");
    expect_u32(g_path.object.out.type, (uint32_t)WFS_TYPE_FILE, "as a file");
    expect_u32((uint32_t)g_path.object.out.size, 0u, "of zero size");
    expect(g_path.object.out.flags & WFS_OBJ_INLINE_DATA, "stored inline to begin with");

    wfs_stub_teardown();
}

static void test_a_created_directory_has_dot_and_dotdot(void) {
    uint32_t id = 0u;
    uint32_t before = 0u;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    before = root_links();
    expect_rc(wfs_ns_create(&g_vol, WFS_OBJECT_ROOT, "sub", 3u, WFS_TYPE_DIR, 0755u, OP_NS, &id),
              WASMOS_ERR_NONE,
              "the mkdir succeeds");
    expect_u32(resolve("/sub"), id, "the directory resolves");
    expect_u32(g_path.object.out.type, (uint32_t)WFS_TYPE_DIR, "as a directory");

    /* `.` and `..` have to be REACHABLE, not merely written: they are how a
     * client walks into and back out of the new directory. */
    expect_u32(resolve("/sub/."), id, "its `.` resolves to itself");
    expect_u32(resolve("/sub/.."), WFS_OBJECT_ROOT, "and its `..` to the parent");
    expect_u32(root_links(), before + 1u, "the parent gained a link for the new `..`");

    wfs_stub_teardown();
}

/* A file created inside a new directory proves the directory is usable, not just
 * present: it exercises the block mkdir laid out. */
static void test_a_file_can_be_created_inside_a_new_directory(void) {
    uint32_t dir_id = 0u;
    uint32_t file_id = 0u;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect_rc(wfs_ns_create(&g_vol, WFS_OBJECT_ROOT, "d", 1u, WFS_TYPE_DIR, 0755u, OP_NS, &dir_id),
              WASMOS_ERR_NONE,
              "mkdir /d");
    expect_rc(wfs_ns_create(
                  &g_vol, WFS_OBJECT_ROOT, "/d/inner", 8u, WFS_TYPE_FILE, 0644u, OP_NS, &file_id),
              WASMOS_ERR_NONE,
              "create /d/inner");
    expect_u32(resolve("/d/inner"), file_id, "the nested path resolves");

    wfs_stub_teardown();
}

static void test_create_refusals(void) {
    uint32_t id = 0u;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect_rc(wfs_ns_create(&g_vol, WFS_OBJECT_ROOT, "dup", 3u, WFS_TYPE_FILE, 0644u, OP_NS, &id),
              WASMOS_ERR_NONE,
              "the first create succeeds");
    expect_rc(wfs_ns_create(&g_vol, WFS_OBJECT_ROOT, "dup", 3u, WFS_TYPE_FILE, 0644u, OP_NS, &id),
              WASMOS_ERR_FS_EXISTS,
              "a duplicate name is refused");
    /* mkfs put hello.txt in the root; creating INSIDE a file is not a thing. */
    expect_rc(wfs_ns_create(
                  &g_vol, WFS_OBJECT_ROOT, "/dup/deeper", 11u, WFS_TYPE_FILE, 0644u, OP_NS, &id),
              WASMOS_ERR_FS_NOT_DIR,
              "a file cannot be a parent");
    expect_rc(wfs_ns_create(&g_vol, WFS_OBJECT_ROOT, "/", 1u, WFS_TYPE_FILE, 0644u, OP_NS, &id),
              WASMOS_ERR_FS_NAME,
              "the root names nothing to create");
    expect_rc(
        wfs_ns_create(&g_vol, WFS_OBJECT_ROOT, "/absent/x", 9u, WFS_TYPE_FILE, 0644u, OP_NS, &id),
        WASMOS_ERR_FS_NOT_FOUND,
        "a missing parent is not found");

    g_vol.super.read_only = 1u;
    expect_rc(wfs_ns_create(&g_vol, WFS_OBJECT_ROOT, "ro", 2u, WFS_TYPE_FILE, 0644u, OP_NS, &id),
              WASMOS_ERR_FS_READ_ONLY,
              "a read-only volume refuses to create");

    wfs_stub_teardown();
}

/* ---- unlink ------------------------------------------------------------- */

static void test_an_unlinked_file_stops_resolving(void) {
    uint32_t id = 0u;
    uint32_t free_before;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect_rc(wfs_ns_create(&g_vol, WFS_OBJECT_ROOT, "gone", 4u, WFS_TYPE_FILE, 0644u, OP_NS, &id),
              WASMOS_ERR_NONE,
              "create gone");
    expect_u32(resolve("/gone"), id, "it resolves before the unlink");
    free_before = g_vol.super.free_objects;

    expect_rc(wfs_ns_unlink(&g_vol, WFS_OBJECT_ROOT, "gone", 4u, OP_NS),
              WASMOS_ERR_NONE,
              "the unlink succeeds");
    expect_u32(resolve("/gone"), 0u, "and it no longer resolves");
    expect_u32(g_vol.super.free_objects, free_before + 1u, "its record came back");

    /* The rest of the directory survives: mkfs put these there. */
    expect(resolve("/hello.txt") != 0u, "a sibling still resolves");
    expect(resolve("/docs") != 0u, "and so does a sibling directory");

    wfs_stub_teardown();
}

/* Unlinking a file that holds blocks must release them, not just its record. */
static void test_unlinking_releases_the_data_blocks(void) {
    uint32_t id;
    uint32_t free_before;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    /* docs/big.txt spans blocks, which is what makes this worth checking. */
    id = resolve("/docs/big.txt");
    expect(id != 0u, "the multi-block file resolves");
    free_before = g_vol.super.free_blocks;

    expect_rc(wfs_ns_unlink(&g_vol, WFS_OBJECT_ROOT, "/docs/big.txt", 13u, OP_NS),
              WASMOS_ERR_NONE,
              "the unlink succeeds");
    expect_u32(resolve("/docs/big.txt"), 0u, "it no longer resolves");
    expect(g_vol.super.free_blocks > free_before, "and its blocks came back");

    wfs_stub_teardown();
}

static void test_unlink_refusals(void) {
    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect_rc(wfs_ns_unlink(&g_vol, WFS_OBJECT_ROOT, "absent", 6u, OP_NS),
              WASMOS_ERR_FS_NOT_FOUND,
              "unlinking an absent name is not found");
    /* A directory needs rmdir, whose precondition unlink does not check. */
    expect_rc(wfs_ns_unlink(&g_vol, WFS_OBJECT_ROOT, "docs", 4u, OP_NS),
              WASMOS_ERR_FS_IS_DIR,
              "unlinking a directory is refused");
    expect(resolve("/docs") != 0u, "and the directory survives");

    wfs_stub_teardown();
}

/* ---- rmdir -------------------------------------------------------------- */

static void test_an_empty_directory_can_be_removed(void) {
    uint32_t id = 0u;
    uint32_t before;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect_rc(wfs_ns_create(&g_vol, WFS_OBJECT_ROOT, "tmp", 3u, WFS_TYPE_DIR, 0755u, OP_NS, &id),
              WASMOS_ERR_NONE,
              "mkdir tmp");
    before = root_links();

    expect_rc(wfs_ns_rmdir(&g_vol, WFS_OBJECT_ROOT, "tmp", 3u, OP_NS),
              WASMOS_ERR_NONE,
              "the rmdir succeeds");
    expect_u32(resolve("/tmp"), 0u, "it no longer resolves");
    expect_u32(root_links(), before - 1u, "and the parent lost the `..` link");

    wfs_stub_teardown();
}

/* A directory holding an entry must not be removed: the entry would become
 * unreachable while its object stayed allocated. */
static void test_a_populated_directory_is_not_removed(void) {
    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect_rc(wfs_ns_rmdir(&g_vol, WFS_OBJECT_ROOT, "docs", 4u, OP_NS),
              WASMOS_ERR_FS_NOT_EMPTY,
              "a populated directory is refused");
    expect(resolve("/docs") != 0u, "and survives");
    expect(resolve("/docs/big.txt") != 0u, "with its contents");

    /* Emptied, it goes. `.` and `..` do not count as contents. */
    expect_rc(wfs_ns_unlink(&g_vol, WFS_OBJECT_ROOT, "/docs/big.txt", 13u, OP_NS),
              WASMOS_ERR_NONE,
              "unlink one child");
    expect_rc(wfs_ns_unlink(&g_vol, WFS_OBJECT_ROOT, "/docs/README", 12u, OP_NS),
              WASMOS_ERR_NONE,
              "unlink the other");
    expect_rc(wfs_ns_rmdir(&g_vol, WFS_OBJECT_ROOT, "docs", 4u, OP_NS),
              WASMOS_ERR_NONE,
              "now the rmdir succeeds");
    expect_u32(resolve("/docs"), 0u, "and it is gone");

    wfs_stub_teardown();
}

static void test_rmdir_refuses_a_file(void) {
    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect_rc(wfs_ns_rmdir(&g_vol, WFS_OBJECT_ROOT, "hello.txt", 9u, OP_NS),
              WASMOS_ERR_FS_NOT_DIR,
              "rmdir on a file is refused");
    expect(resolve("/hello.txt") != 0u, "and the file survives");

    wfs_stub_teardown();
}

/* ---- rename ------------------------------------------------------------- */

static void test_a_rename_moves_the_name_not_the_object(void) {
    uint32_t id;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    id = resolve("/hello.txt");
    expect(id != 0u, "the file resolves before the rename");

    expect_rc(wfs_ns_rename(&g_vol, WFS_OBJECT_ROOT, "hello.txt", 9u, "renamed.txt", 11u, OP_NS),
              WASMOS_ERR_NONE,
              "the rename succeeds");
    expect_u32(resolve("/hello.txt"), 0u, "the old name is gone");
    /* The SAME object id: a rename moves a name, it does not copy an object. */
    expect_u32(resolve("/renamed.txt"), id, "and the new name resolves to the same object");

    wfs_stub_teardown();
}

static void test_a_rename_can_cross_directories(void) {
    uint32_t id;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    id = resolve("/hello.txt");
    expect(id != 0u, "the file resolves");

    expect_rc(wfs_ns_rename(&g_vol, WFS_OBJECT_ROOT, "hello.txt", 9u, "/docs/moved", 11u, OP_NS),
              WASMOS_ERR_NONE,
              "the cross-directory rename succeeds");
    expect_u32(resolve("/hello.txt"), 0u, "the old name is gone");
    expect_u32(resolve("/docs/moved"), id, "and the file is under its new parent");
    expect(resolve("/docs/big.txt") != 0u, "the destination's other entries survive");

    wfs_stub_teardown();
}

static void test_rename_refusals(void) {
    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect_rc(wfs_ns_rename(&g_vol, WFS_OBJECT_ROOT, "absent", 6u, "x", 1u, OP_NS),
              WASMOS_ERR_FS_NOT_FOUND,
              "renaming an absent name is not found");
    /* This does not replace: doing so would need the destination removed before
     * the insert, leaving a window where neither name resolves. */
    expect_rc(wfs_ns_rename(&g_vol, WFS_OBJECT_ROOT, "hello.txt", 9u, "etc", 3u, OP_NS),
              WASMOS_ERR_FS_EXISTS,
              "renaming onto an existing name is refused");
    expect(resolve("/hello.txt") != 0u, "the source survives a refused rename");
    expect(resolve("/etc") != 0u, "and so does the destination");

    wfs_stub_teardown();
}

/* Every namespace op writes metadata, so each must mark the volume dirty. */
static void test_namespace_ops_mark_the_volume_dirty(void) {
    uint32_t id = 0u;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect_u32(g_vol.dirty_marked, 0u, "a fresh mount is not marked");
    expect_rc(wfs_ns_create(&g_vol, WFS_OBJECT_ROOT, "dm", 2u, WFS_TYPE_FILE, 0644u, OP_NS, &id),
              WASMOS_ERR_NONE,
              "the create succeeds");
    expect_u32(g_vol.dirty_marked, 1u, "and the volume is marked");

    wfs_stub_teardown();
}

/* ---- verifying the image ------------------------------------------------ */

/* Read an object record straight out of the image and check its checksum.
 * Returns 0 on success. The record is copied into `out` decoded only as far as
 * these checks need. */
static int image_object(uint32_t id, uint16_t* out_type, uint64_t* out_size,
                        uint32_t* out_extent_count, uint64_t extents[WFS_INLINE_EXTENTS][3]) {
    uint32_t bs = wfs_stub_block_size;
    uint32_t per_block = wfs_objects_per_block(bs);
    const uint8_t* d;
    uint32_t stored;
    uint32_t i;

    if (id == 0u || id >= g_layout.total_objects) {
        return -1;
    }
    d = wfs_stub_image + (size_t)(g_layout.object_table_start + id / per_block) * bs +
        (size_t)(id % per_block) * WFS_OBJECT_SIZE;
    stored = (uint32_t)d[offsetof(struct wfs_object, checksum)] |
             ((uint32_t)d[offsetof(struct wfs_object, checksum) + 1] << 8) |
             ((uint32_t)d[offsetof(struct wfs_object, checksum) + 2] << 16) |
             ((uint32_t)d[offsetof(struct wfs_object, checksum) + 3] << 24);
    if (stored !=
        wfs_checksum_struct(
            k_uuid, id, d, WFS_OBJECT_SIZE, (uint32_t)offsetof(struct wfs_object, checksum))) {
        return -1;
    }
    *out_type = (uint16_t)((uint32_t)d[offsetof(struct wfs_object, type)] |
                           ((uint32_t)d[offsetof(struct wfs_object, type) + 1] << 8));
    *out_size = 0u;
    for (i = 0; i < 8u; ++i) {
        *out_size |= (uint64_t)d[offsetof(struct wfs_object, size) + i] << (i * 8u);
    }
    *out_extent_count = (uint32_t)d[offsetof(struct wfs_object, extent_count)] |
                        ((uint32_t)d[offsetof(struct wfs_object, extent_count) + 1] << 8) |
                        ((uint32_t)d[offsetof(struct wfs_object, extent_count) + 2] << 16) |
                        ((uint32_t)d[offsetof(struct wfs_object, extent_count) + 3] << 24);
    if (*out_extent_count > WFS_INLINE_EXTENTS) {
        return -1;
    }
    for (i = 0; i < *out_extent_count; ++i) {
        const uint8_t* e = d + offsetof(struct wfs_object, extents) + i * sizeof(struct wfs_extent);
        uint32_t k;

        extents[i][0] = 0u;
        extents[i][1] = 0u;
        extents[i][2] = 0u;
        for (k = 0; k < 8u; ++k) {
            extents[i][0] |= (uint64_t)e[k] << (k * 8u);
            extents[i][1] |= (uint64_t)e[8u + k] << (k * 8u);
        }
        for (k = 0; k < 4u; ++k) {
            extents[i][2] |= (uint64_t)e[16u + k] << (k * 8u);
        }
    }
    return 0;
}

/* Walk the whole tree in the IMAGE and check that it is internally consistent:
 * every directory block's record chain validates and its tail checksum matches,
 * and every entry names an object whose own record verifies.
 *
 * This is what makes a failure assertion mean something. "The call returned
 * NO_SPACE and the source still resolves" would also hold if the failure came
 * from somewhere unintended -- a growth allocation, the object allocator -- while
 * leaving a half-written directory behind. Checking the image afterwards is the
 * difference between testing the ordering and testing that an error code came
 * back.
 *
 * Returns the number of live entries found, or -1 on any inconsistency. */
static int32_t verify_subtree(uint32_t dir_id, uint32_t depth) {
    uint32_t bs = wfs_stub_block_size;
    uint32_t usable = wfs_dir_usable_bytes(bs);
    uint16_t type = 0u;
    uint64_t size = 0u;
    uint32_t extent_count = 0u;
    uint64_t extents[WFS_INLINE_EXTENTS][3];
    uint32_t blocks;
    uint32_t logical;
    int32_t found = 0;

    if (depth > 8u) {
        return -1; /* a cycle, or deeper than any fixture builds */
    }
    if (image_object(dir_id, &type, &size, &extent_count, extents) != 0) {
        return -1;
    }
    if (type != WFS_TYPE_DIR) {
        return -1;
    }
    blocks = (uint32_t)((size + bs - 1u) / bs);
    for (logical = 0; logical < blocks; ++logical) {
        const uint8_t* blk = 0;
        uint32_t phys = 0u;
        uint32_t i;
        uint32_t off = 0u;
        uint32_t tail_at;
        uint32_t stored;

        for (i = 0; i < extent_count; ++i) {
            if ((uint64_t)logical >= extents[i][0] &&
                (uint64_t)logical < extents[i][0] + extents[i][2]) {
                phys = (uint32_t)(extents[i][1] + ((uint64_t)logical - extents[i][0]));
                break;
            }
        }
        if (phys == 0u) {
            continue; /* a hole maps nothing */
        }
        if (phys >= wfs_stub_blocks) {
            return -1;
        }
        blk = wfs_stub_image + (size_t)phys * bs;
        if (wfs_dirent_validate(blk, bs) != WASMOS_ERR_NONE) {
            return -1;
        }
        tail_at = usable + (uint32_t)offsetof(struct wfs_dir_tail, checksum);
        stored = (uint32_t)blk[tail_at] | ((uint32_t)blk[tail_at + 1] << 8) |
                 ((uint32_t)blk[tail_at + 2] << 16) | ((uint32_t)blk[tail_at + 3] << 24);
        if (stored != wfs_checksum_struct(k_uuid, phys, blk, bs, tail_at)) {
            return -1;
        }
        while (off + WFS_DIR_ENTRY_HEADER <= usable) {
            uint32_t len = (uint32_t)blk[off + 8u] | ((uint32_t)blk[off + 9u] << 8);
            uint32_t nl = blk[off + 10u];
            uint32_t id = 0u;
            uint32_t k;

            for (k = 0; k < 4u; ++k) {
                id |= (uint32_t)blk[off + k] << (k * 8u);
            }
            if (id != 0u && nl != 0u) {
                const uint8_t* nm = blk + off + WFS_DIR_ENTRY_HEADER;
                int is_dot = nl == 1u && nm[0] == '.';
                int is_dotdot = nl == 2u && nm[0] == '.' && nm[1] == '.';
                uint16_t child_type = 0u;
                uint64_t child_size = 0u;
                uint32_t child_extents = 0u;
                uint64_t child_ext[WFS_INLINE_EXTENTS][3];

                /* Every entry must name a record that VERIFIES. An entry pointing
                 * at an unallocated or half-written record is the corruption the
                 * write orderings exist to prevent. */
                if (image_object(id, &child_type, &child_size, &child_extents, child_ext) != 0) {
                    return -1;
                }
                if (!is_dot && !is_dotdot) {
                    found++;
                    if (child_type == WFS_TYPE_DIR) {
                        int32_t sub = verify_subtree(id, depth + 1u);

                        if (sub < 0) {
                            return -1;
                        }
                        found += sub;
                    }
                }
            }
            off += len;
        }
    }
    return found;
}

static void expect_consistent(const char* what) {
    g_checks++;
    if (verify_subtree(WFS_OBJECT_ROOT, 0u) < 0) {
        g_failures++;
        printf("[fail] %s: the image is not internally consistent\n", what);
    }
}

/* Allocate blocks until the volume has none, so a directory cannot GROW.
 * Returns how many runs were taken. */
static uint32_t exhaust_blocks(void) {
    uint32_t runs = 0u;

    for (;;) {
        wfs_alloc_ctx_t a;
        wasmos_wasm_coroutine_t task;

        memset(&a, 0, sizeof(a));
        a.vol = &g_vol;
        a.want = 64u;
        if (wfs_stub_run_task(&task, wfs_alloc_blocks_task, &a) != 0 || a.length == 0u) {
            break;
        }
        runs++;
        if (runs > 100000u) {
            break;
        }
    }
    return runs;
}

/* Fill a directory until it takes no more, returning how many names went in.
 * Only bounded once the volume has no free block: a directory GROWS, so with
 * space available this would run until the object table is exhausted instead. */
static uint32_t fill_directory(const char* dir) {
    uint32_t i;

    for (i = 0; i < 4096u; ++i) {
        char path[64];
        uint32_t id = 0u;

        snprintf(path, sizeof(path), "%s/p%04u", dir, (unsigned)i);
        if (wfs_ns_create(&g_vol,
                          WFS_OBJECT_ROOT,
                          path,
                          (uint32_t)strlen(path),
                          WFS_TYPE_FILE,
                          0644u,
                          OP_NS,
                          &id) != WASMOS_ERR_NONE) {
            break;
        }
    }
    return i;
}

/* The ORDER of a rename, tested through its observable consequence.
 *
 * Rename inserts the destination BEFORE removing the source. Interrupted, the
 * object is reachable under both names; the other order leaves it reachable under
 * neither, which loses the file. A crash cannot be staged here, but a FAILING
 * insert has the same shape.
 *
 * Manufacturing that failure takes two steps now that directories grow: exhaust
 * the volume's blocks so no directory CAN grow, then fill the destination's
 * existing block. Reversing the two operations in wfs_namespace.c makes exactly
 * this case fail -- checked, because without it nothing constrained the order.
 *
 * The image is verified afterwards, which is what stops this passing for the
 * wrong reason: "NO_SPACE came back and the source still resolves" would also
 * hold if the failure arrived from somewhere unintended while leaving a
 * half-written directory behind. */
static void test_a_failed_rename_leaves_the_source_in_place(void) {
    uint32_t id;
    uint32_t placed;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    id = resolve("/hello.txt");
    expect(id != 0u, "the source resolves to begin with");

    expect(exhaust_blocks() > 0u, "the volume's blocks are exhausted");
    placed = fill_directory("/etc");
    expect(placed > 0u, "and the destination's own block is filled");
    expect_consistent("after filling the destination");

    expect_rc(wfs_ns_rename(&g_vol, WFS_OBJECT_ROOT, "hello.txt", 9u, "/etc/hello.txt", 14u, OP_NS),
              WASMOS_ERR_FS_NO_SPACE,
              "the rename fails for want of room");
    expect_u32(resolve("/hello.txt"), id, "and the source is still there, under its own name");
    expect_u32(resolve("/etc/hello.txt"), 0u, "with nothing left behind at the destination");
    /* And nothing else was damaged on the way to that failure. */
    expect_consistent("after the failed rename");

    wfs_stub_teardown();
}

/* A create whose directory record cannot be inserted leaks an object rather than
 * writing an entry that names an unallocated id. The leak is what fsck reclaims;
 * the alternative is corruption no pass can repair. So what must hold is that the
 * image is still consistent and every name that resolved before still does. */
static void test_a_failed_create_leaves_the_volume_consistent(void) {
    uint32_t id = 0u;
    uint32_t placed;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    expect(exhaust_blocks() > 0u, "the volume's blocks are exhausted");
    placed = fill_directory("/etc");
    expect(placed > 0u, "the directory's own block is filled");

    expect_rc(wfs_ns_create(
                  &g_vol, WFS_OBJECT_ROOT, "/etc/one-more", 13u, WFS_TYPE_FILE, 0644u, OP_NS, &id),
              WASMOS_ERR_FS_NO_SPACE,
              "the create fails for want of room");
    expect_u32(resolve("/etc/one-more"), 0u, "the name does not resolve");
    expect(resolve("/hello.txt") != 0u, "the other entries still resolve");
    expect(resolve("/docs/big.txt") != 0u, "including nested ones");
    expect(resolve("/etc/p0000") != 0u, "and the ones this case created");
    expect_consistent("after the failed create");

    wfs_stub_teardown();
}

/* ---- directory growth --------------------------------------------------- */

/* A directory outgrows its first block. Every name has to stay reachable across
 * the boundary, which is what a growth that mislaid the extent map would break --
 * and the image is verified, so a growth that produced a block no scan can walk
 * fails here rather than at some later read. */
static void test_a_directory_grows_past_one_block(void) {
    uint32_t i;
    uint32_t made = 0u;
    /* More entries than one 4096-byte block holds: the tail leaves 4080 bytes and
     * each of these needs 24, so ~170 fit and 400 cannot. */
    const uint32_t want = 400u;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    for (i = 0; i < want; ++i) {
        char path[32];
        uint32_t id = 0u;

        snprintf(path, sizeof(path), "/etc/grow%04u", (unsigned)i);
        if (wfs_ns_create(&g_vol,
                          WFS_OBJECT_ROOT,
                          path,
                          (uint32_t)strlen(path),
                          WFS_TYPE_FILE,
                          0644u,
                          OP_NS,
                          &id) != WASMOS_ERR_NONE) {
            break;
        }
        made++;
    }
    expect_u32(made, want, "every name went in, so the directory grew");
    expect(resolve("/etc") != 0u, "the directory still resolves");
    expect(g_path.object.out.size > 4096u, "and now spans more than one block");
    expect(g_path.object.out.extent_count >= 1u, "with its extent map intact");

    /* Reachability across the boundary, not just the count: the first name, the
     * last, and one in between. */
    expect(resolve("/etc/grow0000") != 0u, "the first name is reachable");
    expect(resolve("/etc/grow0199") != 0u, "one past the first block is reachable");
    expect(resolve("/etc/grow0399") != 0u, "and so is the last");
    expect(resolve("/etc/wfs.conf") == 0u, "a name never created does not resolve");
    expect_consistent("after growing the directory");

    wfs_stub_teardown();
}

/* Growth is not one-way: an entry in a grown-into block can be removed, and the
 * ones around it survive. */
static void test_an_entry_in_a_grown_block_can_be_removed(void) {
    uint32_t i;
    uint32_t victim;

    if (setup() != 0) {
        wfs_stub_teardown();
        return;
    }
    for (i = 0; i < 300u; ++i) {
        char path[32];
        uint32_t id = 0u;

        snprintf(path, sizeof(path), "/etc/g%04u", (unsigned)i);
        if (wfs_ns_create(&g_vol,
                          WFS_OBJECT_ROOT,
                          path,
                          (uint32_t)strlen(path),
                          WFS_TYPE_FILE,
                          0644u,
                          OP_NS,
                          &id) != WASMOS_ERR_NONE) {
            break;
        }
    }
    victim = resolve("/etc/g0250");
    expect(victim != 0u, "a name in a grown-into block resolves");

    expect_rc(wfs_ns_unlink(&g_vol, WFS_OBJECT_ROOT, "/etc/g0250", 10u, OP_NS),
              WASMOS_ERR_NONE,
              "and can be unlinked");
    expect_u32(resolve("/etc/g0250"), 0u, "it no longer resolves");
    expect(resolve("/etc/g0249") != 0u, "its neighbour below survives");
    expect(resolve("/etc/g0251") != 0u, "and its neighbour above");
    expect_consistent("after removing from a grown block");

    wfs_stub_teardown();
}

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_a_created_file_resolves),
    WASMOS_TEST_CASE(test_a_created_directory_has_dot_and_dotdot),
    WASMOS_TEST_CASE(test_a_file_can_be_created_inside_a_new_directory),
    WASMOS_TEST_CASE(test_create_refusals),
    WASMOS_TEST_CASE(test_an_unlinked_file_stops_resolving),
    WASMOS_TEST_CASE(test_unlinking_releases_the_data_blocks),
    WASMOS_TEST_CASE(test_unlink_refusals),
    WASMOS_TEST_CASE(test_an_empty_directory_can_be_removed),
    WASMOS_TEST_CASE(test_a_populated_directory_is_not_removed),
    WASMOS_TEST_CASE(test_rmdir_refuses_a_file),
    WASMOS_TEST_CASE(test_a_rename_moves_the_name_not_the_object),
    WASMOS_TEST_CASE(test_a_rename_can_cross_directories),
    WASMOS_TEST_CASE(test_rename_refusals),
    WASMOS_TEST_CASE(test_namespace_ops_mark_the_volume_dirty),
    WASMOS_TEST_CASE(test_a_failed_rename_leaves_the_source_in_place),
    WASMOS_TEST_CASE(test_a_failed_create_leaves_the_volume_consistent),
    WASMOS_TEST_CASE(test_a_directory_grows_past_one_block),
    WASMOS_TEST_CASE(test_an_entry_in_a_grown_block_can_be_removed),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_namespace: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_namespace: %d checks passed\n", g_checks);
    return 0;
}
