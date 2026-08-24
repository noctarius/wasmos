/* Host unit test for the WFS extent-map walk (src/drivers/fs_wfs/wfs_extent.c).
 *
 * The walk is what every read above it goes through, so it is tested against
 * real node blocks rather than a model: the cases write extent-tree nodes into a
 * volume mkfs_wfs formatted, then run the task on the system coroutine runtime
 * over the shared fake block server (stubs_wfs_block_server.c).
 *
 * The OBJECT is built in memory rather than written to the image, because the
 * walk takes a record it was handed — resolving an object id is
 * wfs_object_task's job, covered by test_wfs_mount.c. What must come off the
 * device is the tree.
 */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "stubs_wfs_block_server.h"
#include "wasmos_status.h"
#include "wfs_crc32c.h"
#include "wfs_extent.h"
#include "wfs_format.h"
#include "wfs_types.h"

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
    0xa1, 0x0c, 0x5e, 0x77, 0x2b, 0x94, 0x4d, 0x03, 0xb6, 0x18, 0x7f, 0xe2, 0x35, 0x8c, 0x60, 0x49};
#define NOW_NS 1750000000000000000ull
#define VOL_16M (16ull * 1024ull * 1024ull)

static wfs_mkfs_layout_t g_layout;
static wfs_volume_t g_vol;

/* ---- writing nodes into the image --------------------------------------- */

