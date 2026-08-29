/* End-to-end host test: mkfs_wfs populates a volume, and the DRIVER reads it
 * back — mount, object records, the directory scan, and the extent map, all as
 * tasks on the system coroutine runtime over the shared fake block server.
 *
 * This is the first test where the writer and the reader meet on a volume with
 * content in it. Every other suite checks one side: the format tests check the
 * reader against images they built themselves, and test_wfs_mkfs checks the
 * writer's output field by field. Here a disagreement about record packing, an
 * object id, a link count, a checksum seed, or an extent shows up as a driver
 * that cannot find a file the formatter put there.
 *
 * The tree is declared in memory rather than read from the host: what is under
 * test is the formatter and the driver, not opendir.
 */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "stubs_wfs_block_server.h"
#include "wasmos_status.h"
#include "wfs_dir.h"
#include "wfs_extent.h"
#include "wfs_format.h"
#include "wfs_mount.h"

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
    0x0e, 0x8a, 0x31, 0x55, 0xd7, 0x62, 0x4f, 0x1b, 0x90, 0x43, 0xcc, 0x27, 0x6b, 0xf4, 0x18, 0xa5};
#define NOW_NS 1750000000000000000ull
#define VOL_16M (16ull * 1024ull * 1024ull)

/* ---- the tree under test ------------------------------------------------ */

/* Deterministic content, so a test can predict any byte of any file without
 * holding it. */
static uint8_t content_byte(uint32_t seed, uint64_t offset) {
    return (uint8_t)((seed * 31u + (uint32_t)(offset * 7u) + (uint32_t)(offset >> 8)) & 0xFFu);
}

typedef struct {
    uint32_t seed;
    uint64_t size;
} gen_t;

static int gen_read(void* ctx, uint64_t offset, void* dst, uint32_t len) {
    gen_t* g = (gen_t*)ctx;
    uint8_t* p = (uint8_t*)dst;
    uint32_t i;

    for (i = 0; i < len; ++i) {
        p[i] = (offset + i) < g->size ? content_byte(g->seed, offset + i) : 0u;
    }
    return 0;
}

/* Indices into the entry array below, so a case can name what it is checking. */
enum {
    E_README = 0,
    E_BIN,
    E_BIG,
    E_ETC,
    E_SMALL,
    E_NESTED,
    E_ZERO,
    E_COUNT,
};

#define BIG_SIZE 20000u
#define SMALL_SIZE 40u

static gen_t g_gen[E_COUNT];
static wfs_mkfs_entry_t g_entries[E_COUNT];
static wfs_mkfs_node_t g_plan[E_COUNT + 1u];
static wfs_mkfs_layout_t g_layout;
static wfs_volume_t g_vol;

static void add(uint32_t idx, const char* name, uint32_t parent, int is_dir, uint64_t size,
                uint32_t seed) {
    memset(&g_entries[idx], 0, sizeof(g_entries[idx]));
    g_entries[idx].name = name;
    g_entries[idx].name_len = (uint32_t)strlen(name);
    g_entries[idx].parent = parent;
    g_entries[idx].is_dir = (uint8_t)(is_dir ? 1 : 0);
    g_entries[idx].mode = is_dir ? 0755u : 0644u;
    if (!is_dir) {
        g_gen[idx].seed = seed;
        g_gen[idx].size = size;
        g_entries[idx].size = size;
        g_entries[idx].read = gen_read;
        g_entries[idx].read_ctx = &g_gen[idx];
    }
}

/* /README  /bin/big.bin  /etc/small.conf  /etc/nested/zero */
static void declare_tree(void) {
    add(E_README, "README", WFS_MKFS_ROOT, 0, 15u, 3u);
    add(E_BIN, "bin", WFS_MKFS_ROOT, 1, 0u, 0u);
    add(E_BIG, "big.bin", E_BIN, 0, BIG_SIZE, 11u);
    add(E_ETC, "etc", WFS_MKFS_ROOT, 1, 0u, 0u);
    add(E_SMALL, "small.conf", E_ETC, 0, SMALL_SIZE, 5u);
    add(E_NESTED, "nested", E_ETC, 1, 0u, 0u);
    add(E_ZERO, "zero", E_NESTED, 0, 0u, 0u);
}

