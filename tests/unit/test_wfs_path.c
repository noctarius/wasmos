/* Host unit test for path resolution (src/drivers/fs_wfs/wfs_path.c), against
 * volumes mkfs_wfs populated.
 *
 * The walk is what an open or a stat begins with, so its edge cases are the ones
 * a client will hit first: a bare root, redundant separators, a trailing slash,
 * dot and dotdot, a component under a file, and a path that is simply not there.
 */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "stubs_wfs_block_server.h"
#include "wasmos_status.h"
#include "wfs_format.h"
#include "wfs_mount.h"
#include "wfs_path.h"

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
    0x4d, 0x13, 0x7a, 0x2e, 0x88, 0x05, 0x4b, 0x6c, 0xa2, 0x39, 0xe1, 0x74, 0x9f, 0x2b, 0xc0, 0x81};
#define NOW_NS 1750000000000000000ull
#define VOL_16M (16ull * 1024ull * 1024ull)

/* /README  /etc/conf  /etc/sub/deep */
enum { E_README = 0, E_ETC, E_CONF, E_SUB, E_DEEP, E_COUNT };

static wfs_mkfs_entry_t g_entries[E_COUNT];
static wfs_mkfs_node_t g_plan[E_COUNT + 1u];
static wfs_mkfs_layout_t g_layout;
static wfs_volume_t g_vol;

static int one_read(void* ctx, uint64_t offset, void* dst, uint32_t len) {
    (void)ctx;
    (void)offset;
    memset(dst, 0xAB, len);
    return 0;
}

static void add(uint32_t idx, const char* name, uint32_t parent, int is_dir, uint64_t size) {
    memset(&g_entries[idx], 0, sizeof(g_entries[idx]));
    g_entries[idx].name = name;
    g_entries[idx].name_len = (uint32_t)strlen(name);
    g_entries[idx].parent = parent;
    g_entries[idx].is_dir = (uint8_t)(is_dir ? 1 : 0);
    g_entries[idx].mode = is_dir ? 0755u : 0644u;
    if (!is_dir) {
        g_entries[idx].size = size;
        g_entries[idx].read = one_read;
        g_entries[idx].read_ctx = NULL;
    }
}