static void wr16(uint8_t* p, uint32_t off, uint16_t v) {
    p[off] = (uint8_t)(v & 0xFFu);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32(uint8_t* p, uint32_t off, uint32_t v) {
    p[off] = (uint8_t)(v & 0xFFu);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
    p[off + 2] = (uint8_t)((v >> 16) & 0xFFu);
    p[off + 3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wr64(uint8_t* p, uint32_t off, uint64_t v) {
    wr32(p, off, (uint32_t)(v & 0xFFFFFFFFu));
    wr32(p, off + 4u, (uint32_t)(v >> 32));
}

static uint8_t* image_block(uint32_t block) {
    return wfs_stub_image + (size_t)block * wfs_stub_block_size;
}

/* Seal a node: the checksum covers the whole block with its own four bytes
 * zeroed, seeded with the block's number (§13). */
static void seal_node(uint32_t block) {
    uint8_t* n = image_block(block);
    uint32_t off = (uint32_t)offsetof(struct wfs_extent_header, checksum);

    wr32(n, off, 0u);
    wr32(n, off, wfs_checksum_struct(k_uuid, block, n, wfs_stub_block_size, off));
}

static void write_header(uint32_t block, uint16_t depth, uint16_t entries, uint16_t capacity) {
    uint8_t* n = image_block(block);

    memset(n, 0, wfs_stub_block_size);
    wr16(n, (uint32_t)offsetof(struct wfs_extent_header, magic), WFS_EXTENT_NODE_MAGIC);
    wr16(n, (uint32_t)offsetof(struct wfs_extent_header, depth), depth);
    wr16(n, (uint32_t)offsetof(struct wfs_extent_header, entries), entries);
    wr16(n, (uint32_t)offsetof(struct wfs_extent_header, capacity), capacity);
}

static void write_leaf_extent(uint32_t block, uint32_t slot, uint64_t logical, uint64_t physical,
                              uint32_t length) {
    uint8_t* n = image_block(block) + sizeof(struct wfs_extent_header) +
                 (size_t)slot * sizeof(struct wfs_extent);

    wr64(n, (uint32_t)offsetof(struct wfs_extent, logical_block), logical);
    wr64(n, (uint32_t)offsetof(struct wfs_extent, physical_block), physical);
    wr32(n, (uint32_t)offsetof(struct wfs_extent, length), length);
}

static void write_index(uint32_t block, uint32_t slot, uint64_t logical, uint64_t child) {
    uint8_t* n = image_block(block) + sizeof(struct wfs_extent_header) +
                 (size_t)slot * sizeof(struct wfs_extent_index);

    wr64(n, (uint32_t)offsetof(struct wfs_extent_index, logical_block), logical);
    wr64(n, (uint32_t)offsetof(struct wfs_extent_index, child_block), child);
}

static uint32_t leaf_cap(void) {
    return wfs_extent_leaf_capacity(wfs_stub_block_size);
}

static uint32_t interior_cap(void) {
    return wfs_extent_interior_capacity(wfs_stub_block_size);
}

/* A scratch block beyond the root directory's, which mkfs left free. */
static uint32_t scratch(uint32_t n) {
    return g_layout.root_data_block + 1u + n;
}

static int setup(void) {
    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, NOW_NS, &g_layout) != 0) {
        return -1;
    }
    memset(&g_vol, 0, sizeof(g_vol));
    /* The walk needs only the geometry and the uuid, both of which the parsed
     * superblock carries; mounting is covered elsewhere. */
    if (wfs_super_parse(wfs_stub_image + WFS_SUPER_OFFSET, WFS_SUPER_SIZE, 0u, &g_vol.super) !=
        WASMOS_ERR_NONE) {
        return -1;
    }
    g_vol.mounted = 1u;
    return 0;
}

static int32_t resolve(wfs_extent_ctx_t* ctx, const struct wfs_object* obj, uint64_t logical) {
    wasmos_wasm_coroutine_t task;

    memset(ctx, 0, sizeof(*ctx));
    ctx->vol = &g_vol;
    ctx->obj = obj;
    ctx->logical = logical;
    return wfs_stub_run_task(&task, wfs_extent_task, ctx);
}

/* ---- inline maps -------------------------------------------------------- */

/* An inline map is answered from the record already in hand, so a small file
 * costs no metadata block beyond its own record. */
static void test_an_inline_map_costs_no_device_read(void) {
    struct wfs_object obj;
    wfs_extent_ctx_t ctx;
    uint32_t before;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    memset(&obj, 0, sizeof(obj));
    obj.extent_count = 1u;
    obj.extents[0].logical_block = 0u;
    obj.extents[0].physical_block = 500u;
    obj.extents[0].length = 4u;

    before = wfs_stub_reads;
    expect(resolve(&ctx, &obj, 2u) == 0, "an inline map resolves");
    expect(wfs_stub_reads == before, "and reads nothing from the device");
    expect(ctx.found == 1u, "the block is mapped");
    expect(ctx.physical == 502u, "the physical block is the extent's, plus the offset");
    expect(ctx.run == 2u, "the run is what remains of the extent");

    wfs_stub_teardown();
}

/* Several disjoint inline extents, and the offset arithmetic inside each. */
static void test_inline_extents_are_searched_by_range(void) {
    struct wfs_object obj;
    wfs_extent_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    memset(&obj, 0, sizeof(obj));
    obj.extent_count = 3u;
    obj.extents[0].logical_block = 0u;
    obj.extents[0].physical_block = 100u;
    obj.extents[0].length = 2u;
    obj.extents[1].logical_block = 2u;
    obj.extents[1].physical_block = 700u;
    obj.extents[1].length = 3u;
    obj.extents[2].logical_block = 10u;
    obj.extents[2].physical_block = 900u;
    obj.extents[2].length = 1u;

    expect(resolve(&ctx, &obj, 0u) == 0 && ctx.physical == 100u && ctx.run == 2u,
           "the first extent's first block");
    expect(resolve(&ctx, &obj, 1u) == 0 && ctx.physical == 101u && ctx.run == 1u,
           "the first extent's last block");
    expect(resolve(&ctx, &obj, 3u) == 0 && ctx.physical == 701u && ctx.run == 2u,
           "inside the second extent");
    expect(resolve(&ctx, &obj, 10u) == 0 && ctx.physical == 900u && ctx.run == 1u,
           "the third extent");

    wfs_stub_teardown();
}

/* A logical block no extent covers is a HOLE: it reads as zeroes and is not an
 * error, because a sparsely written file has ranges nothing maps. */
static void test_an_unmapped_block_is_a_hole_not_an_error(void) {
    struct wfs_object obj;
    wfs_extent_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    memset(&obj, 0, sizeof(obj));
    obj.extent_count = 2u;
    obj.extents[0].logical_block = 0u;
    obj.extents[0].physical_block = 100u;
    obj.extents[0].length = 2u;
    obj.extents[1].logical_block = 8u;
    obj.extents[1].physical_block = 200u;
    obj.extents[1].length = 1u;

    expect(resolve(&ctx, &obj, 4u) == 0, "a gap between extents is not a failure");
    expect(ctx.found == 0u, "and reports no mapping");
    expect(resolve(&ctx, &obj, 99u) == 0 && ctx.found == 0u, "past the last extent is a hole too");

    wfs_stub_teardown();
}

/* More inline extents than the record holds, with no tree to hold them, is a
 * record that cannot be believed. */
static void test_too_many_inline_extents_is_corrupt(void) {
    struct wfs_object obj;
    wfs_extent_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    memset(&obj, 0, sizeof(obj));
    obj.extent_count = WFS_INLINE_EXTENTS + 1u;
    expect_rc((wasmos_error_code_t)resolve(&ctx, &obj, 0u),
              WASMOS_ERR_FS_CORRUPT,
              "extent_count past the inline array with no tree");

    wfs_stub_teardown();
}

/* ---- a leaf-only tree --------------------------------------------------- */

static void test_a_leaf_root_resolves(void) {
    struct wfs_object obj;
    wfs_extent_ctx_t ctx;
    uint32_t root = 0;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    root = scratch(0);
    write_header(root, 0u, 2u, (uint16_t)leaf_cap());
    write_leaf_extent(root, 0u, 0u, 300u, 4u);
    write_leaf_extent(root, 1u, 4u, 800u, 2u);
    seal_node(root);

    memset(&obj, 0, sizeof(obj));
    obj.extent_tree_block = root;
    obj.extent_count = 2u;

    expect(resolve(&ctx, &obj, 1u) == 0 && ctx.physical == 301u && ctx.run == 3u,
           "inside the first leaf extent");
    expect(resolve(&ctx, &obj, 5u) == 0 && ctx.physical == 801u && ctx.run == 1u,
           "inside the second");
    expect(resolve(&ctx, &obj, 6u) == 0 && ctx.found == 0u, "past the last is a hole");

    /* The root node is one block, and reading it is the only device traffic. */
    wfs_stub_reset_counters();
    expect(resolve(&ctx, &obj, 1u) == 0, "resolve again");
    expect(wfs_stub_req_count <= 1u, "a leaf-only tree costs at most one read");

    wfs_stub_teardown();
}

/* ---- a two-level tree --------------------------------------------------- */

/* Descent, and the rule that picks the child: the LAST index whose
 * logical_block does not exceed the target. Picking the first, or the one whose
 * range merely contains the target's start, sends the walk down the wrong
 * subtree — which surfaces as a hole or a wrong physical block, not as an
 * error. */
static void test_an_interior_root_descends_to_the_right_leaf(void) {
    struct wfs_object obj;
    wfs_extent_ctx_t ctx;
    uint32_t root;
    uint32_t left;
    uint32_t right;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    root = scratch(0);
    left = scratch(1);
    right = scratch(2);

    write_header(left, 0u, 2u, (uint16_t)leaf_cap());
    write_leaf_extent(left, 0u, 0u, 400u, 4u);
    write_leaf_extent(left, 1u, 4u, 500u, 4u);
    seal_node(left);

    write_header(right, 0u, 2u, (uint16_t)leaf_cap());
    write_leaf_extent(right, 0u, 100u, 600u, 4u);
    write_leaf_extent(right, 1u, 104u, 700u, 4u);
    seal_node(right);

    write_header(root, 1u, 2u, (uint16_t)interior_cap());
    write_index(root, 0u, 0u, left);
    write_index(root, 1u, 100u, right);
    seal_node(root);

    memset(&obj, 0, sizeof(obj));
    obj.extent_tree_block = root;
    obj.extent_count = 4u;

    expect(resolve(&ctx, &obj, 5u) == 0 && ctx.physical == 501u, "a target in the left subtree");
    expect(resolve(&ctx, &obj, 105u) == 0 && ctx.physical == 701u, "a target in the right subtree");
    expect(resolve(&ctx, &obj, 50u) == 0 && ctx.found == 0u,
           "a target in the left subtree's gap is a hole");
    expect(resolve(&ctx, &obj, 200u) == 0 && ctx.found == 0u, "a target past every leaf is a hole");

    /* The descent reads the root then exactly one leaf, and which leaf is the
     * whole point. */
    wfs_stub_reset_counters();
    expect(resolve(&ctx, &obj, 105u) == 0, "resolve into the right subtree");
    expect(wfs_stub_req_count == 2u, "the descent reads two nodes");
    if (wfs_stub_req_count >= 2u) {
        expect(wfs_stub_req_blocks[0] == root, "the root first");
        expect(wfs_stub_req_blocks[1] == right, "then the leaf that covers the target");
    }

    wfs_stub_teardown();
}

/* A target below every index means nothing maps it. */
static void test_a_target_below_every_index_is_a_hole(void) {
    struct wfs_object obj;
    wfs_extent_ctx_t ctx;
    uint32_t root;
    uint32_t leaf;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    root = scratch(0);
    leaf = scratch(1);

    write_header(leaf, 0u, 1u, (uint16_t)leaf_cap());
    write_leaf_extent(leaf, 0u, 100u, 600u, 4u);
    seal_node(leaf);

    write_header(root, 1u, 1u, (uint16_t)interior_cap());
    write_index(root, 0u, 100u, leaf);
    seal_node(root);

    memset(&obj, 0, sizeof(obj));
    obj.extent_tree_block = root;

    expect(resolve(&ctx, &obj, 5u) == 0, "a target before the first index is not a failure");
    expect(ctx.found == 0u, "and reports no mapping");

    wfs_stub_teardown();
}

/* ---- nodes that must be refused ---------------------------------------- */

static void test_a_node_that_is_not_a_node_is_refused(void) {
    struct wfs_object obj;
    wfs_extent_ctx_t ctx;
    uint32_t root;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    root = scratch(0);
    write_header(root, 0u, 1u, (uint16_t)leaf_cap());
    write_leaf_extent(root, 0u, 0u, 300u, 1u);
    /* Magic distinguishes a node from a data block a corrupt pointer named. */
    wr16(image_block(root), (uint32_t)offsetof(struct wfs_extent_header, magic), 0x1234u);
    seal_node(root);

    memset(&obj, 0, sizeof(obj));
    obj.extent_tree_block = root;
    expect_rc((wasmos_error_code_t)resolve(&ctx, &obj, 0u),
              WASMOS_ERR_FS_CORRUPT,
              "a block without the node magic");

    wfs_stub_teardown();
}

static void test_a_node_that_does_not_verify_is_refused(void) {
    struct wfs_object obj;
    wfs_extent_ctx_t ctx;
    uint32_t root;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    root = scratch(0);
    write_header(root, 0u, 1u, (uint16_t)leaf_cap());
    write_leaf_extent(root, 0u, 0u, 300u, 1u);
    seal_node(root);
    /* One flipped bit in a record the checksum covers. */
    image_block(root)[sizeof(struct wfs_extent_header) + 1u] ^= 0x04u;

    memset(&obj, 0, sizeof(obj));
    obj.extent_tree_block = root;
    expect_rc(
        (wasmos_error_code_t)resolve(&ctx, &obj, 0u), WASMOS_ERR_FS_CHECKSUM, "a corrupted node");

    wfs_stub_teardown();
}

/* A node sealed for one block must not verify at another: the seed binds it to
 * its location (§13). */
static void test_a_node_is_bound_to_its_block(void) {
    struct wfs_object obj;
    wfs_extent_ctx_t ctx;
    uint32_t built;
    uint32_t elsewhere;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    built = scratch(0);
    elsewhere = scratch(1);
    write_header(built, 0u, 1u, (uint16_t)leaf_cap());
    write_leaf_extent(built, 0u, 0u, 300u, 1u);
    seal_node(built);
    memcpy(image_block(elsewhere), image_block(built), wfs_stub_block_size);

    memset(&obj, 0, sizeof(obj));
    obj.extent_tree_block = elsewhere;
    expect_rc((wasmos_error_code_t)resolve(&ctx, &obj, 0u),
              WASMOS_ERR_FS_CHECKSUM,
              "a node copied to another block");

    wfs_stub_teardown();
}

/* `capacity` is derived from the block size, so a value that disagrees means the
 * node was written by something with a different idea of the layout, and its
 * records cannot be indexed safely. */
static void test_a_node_with_the_wrong_capacity_is_refused(void) {
    struct wfs_object obj;
    wfs_extent_ctx_t ctx;
    uint32_t root;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    root = scratch(0);
    write_header(root, 0u, 1u, (uint16_t)(leaf_cap() + 1u));
    write_leaf_extent(root, 0u, 0u, 300u, 1u);
    seal_node(root);

    memset(&obj, 0, sizeof(obj));
    obj.extent_tree_block = root;
    expect_rc((wasmos_error_code_t)resolve(&ctx, &obj, 0u),
              WASMOS_ERR_FS_CORRUPT,
              "a capacity the block cannot hold");

    /* And an entry count past the capacity, which would index past the block. */
    write_header(root, 0u, (uint16_t)(leaf_cap() + 1u), (uint16_t)leaf_cap());
    seal_node(root);
    expect_rc((wasmos_error_code_t)resolve(&ctx, &obj, 0u),
              WASMOS_ERR_FS_CORRUPT,
              "more entries than the capacity");

    wfs_stub_teardown();
}

/* An extent that verifies can still point outside the volume, and the read that
 * followed would address whatever the device returns past its end. */
static void test_an_extent_outside_the_volume_is_refused(void) {
    struct wfs_object obj;
    wfs_extent_ctx_t ctx;
    uint32_t root;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    root = scratch(0);
    write_header(root, 0u, 1u, (uint16_t)leaf_cap());
    write_leaf_extent(root, 0u, 0u, g_vol.super.total_blocks - 1u, 4u);
    seal_node(root);

    memset(&obj, 0, sizeof(obj));
    obj.extent_tree_block = root;
    expect_rc((wasmos_error_code_t)resolve(&ctx, &obj, 0u),
              WASMOS_ERR_FS_CORRUPT,
              "an extent running past the last block");

    wfs_stub_teardown();
}

/* A child that does not shrink the depth is a cycle, and a cycle would spin the
 * task against the device forever. */
static void test_a_cyclic_tree_is_refused_rather_than_walked(void) {
    struct wfs_object obj;
    wfs_extent_ctx_t ctx;
    uint32_t root;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    root = scratch(0);
    /* An interior node whose only child is itself. */
    write_header(root, 1u, 1u, (uint16_t)interior_cap());
    write_index(root, 0u, 0u, root);
    seal_node(root);

    memset(&obj, 0, sizeof(obj));
    obj.extent_tree_block = root;
    expect_rc((wasmos_error_code_t)resolve(&ctx, &obj, 0u),
              WASMOS_ERR_FS_CORRUPT,
              "a node that is its own child");

    wfs_stub_teardown();
}

static void test_a_tree_root_outside_the_volume_is_refused(void) {
    struct wfs_object obj;
    wfs_extent_ctx_t ctx;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    memset(&obj, 0, sizeof(obj));
    obj.extent_tree_block = g_vol.super.total_blocks;
    expect_rc((wasmos_error_code_t)resolve(&ctx, &obj, 0u),
              WASMOS_ERR_FS_CORRUPT,
              "a tree root past the last block");

    wfs_stub_teardown();
}

static void test_a_device_error_fails_the_walk(void) {
    struct wfs_object obj;
    wfs_extent_ctx_t ctx;
    uint32_t root;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    root = scratch(0);
    write_header(root, 0u, 1u, (uint16_t)leaf_cap());
    write_leaf_extent(root, 0u, 0u, 300u, 1u);
    seal_node(root);

    memset(&obj, 0, sizeof(obj));
    obj.extent_tree_block = root;
    wfs_stub_fail_next = 1;
    expect_rc((wasmos_error_code_t)resolve(&ctx, &obj, 0u),
              WASMOS_ERR_FS_IO,
              "a failed transfer fails the walk");

    wfs_stub_teardown();
}

/* ---- runner -------------------------------------------------------------- */

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_an_inline_map_costs_no_device_read),
    WASMOS_TEST_CASE(test_inline_extents_are_searched_by_range),
    WASMOS_TEST_CASE(test_an_unmapped_block_is_a_hole_not_an_error),
    WASMOS_TEST_CASE(test_too_many_inline_extents_is_corrupt),
    WASMOS_TEST_CASE(test_a_leaf_root_resolves),
    WASMOS_TEST_CASE(test_an_interior_root_descends_to_the_right_leaf),
    WASMOS_TEST_CASE(test_a_target_below_every_index_is_a_hole),
    WASMOS_TEST_CASE(test_a_node_that_is_not_a_node_is_refused),
    WASMOS_TEST_CASE(test_a_node_that_does_not_verify_is_refused),
    WASMOS_TEST_CASE(test_a_node_is_bound_to_its_block),
    WASMOS_TEST_CASE(test_a_node_with_the_wrong_capacity_is_refused),
    WASMOS_TEST_CASE(test_an_extent_outside_the_volume_is_refused),
    WASMOS_TEST_CASE(test_a_cyclic_tree_is_refused_rather_than_walked),
    WASMOS_TEST_CASE(test_a_tree_root_outside_the_volume_is_refused),
    WASMOS_TEST_CASE(test_a_device_error_fails_the_walk),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_extent: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_extent: %d checks passed\n", g_checks);
    return 0;
}