/* Format the tree into a fresh in-memory volume and mount it with the driver. */
static int setup(void) {
    wfs_mkfs_params_t params;
    wfs_mkfs_sink_t sink;
    wfs_mount_ctx_t ctx;
    wasmos_wasm_coroutine_t task;

    declare_tree();

    memset(&params, 0, sizeof(params));
    params.size_bytes = VOL_16M;
    params.block_size = 4096u;
    params.now_ns = NOW_NS;
    memcpy(params.uuid, k_uuid, WFS_UUID_LEN);

    /* Build an empty volume first, only to get an image of the right size that
     * the shared fixture owns; then overwrite it with the populated one. */
    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, NOW_NS, &g_layout) != 0) {
        return -1;
    }
    sink.ctx = NULL;
    sink.write_block = wfs_stub_sink_write;
    if (wfs_mkfs_format_tree(&params, g_entries, E_COUNT, g_plan, &sink, &g_layout) !=
        WASMOS_ERR_NONE) {
        return -1;
    }
    wfs_stub_reset_counters();

    memset(&ctx, 0, sizeof(ctx));
    memset(&g_vol, 0, sizeof(g_vol));
    ctx.vol = &g_vol;
    if (wfs_stub_run_task(&task, wfs_mount_task, &ctx) != 0) {
        return -1;
    }
    return g_vol.mounted ? 0 : -1;
}

static int32_t read_object(wfs_object_ctx_t* o, uint32_t object_id) {
    wasmos_wasm_coroutine_t task;

    memset(o, 0, sizeof(*o));
    o->vol = &g_vol;
    o->object_id = object_id;
    return wfs_stub_run_task(&task, wfs_object_task, o);
}

static int32_t lookup(wfs_dir_ctx_t* d, const struct wfs_object* dir, const char* name) {
    wasmos_wasm_coroutine_t task;

    wfs_dir_lookup_init(d, &g_vol, dir, name, (uint32_t)strlen(name));
    return wfs_stub_run_task(&task, wfs_dir_task, d);
}

static int32_t map_block(wfs_extent_ctx_t* e, const struct wfs_object* obj, uint64_t logical) {
    wasmos_wasm_coroutine_t task;

    memset(e, 0, sizeof(*e));
    e->vol = &g_vol;
    e->obj = obj;
    e->logical = logical;
    return wfs_stub_run_task(&task, wfs_extent_task, e);
}

/* Resolve a path from the root, one component at a time, exactly as a real
 * lookup would. Returns 0 and fills `out` on success. */
static int32_t resolve_path(const char* const* parts, uint32_t n, struct wfs_object* out) {
    wfs_object_ctx_t o;
    wfs_dir_ctx_t d;
    uint32_t id = WFS_OBJECT_ROOT;
    uint32_t i;
    int32_t rc;

    for (i = 0; i < n; ++i) {
        rc = read_object(&o, id);
        if (rc != 0) {
            return rc;
        }
        rc = lookup(&d, &o.out, parts[i]);
        if (rc != 0) {
            return rc;
        }
        if (!d.found) {
            return WASMOS_ERR_FS_NOT_FOUND;
        }
        id = d.object_id;
    }
    rc = read_object(&o, id);
    if (rc != 0) {
        return rc;
    }
    *out = o.out;
    return 0;
}

/* ---- the volume as the driver sees it ----------------------------------- */

static void test_a_populated_volume_mounts(void) {
    if (setup() != 0) {
        expect(0, "format and mount a populated volume");
        return;
    }
    expect(g_vol.mounted == 1u, "the volume mounts");
    expect(g_vol.super.total_blocks == g_layout.total_blocks, "geometry round-trips");
    expect(g_layout.entry_count == (uint32_t)E_COUNT, "every entry was placed");
    /* Object ids are dense from the first allocatable one (§25). */
    expect(g_plan[E_README].object_id == WFS_OBJECT_FIRST, "ids start at the first allocatable");
    expect(g_plan[E_ZERO].object_id == WFS_OBJECT_FIRST + E_ZERO, "and run densely");

    wfs_stub_teardown();
}