static int setup(void) {
    wfs_mkfs_params_t params;
    wfs_mkfs_sink_t sink;
    wfs_mount_ctx_t ctx;
    wasmos_wasm_coroutine_t task;

    add(E_README, "README", WFS_MKFS_ROOT, 0, 20u);
    add(E_ETC, "etc", WFS_MKFS_ROOT, 1, 0u);
    add(E_CONF, "conf", E_ETC, 0, 30u);
    add(E_SUB, "sub", E_ETC, 1, 0u);
    add(E_DEEP, "deep", E_SUB, 0, 40u);

    if (wfs_stub_build_volume(VOL_16M, 4096u, k_uuid, NOW_NS, &g_layout) != 0) {
        return -1;
    }
    memset(&params, 0, sizeof(params));
    params.size_bytes = VOL_16M;
    params.block_size = 4096u;
    params.now_ns = NOW_NS;
    memcpy(params.uuid, k_uuid, WFS_UUID_LEN);
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

static wfs_path_ctx_t g_ctx;

/* Resolve `path`. Returns the task status; the outcome is in g_ctx. */
static int32_t resolve(const char* path) {
    wasmos_wasm_coroutine_t task;
    wasmos_error_code_t rc;

    memset(&g_ctx, 0, sizeof(g_ctx));
    rc = wfs_path_init(&g_ctx, &g_vol, path, (uint32_t)strlen(path));
    if (rc != WASMOS_ERR_NONE) {
        return (int32_t)rc;
    }
    return wfs_stub_run_task(&task, wfs_path_task, &g_ctx);
}

/* ---- what must resolve -------------------------------------------------- */

static void test_the_root_resolves_to_itself(void) {
    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    expect(resolve("/") == 0 && g_ctx.found == 1u, "\"/\" resolves");
    expect(g_ctx.object_id == WFS_OBJECT_ROOT, "to the root object");
    expect(g_ctx.object.out.type == WFS_TYPE_DIR, "which is a directory");

    wfs_stub_teardown();
}

static void test_paths_resolve_at_every_depth(void) {
    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    expect(resolve("/README") == 0 && g_ctx.object_id == g_plan[E_README].object_id,
           "a top-level file");
    expect(g_ctx.object.out.size == 20u, "with its size");

    expect(resolve("/etc") == 0 && g_ctx.object_id == g_plan[E_ETC].object_id,
           "a top-level directory");
    expect(resolve("/etc/conf") == 0 && g_ctx.object_id == g_plan[E_CONF].object_id,
           "two levels down");
    expect(resolve("/etc/sub/deep") == 0 && g_ctx.object_id == g_plan[E_DEEP].object_id,
           "three levels down");
    expect(g_ctx.object.out.size == 40u, "with its size");

    wfs_stub_teardown();
}

/* Redundant separators name the same object, so a caller need not normalise. */
static void test_redundant_separators_are_harmless(void) {
    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    expect(resolve("//etc") == 0 && g_ctx.object_id == g_plan[E_ETC].object_id,
           "a doubled leading separator");
    expect(resolve("/etc///conf") == 0 && g_ctx.object_id == g_plan[E_CONF].object_id,
           "separators between components");
    expect(resolve("/etc/") == 0 && g_ctx.object_id == g_plan[E_ETC].object_id,
           "a trailing separator on a directory");
    expect(resolve("/etc/sub/") == 0 && g_ctx.object_id == g_plan[E_SUB].object_id,
           "and one deeper");
    expect(resolve("///") == 0 && g_ctx.object_id == WFS_OBJECT_ROOT,
           "a path of nothing but separators is the root");

    wfs_stub_teardown();
}

/* Dot and dotdot resolve through the records a directory carries, so they need
 * no special case in the walk — and dotdot from the root stays at the root, which
 * is what stops a client escaping the volume. */
static void test_dot_and_dotdot_resolve_through_the_records(void) {
    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    expect(resolve("/etc/.") == 0 && g_ctx.object_id == g_plan[E_ETC].object_id, "a trailing dot");
    expect(resolve("/etc/..") == 0 && g_ctx.object_id == WFS_OBJECT_ROOT, "dotdot from /etc");
    expect(resolve("/etc/sub/../conf") == 0 && g_ctx.object_id == g_plan[E_CONF].object_id,
           "dotdot in the middle of a path");
    expect(resolve("/..") == 0 && g_ctx.object_id == WFS_OBJECT_ROOT,
           "dotdot from the root stays at the root");
    expect(resolve("/../../..") == 0 && g_ctx.object_id == WFS_OBJECT_ROOT,
           "and cannot be walked out of the volume");
    expect(resolve("/./etc/./conf") == 0 && g_ctx.object_id == g_plan[E_CONF].object_id,
           "dots between components");

    wfs_stub_teardown();
}

/* ---- what must not ------------------------------------------------------ */

/* A path that is not there is NOT a failure: an open that misses is how a create
 * learns it may proceed. */
static void test_a_missing_path_reports_not_found_without_failing(void) {
    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    expect(resolve("/nope") == 0 && g_ctx.found == 0u, "a missing top-level name");
    expect(resolve("/etc/nope") == 0 && g_ctx.found == 0u, "a missing name in a subdirectory");
    expect(resolve("/nope/deeper") == 0 && g_ctx.found == 0u, "a name under a missing directory");
    expect(resolve("/etc/sub/nope") == 0 && g_ctx.found == 0u, "a miss three levels down");

    wfs_stub_teardown();
}

/* A component under a file cannot resolve, and saying NOT_DIR beats reporting
 * the component missing: the two send a caller to different places. */
static void test_a_component_under_a_file_is_not_a_directory(void) {
    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    expect_rc((wasmos_error_code_t)resolve("/README/x"),
              WASMOS_ERR_FS_NOT_DIR,
              "a component under a file");
    expect_rc((wasmos_error_code_t)resolve("/etc/conf/x"), WASMOS_ERR_FS_NOT_DIR, "and one deeper");
    /* A trailing separator on a FILE is still the file: there is no component
     * after it to resolve. */
    expect(resolve("/README/") == 0 && g_ctx.object_id == g_plan[E_README].object_id,
           "a trailing separator on a file is the file");

    wfs_stub_teardown();
}

static void test_paths_the_walk_will_not_accept(void) {
    wfs_path_ctx_t ctx;
    static char toolong[WFS_PATH_MAX + 8u];

    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    /* Relative paths are the caller's to root: the driver has no working
     * directory to resolve one against. */
    expect_rc(
        wfs_path_init(&ctx, &g_vol, "etc/conf", 8u), WASMOS_ERR_FS_NOT_ABSOLUTE, "a relative path");
    expect_rc(wfs_path_init(&ctx, &g_vol, "", 0u), WASMOS_ERR_FS_NOT_ABSOLUTE, "an empty path");

    memset(toolong, 'a', sizeof(toolong) - 1u);
    toolong[0] = '/';
    toolong[sizeof(toolong) - 1u] = '\0';
    expect_rc(wfs_path_init(&ctx, &g_vol, toolong, (uint32_t)strlen(toolong)),
              WASMOS_ERR_FS_PATH_TOO_LONG,
              "a path past the limit");

    wfs_stub_teardown();
}

/* The record of whatever the path named comes back with it, so a caller does not
 * fetch it again — including the inline bytes, which the decoded record cannot
 * supply. */
static void test_the_resolved_object_comes_back_with_its_record(void) {
    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    expect(resolve("/etc/conf") == 0, "/etc/conf resolves");
    expect(g_ctx.object.out.object_id == g_plan[E_CONF].object_id, "the record is the object's");
    expect(g_ctx.object.out.type == WFS_TYPE_FILE, "typed as a file");
    expect(g_ctx.object.out.size == 30u, "with its size");
    /* 30 bytes fits a record, so mkfs stored it inline and the bytes came with
     * it. */
    expect((g_ctx.object.out.flags & WFS_OBJ_INLINE_DATA) != 0u, "stored inline");
    expect(g_ctx.object.inline_data[0] == 0xABu, "and its bytes came back too");

    wfs_stub_teardown();
}

static void test_a_device_error_fails_the_walk(void) {
    if (setup() != 0) {
        expect(0, "setup");
        return;
    }
    wfs_stub_fail_next = 1;
    expect_rc((wasmos_error_code_t)resolve("/etc/conf"),
              WASMOS_ERR_FS_IO,
              "a failed transfer fails the walk");

    wfs_stub_teardown();
}

/* ---- runner -------------------------------------------------------------- */

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_the_root_resolves_to_itself),
    WASMOS_TEST_CASE(test_paths_resolve_at_every_depth),
    WASMOS_TEST_CASE(test_redundant_separators_are_harmless),
    WASMOS_TEST_CASE(test_dot_and_dotdot_resolve_through_the_records),
    WASMOS_TEST_CASE(test_a_missing_path_reports_not_found_without_failing),
    WASMOS_TEST_CASE(test_a_component_under_a_file_is_not_a_directory),
    WASMOS_TEST_CASE(test_paths_the_walk_will_not_accept),
    WASMOS_TEST_CASE(test_the_resolved_object_comes_back_with_its_record),
    WASMOS_TEST_CASE(test_a_device_error_fails_the_walk),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_path: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_path: %d checks passed\n", g_checks);
    return 0;
}