static void test_every_top_level_name_resolves(void) {
    wfs_object_ctx_t root;
    wfs_dir_ctx_t d;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    expect(read_object(&root, WFS_OBJECT_ROOT) == 0, "the root record reads");
    expect(root.out.type == WFS_TYPE_DIR, "the root is a directory");

    expect(lookup(&d, &root.out, "README") == 0 && d.found == 1u, "README is found");
    expect(d.object_id == g_plan[E_README].object_id, "and names the object mkfs gave it");
    expect(d.type == WFS_TYPE_FILE, "recorded as a file");

    expect(lookup(&d, &root.out, "bin") == 0 && d.object_id == g_plan[E_BIN].object_id,
           "bin is found");
    expect(d.type == WFS_TYPE_DIR, "recorded as a directory");
    expect(lookup(&d, &root.out, "etc") == 0 && d.object_id == g_plan[E_ETC].object_id,
           "etc is found");

    /* Nothing the tree did not declare. */
    expect(lookup(&d, &root.out, "big.bin") == 0 && d.found == 0u,
           "a child of a subdirectory is not in the root");

    wfs_stub_teardown();
}

/* The whole point of the tree: descend it the way a path lookup does. */
static void test_a_nested_path_resolves_component_by_component(void) {
    static const char* const deep[] = {"etc", "nested", "zero"};
    static const char* const mid[] = {"etc", "small.conf"};
    struct wfs_object obj;

    memset(&obj, 0, sizeof(obj));

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    expect(resolve_path(mid, 2u, &obj) == 0, "/etc/small.conf resolves");
    expect(obj.object_id == g_plan[E_SMALL].object_id, "to the object mkfs gave it");
    expect(obj.size == SMALL_SIZE, "with its size");

    expect(resolve_path(deep, 3u, &obj) == 0, "/etc/nested/zero resolves three levels down");
    expect(obj.object_id == g_plan[E_ZERO].object_id, "to the right object");
    expect(obj.size == 0u, "an empty file has no size");

    wfs_stub_teardown();
}

/* Dotdot must name the parent, which is what makes an upward walk possible. */
static void test_dotdot_names_the_parent(void) {
    wfs_object_ctx_t o;
    wfs_dir_ctx_t d;
    static const char* const etc[] = {"etc"};
    static const char* const k_nested[] = {"etc", "nested"};
    struct wfs_object etc_obj;
    struct wfs_object nested_obj;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    expect(resolve_path(etc, 1u, &etc_obj) == 0, "/etc resolves");
    expect(lookup(&d, &etc_obj, "..") == 0 && d.object_id == WFS_OBJECT_ROOT,
           "/etc/.. names the root");
    expect(lookup(&d, &etc_obj, ".") == 0 && d.object_id == g_plan[E_ETC].object_id,
           "/etc/. names /etc");

    /* And one level deeper, where the parent is not the root. */
    memset(&nested_obj, 0, sizeof(nested_obj));
    expect(resolve_path(k_nested, 2u, &nested_obj) == 0, "/etc/nested resolves");
    expect(lookup(&d, &nested_obj, "..") == 0 && d.object_id == g_plan[E_ETC].object_id,
           "/etc/nested/.. names /etc");
    (void)o;

    wfs_stub_teardown();
}

/* A file small enough to live in its own record costs no data block (§7). */
static void test_a_small_file_is_stored_inline(void) {
    static const char* const parts[] = {"etc", "small.conf"};
    struct wfs_object obj;
    wfs_object_ctx_t o;
    wfs_extent_ctx_t e;
    uint32_t i;
    int same;

    memset(&obj, 0, sizeof(obj));
    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    expect(resolve_path(parts, 2u, &obj) == 0, "/etc/small.conf resolves");
    expect((obj.flags & WFS_OBJ_INLINE_DATA) != 0u, "it is marked inline");
    expect(obj.extent_count == 0u, "with no extents");
    expect(obj.extent_tree_block == 0u, "and no tree");
    expect(g_plan[E_SMALL].block_count == 0u, "and mkfs gave it no blocks");

    /* The content comes back through the context's inline_data, which the object
     * reader keeps verbatim: the decoded extents array holds those same bytes
     * read as block numbers, which cannot be turned back into the file's. */
    expect(read_object(&o, g_plan[E_SMALL].object_id) == 0, "the record reads");
    same = 1;
    for (i = 0; i < SMALL_SIZE; ++i) {
        if (o.inline_data[i] != content_byte(5u, i)) {
            same = 0;
            break;
        }
    }
    expect(same, "and the bytes are the ones the formatter was handed");

    /* An inline object still resolves as a map: it answers from the record. */
    expect(map_block(&e, &obj, 0u) == 0 && e.found == 0u, "an inline object maps no logical block");

    wfs_stub_teardown();
}

static void test_an_empty_file_is_inline_and_empty(void) {
    static const char* const parts[] = {"etc", "nested", "zero"};
    struct wfs_object obj;

    memset(&obj, 0, sizeof(obj));

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    expect(resolve_path(parts, 3u, &obj) == 0, "the empty file resolves");
    expect(obj.size == 0u, "size zero");
    expect((obj.flags & WFS_OBJ_INLINE_DATA) != 0u, "an empty file takes the inline form");
    expect(obj.extent_count == 0u, "and has no extents");

    wfs_stub_teardown();
}

/* A file too big to inline gets one contiguous extent, and every logical block
 * must map to the right physical block with the right content. */
static void test_a_multi_block_file_maps_to_its_data(void) {
    static const char* const parts[] = {"bin", "big.bin"};
    struct wfs_object obj;

    memset(&obj, 0, sizeof(obj));
    wfs_extent_ctx_t e;
    uint32_t blocks = (BIG_SIZE + 4095u) / 4096u;
    uint32_t lb;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    expect(resolve_path(parts, 2u, &obj) == 0, "/bin/big.bin resolves");
    expect(obj.size == BIG_SIZE, "with its size");
    expect((obj.flags & WFS_OBJ_INLINE_DATA) == 0u, "not inline");
    expect(obj.extent_count == 1u, "one extent, because blocks are bump-allocated");
    expect(obj.extents[0].length == blocks, "covering every block of the file");

    for (lb = 0; lb < blocks; ++lb) {
        uint32_t expect_phys = (uint32_t)obj.extents[0].physical_block + lb;
        const uint8_t* on_disk;
        uint64_t base = (uint64_t)lb * 4096u;
        uint32_t i;
        int same = 1;

        if (map_block(&e, &obj, lb) != 0 || !e.found) {
            expect(0, "every logical block of the file maps");
            break;
        }
        if (e.physical != expect_phys) {
            expect(0, "and maps to the block the extent names");
            break;
        }
        /* Compare against what the read callback would have produced. */
        on_disk = wfs_stub_image + (size_t)e.physical * 4096u;
        for (i = 0; i < 4096u; ++i) {
            uint8_t want = (base + i) < BIG_SIZE ? content_byte(11u, base + i) : 0u;

            if (on_disk[i] != want) {
                same = 0;
                break;
            }
        }
        if (!same) {
            expect(0, "and holds the bytes the formatter was handed");
            break;
        }
    }
    expect(lb == blocks, "every block of the file checked out");

    /* Past the end of the file there is nothing mapped. */
    expect(map_block(&e, &obj, blocks) == 0 && e.found == 0u, "past the last block is a hole");
    /* The run reported from the first block covers the whole file, so a reader
     * can ask for it in one request. */
    expect(map_block(&e, &obj, 0u) == 0 && e.run == blocks, "the run spans the whole file");

    wfs_stub_teardown();
}

/* A directory's link count is its parent's record, its own dot, and one dotdot
 * per child (§17, §18). */
static void test_link_counts_reflect_the_tree(void) {
    wfs_object_ctx_t o;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    expect(read_object(&o, WFS_OBJECT_ROOT) == 0 && o.out.link_count == 2u,
           "the root's link count is 2");

    /* /etc holds small.conf and nested, so two children. */
    expect(read_object(&o, g_plan[E_ETC].object_id) == 0, "/etc's record reads");
    expect(o.out.link_count == 2u + 2u, "a directory with two children has four links");

    /* /etc/nested holds one. */
    expect(read_object(&o, g_plan[E_NESTED].object_id) == 0, "/etc/nested's record reads");
    expect(o.out.link_count == 2u + 1u, "a directory with one child has three");

    /* A file has the one name that points at it. */
    expect(read_object(&o, g_plan[E_README].object_id) == 0, "README's record reads");
    expect(o.out.link_count == 1u, "a file has one link");

    wfs_stub_teardown();
}

/* Walking the root enumerates dot, dotdot and exactly the top-level entries. */
static void test_walking_the_root_sees_what_was_placed(void) {
    wfs_object_ctx_t root;
    wfs_dir_ctx_t d;
    int saw_readme = 0;
    int saw_bin = 0;
    int saw_etc = 0;
    uint32_t n = 0;
    uint32_t i;

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    expect(read_object(&root, WFS_OBJECT_ROOT) == 0, "the root reads");

    wfs_dir_lookup_init(&d, &g_vol, &root.out, NULL, 0u);
    for (i = 0; i < 16u; ++i) {
        wasmos_wasm_coroutine_t task;

        if (wfs_stub_run_task(&task, wfs_dir_task, &d) != 0) {
            expect(0, "a walk step failed");
            break;
        }
        if (!d.found) {
            break;
        }
        n++;
        if (strcmp(d.name, "README") == 0) {
            saw_readme = 1;
        }
        if (strcmp(d.name, "bin") == 0) {
            saw_bin = 1;
        }
        if (strcmp(d.name, "etc") == 0) {
            saw_etc = 1;
        }
        d.pc = WFS_DIR_PC_SCAN;
        d.found = 0u;
    }
    /* dot, dotdot, README, bin, etc. */
    expect(n == 5u, "the root holds five records");
    expect(saw_readme && saw_bin && saw_etc, "and names every top-level entry");

    wfs_stub_teardown();
}

/* A directory with more entries than one block holds must span blocks, and every
 * name must still resolve — the packing the formatter used and the striding the
 * scan uses have to agree exactly. */
static void test_a_directory_larger_than_one_block(void) {
    static char names[200][16];
    static wfs_mkfs_entry_t entries[201];
    static wfs_mkfs_node_t plan[202];
    wfs_mkfs_params_t params;
    wfs_mkfs_sink_t sink;
    wfs_mkfs_layout_t layout;
    wfs_mount_ctx_t mctx;
    wasmos_wasm_coroutine_t task;
    wfs_object_ctx_t o;
    wfs_dir_ctx_t d;
    uint32_t i;
    int all = 1;

    /* One directory holding 200 files, whose records cannot fit in 4080 bytes. */
    memset(&entries[0], 0, sizeof(entries[0]));
    entries[0].name = "many";
    entries[0].name_len = 4u;
    entries[0].parent = WFS_MKFS_ROOT;
    entries[0].is_dir = 1u;
    entries[0].mode = 0755u;
    for (i = 0; i < 200u; ++i) {
        snprintf(names[i], sizeof(names[i]), "file-%03u", i);
        memset(&entries[i + 1u], 0, sizeof(entries[0]));
        entries[i + 1u].name = names[i];
        entries[i + 1u].name_len = (uint32_t)strlen(names[i]);
        entries[i + 1u].parent = 0u;
        entries[i + 1u].mode = 0644u;
    }

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, NOW_NS, &layout) != 0) {
        expect(0, "build");
        return;
    }
    memset(&params, 0, sizeof(params));
    params.size_bytes = VOL_16M;
    params.block_size = 4096u;
    params.now_ns = NOW_NS;
    memcpy(params.uuid, k_uuid, WFS_UUID_LEN);
    sink.ctx = NULL;
    sink.write_block = wfs_stub_sink_write;
    expect_rc(wfs_mkfs_format_tree(&params, entries, 201u, plan, &sink, &layout),
              WASMOS_ERR_NONE,
              "a 201-entry tree formats");
    wfs_stub_reset_counters();

    memset(&mctx, 0, sizeof(mctx));
    memset(&g_vol, 0, sizeof(g_vol));
    mctx.vol = &g_vol;
    expect(wfs_stub_run_task(&task, wfs_mount_task, &mctx) == 0, "and mounts");

    expect(read_object(&o, plan[0].object_id) == 0, "the big directory's record reads");
    expect(o.out.size > 4096u, "it spans more than one block");
    expect(o.out.link_count == 2u + 200u, "with a link per child");

    /* Every name must resolve, including the ones past the first block. */
    for (i = 0; i < 200u; ++i) {
        if (lookup(&d, &o.out, names[i]) != 0 || !d.found ||
            d.object_id != plan[i + 1u].object_id) {
            all = 0;
            break;
        }
    }
    expect(all, "every one of 200 names resolves to the object mkfs gave it");
    if (!all) {
        printf("       first failure at %s\n", names[i]);
    }

    wfs_stub_teardown();
}

/* Names go up to 255 bytes, stored inline in the record: `name_length` is one
 * byte and there is no short-name/long-name split to reassemble, unlike FAT. The
 * boundary is worth pinning because a 255-byte name needs a 272-byte record, and
 * an off-by-one in either the packing or the stride validation would land
 * exactly here. */
static void test_names_run_to_the_full_length(void) {
    static char longest[WFS_NAME_MAX + 1u];
    static char mid[130];
    static char toolong[WFS_NAME_MAX + 8u];
    static wfs_mkfs_entry_t entries[3];
    static wfs_mkfs_node_t plan[4];
    wfs_mkfs_params_t params;
    wfs_mkfs_sink_t sink;
    wfs_mkfs_layout_t layout;
    wfs_mount_ctx_t mctx;
    wasmos_wasm_coroutine_t task;
    wfs_object_ctx_t root;
    wfs_dir_ctx_t d;
    uint32_t i;

    for (i = 0; i < WFS_NAME_MAX; ++i) {
        longest[i] = (char)('a' + (i % 26u));
    }
    longest[WFS_NAME_MAX] = '\0';
    for (i = 0; i < sizeof(mid) - 1u; ++i) {
        mid[i] = (char)('A' + (i % 26u));
    }
    mid[sizeof(mid) - 1u] = '\0';

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, NOW_NS, &layout) != 0) {
        expect(0, "build");
        return;
    }
    memset(entries, 0, sizeof(entries));
    entries[0].name = longest;
    entries[0].name_len = WFS_NAME_MAX;
    entries[0].parent = WFS_MKFS_ROOT;
    entries[0].mode = 0644u;
    entries[1].name = mid;
    entries[1].name_len = (uint32_t)strlen(mid);
    entries[1].parent = WFS_MKFS_ROOT;
    entries[1].is_dir = 1u;
    entries[1].mode = 0755u;
    entries[2].name = "z";
    entries[2].name_len = 1u;
    entries[2].parent = 1u;
    entries[2].mode = 0644u;

    memset(&params, 0, sizeof(params));
    params.size_bytes = VOL_16M;
    params.block_size = 4096u;
    params.now_ns = NOW_NS;
    memcpy(params.uuid, k_uuid, WFS_UUID_LEN);
    sink.ctx = NULL;
    sink.write_block = wfs_stub_sink_write;
    expect_rc(wfs_mkfs_format_tree(&params, entries, 3u, plan, &sink, &layout),
              WASMOS_ERR_NONE,
              "a tree with a 255-byte name formats");
    wfs_stub_reset_counters();

    memset(&mctx, 0, sizeof(mctx));
    memset(&g_vol, 0, sizeof(g_vol));
    mctx.vol = &g_vol;
    expect(wfs_stub_run_task(&task, wfs_mount_task, &mctx) == 0, "and mounts");
    expect(read_object(&root, WFS_OBJECT_ROOT) == 0, "the root reads");

    expect(lookup(&d, &root.out, longest) == 0 && d.found == 1u, "the 255-byte name resolves");
    expect(d.object_id == plan[0].object_id, "to the object mkfs gave it");
    expect(d.name_length == WFS_NAME_MAX, "and comes back at full length");
    expect(strcmp(d.name, longest) == 0, "byte for byte");

    expect(lookup(&d, &root.out, mid) == 0 && d.object_id == plan[1].object_id,
           "a 129-byte directory name resolves");

    /* Truncating either name by one byte must NOT match, which is what proves the
     * comparison uses the whole length rather than a bounded prefix. */
    longest[WFS_NAME_MAX - 1u] = '\0';
    expect(lookup(&d, &root.out, longest) == 0 && d.found == 0u,
           "the same name one byte shorter does not match");

    /* And a name past the limit is refused rather than truncated into a
     * collision with a shorter one. */
    memset(toolong, 'q', sizeof(toolong) - 1u);
    toolong[sizeof(toolong) - 1u] = '\0';
    entries[0].name = toolong;
    entries[0].name_len = (uint32_t)strlen(toolong);
    expect_rc(wfs_mkfs_format_tree(&params, entries, 3u, plan, &sink, &layout),
              WASMOS_ERR_FS_NAME,
              "a name past 255 bytes is refused");

    wfs_stub_teardown();
}

/* ---- what the formatter must refuse ------------------------------------- */

static void test_bad_entries_are_refused(void) {
    wfs_mkfs_params_t params;
    wfs_mkfs_sink_t sink;
    wfs_mkfs_layout_t layout;
    wfs_mkfs_entry_t bad[2];
    wfs_mkfs_node_t plan[3];

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, NOW_NS, &layout) != 0) {
        expect(0, "build");
        return;
    }
    memset(&params, 0, sizeof(params));
    params.size_bytes = VOL_16M;
    params.block_size = 4096u;
    memcpy(params.uuid, k_uuid, WFS_UUID_LEN);
    sink.ctx = NULL;
    sink.write_block = wfs_stub_sink_write;

    memset(bad, 0, sizeof(bad));
    bad[0].name = "ok";
    bad[0].name_len = 2u;
    bad[0].parent = WFS_MKFS_ROOT;
    bad[0].is_dir = 1u;

    /* A name with a separator in it would be two components, not one. */
    bad[1].name = "a/b";
    bad[1].name_len = 3u;
    bad[1].parent = WFS_MKFS_ROOT;
    expect_rc(wfs_mkfs_format_tree(&params, bad, 2u, plan, &sink, &layout),
              WASMOS_ERR_FS_NAME,
              "a name containing a separator");

    /* Dot and dotdot are the records every directory already carries. */
    bad[1].name = "..";
    bad[1].name_len = 2u;
    expect_rc(wfs_mkfs_format_tree(&params, bad, 2u, plan, &sink, &layout),
              WASMOS_ERR_FS_NAME,
              "an entry claiming to be dotdot");

    bad[1].name = "fine";
    bad[1].name_len = 4u;

    /* A parent must be a directory declared EARLIER, which is what makes one
     * pass enough and rules out a cycle. */
    bad[1].parent = 1u;
    expect_rc(wfs_mkfs_format_tree(&params, bad, 2u, plan, &sink, &layout),
              WASMOS_ERR_FS_CORRUPT,
              "an entry parented to itself");

    bad[1].parent = 0u;
    bad[0].is_dir = 0u;
    expect_rc(wfs_mkfs_format_tree(&params, bad, 2u, plan, &sink, &layout),
              WASMOS_ERR_FS_CORRUPT,
              "an entry parented to a file");

    wfs_stub_teardown();
}

static void test_a_tree_too_big_for_the_volume_is_refused(void) {
    static wfs_mkfs_entry_t big[2];
    static wfs_mkfs_node_t plan[3];
    static gen_t gen;
    wfs_mkfs_params_t params;
    wfs_mkfs_sink_t sink;
    wfs_mkfs_layout_t layout;

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, NOW_NS, &layout) != 0) {
        expect(0, "build");
        return;
    }
    memset(&params, 0, sizeof(params));
    params.size_bytes = VOL_16M;
    params.block_size = 4096u;
    memcpy(params.uuid, k_uuid, WFS_UUID_LEN);
    sink.ctx = NULL;
    sink.write_block = wfs_stub_sink_write;

    memset(big, 0, sizeof(big));
    big[0].name = "huge";
    big[0].name_len = 4u;
    big[0].parent = WFS_MKFS_ROOT;
    big[0].size = VOL_16M; /* the whole volume, metadata and all */
    gen.seed = 1u;
    gen.size = VOL_16M;
    big[0].read = gen_read;
    big[0].read_ctx = &gen;

    expect_rc(wfs_mkfs_format_tree(&params, big, 1u, plan, &sink, &layout),
              WASMOS_ERR_FS_NO_SPACE,
              "a file larger than the free space");

    wfs_stub_teardown();
}

/* ---- runner -------------------------------------------------------------- */

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_a_populated_volume_mounts),
    WASMOS_TEST_CASE(test_every_top_level_name_resolves),
    WASMOS_TEST_CASE(test_a_nested_path_resolves_component_by_component),
    WASMOS_TEST_CASE(test_dotdot_names_the_parent),
    WASMOS_TEST_CASE(test_a_small_file_is_stored_inline),
    WASMOS_TEST_CASE(test_an_empty_file_is_inline_and_empty),
    WASMOS_TEST_CASE(test_a_multi_block_file_maps_to_its_data),
    WASMOS_TEST_CASE(test_link_counts_reflect_the_tree),
    WASMOS_TEST_CASE(test_walking_the_root_sees_what_was_placed),
    WASMOS_TEST_CASE(test_a_directory_larger_than_one_block),
    WASMOS_TEST_CASE(test_names_run_to_the_full_length),
    WASMOS_TEST_CASE(test_bad_entries_are_refused),
    WASMOS_TEST_CASE(test_a_tree_too_big_for_the_volume_is_refused),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_populate: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_populate: %d checks passed\n", g_checks);
    return 0;
}
