/* test_fs_manager_path.c — mount routing for the FS manager
 * (fs_manager_path.h): which mount a path belongs to, and what is left of the
 * path once that mount's name is stripped.
 *
 * src/services/fs_manager/fs_manager_path.c is the only source linked in. It is
 * split out of fs_manager.c precisely because it touches no IPC, no xfer buffer
 * and no backend table, so nothing is stubbed and the mount list is passed in
 * per call.
 *
 * fsmgr_route_path_for_mounts returns 1 on a match and 0 otherwise -- the
 * opposite polarity to the kernel's 0-on-success convention -- and leaves its
 * outputs untouched when it returns 0, which is why the cases assert on `ok`
 * before reading anything else.
 *
 * The cases report through assert(), not through a failure counter: the first
 * failure aborts the process, and main() prints "ok" only if every case ran to
 * completion. The suite is compiled without -DNDEBUG, which those asserts
 * depend on.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "fs_manager_path.h"

/* Route a path and turn the mount index into that mount's backend endpoint,
 * mirroring route_path_to_backend in src/services/fs_manager/fs_manager.c
 * (including its re-check of the returned index against mount_count).
 *
 * `mounts` and `backends` are parallel arrays of `mount_count` entries, borrowed
 * for the call; the real function builds them by walking its live g_backends
 * registration table instead, and refuses outright when no backend is
 * registered, which this composition cannot reach.
 *
 * Returns 1 on a routed path, with *out_backend, out_path and *out_path_len set;
 * returns 0 otherwise, leaving *out_backend untouched. */
static int32_t route_and_select_backend(const char* path, int32_t path_len,
                                        const char* const* mounts, const int32_t* backends,
                                        int32_t mount_count, int32_t allow_relative,
                                        int32_t* out_backend, char* out_path, int32_t out_path_cap,
                                        int32_t* out_path_len) {
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts(path,
                                             path_len,
                                             mounts,
                                             mount_count,
                                             allow_relative,
                                             &mount_idx,
                                             out_path,
                                             out_path_cap,
                                             out_path_len);
    if (!ok) {
        return 0;
    }
    if (mount_idx < 0 || mount_idx >= mount_count) {
        return 0;
    }
    *out_backend = backends[mount_idx];
    return 1;
}

/* "/" carries no mount segment after the leading slash, so it routes to no
 * backend at all: the root is not itself a mount. */
static void test_absolute_root_path_matches_boot(void) {
    const char* mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts(
        "/", 1, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 0);
}

static void test_absolute_boot_path_is_routed_and_trimmed(void) {
    const char* mounts[] = {"fatfs", "boot", "initfs", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts(
        "/boot/xyz", 9, mounts, 4, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 1);
    assert(out_len == 4);
    assert(strcmp(out, "/xyz") == 0);
}

static void test_absolute_init_path_is_routed_and_trimmed(void) {
    const char* mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts(
        "/init/xyz", 9, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 1);
    assert(out_len == 4);
    assert(strcmp(out, "/xyz") == 0);
}

static void test_absolute_mount_path_without_tail_routes_to_root(void) {
    const char* mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts(
        "/boot", 5, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 0);
    assert(out_len == 1);
    assert(strcmp(out, "/") == 0);
}

static void test_relative_boot_path_is_routed_and_trimmed(void) {
    const char* mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts(
        "boot/xyz", 8, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 0);
    assert(out_len == 4);
    assert(strcmp(out, "/xyz") == 0);
}

static void test_relative_mount_path_without_tail_routes_to_root(void) {
    const char* mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts(
        "boot", 4, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 0);
    assert(out_len == 1);
    assert(strcmp(out, "/") == 0);
}

static void test_unknown_mount_is_not_routed(void) {
    const char* mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts(
        "/user/xyz", 9, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 0);
}

static void test_case_insensitive_mount_match(void) {
    const char* mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts(
        "/BOOT/xyz", 9, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 0);
    assert(strcmp(out, "/xyz") == 0);
}

static void test_prefix_collision_does_not_match_mount(void) {
    const char* mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts(
        "/bootx/xyz", 10, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 0);
}

static void test_double_slash_tail_is_preserved(void) {
    const char* mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts(
        "/boot//xyz", 10, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 0);
    assert(out_len == 5);
    assert(strcmp(out, "//xyz") == 0);
}

static void test_relative_is_rejected_when_disallowed(void) {
    const char* mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts(
        "boot/xyz", 8, mounts, 2, 0, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 0);
}

static void test_relative_non_mount_falls_through(void) {
    const char* mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts(
        "foo/bar", 7, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 0);
}

static void test_mount_only_variants_map_to_root(void) {
    const char* mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;

    int32_t ok = fsmgr_route_path_for_mounts(
        "/boot/", 6, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(strcmp(out, "/") == 0);

    ok = fsmgr_route_path_for_mounts(
        "boot/", 5, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(strcmp(out, "/") == 0);

    ok = fsmgr_route_path_for_mounts(
        "/init/", 6, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(strcmp(out, "/") == 0);
}

static void test_out_buffer_size_boundaries(void) {
    const char* mounts[] = {"boot"};
    char out_exact[5];
    char out_small[4];
    int32_t out_len = 0;
    int32_t mount_idx = -1;

    int32_t ok = fsmgr_route_path_for_mounts(
        "/boot/xyz", 9, mounts, 1, 1, &mount_idx, out_exact, (int32_t)sizeof(out_exact), &out_len);
    assert(ok == 1);
    assert(out_len == 4);
    assert(strcmp(out_exact, "/xyz") == 0);

    ok = fsmgr_route_path_for_mounts(
        "/boot/xyz", 9, mounts, 1, 1, &mount_idx, out_small, (int32_t)sizeof(out_small), &out_len);
    assert(ok == 0);
}

static void test_invalid_inputs_are_rejected(void) {
    const char* mounts[] = {"boot"};
    char out[8];
    int32_t out_len = 0;
    int32_t mount_idx = -1;

    assert(fsmgr_route_path_for_mounts(
               "/boot", 0, mounts, 1, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len) == 0);
    assert(fsmgr_route_path_for_mounts(
               "", 0, mounts, 1, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len) == 0);
    assert(fsmgr_route_path_for_mounts(
               "/boot", 5, mounts, 0, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len) == 0);
    assert(fsmgr_route_path_for_mounts("/boot", 5, mounts, 1, 1, &mount_idx, out, 1, &out_len) ==
           0);
}

static void test_null_mount_entries_are_skipped(void) {
    const char* mounts[] = {0, "boot"};
    char out[16];
    int32_t out_len = 0;
    int32_t mount_idx = -1;

    int32_t ok = fsmgr_route_path_for_mounts(
        "/boot/xyz", 9, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 1);
    assert(strcmp(out, "/xyz") == 0);
}

static void test_duplicate_mount_names_use_first_match(void) {
    const char* mounts[] = {"boot", "boot"};
    char out[16];
    int32_t out_len = 0;
    int32_t mount_idx = -1;

    int32_t ok = fsmgr_route_path_for_mounts(
        "/boot/xyz", 9, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 0);
}

static void test_backend_selection_absolute_boot(void) {
    const char* mounts[] = {"boot", "init"};
    const int32_t backends[] = {101, 202};
    const char* path = "/boot/system/fonts/roboto.ttf";
    char out[64];
    int32_t out_len = 0;
    int32_t backend = -1;
    int32_t ok = route_and_select_backend(path,
                                          (int32_t)strlen(path),
                                          mounts,
                                          backends,
                                          2,
                                          1,
                                          &backend,
                                          out,
                                          (int32_t)sizeof(out),
                                          &out_len);
    assert(ok == 1);
    assert(backend == 101);
    assert(strcmp(out, "/system/fonts/roboto.ttf") == 0);
}

static void test_backend_selection_absolute_init(void) {
    const char* mounts[] = {"boot", "init"};
    const int32_t backends[] = {101, 202};
    const char* path = "/init/devmgr/rules/default.rules";
    char out[64];
    int32_t out_len = 0;
    int32_t backend = -1;
    int32_t ok = route_and_select_backend(path,
                                          (int32_t)strlen(path),
                                          mounts,
                                          backends,
                                          2,
                                          1,
                                          &backend,
                                          out,
                                          (int32_t)sizeof(out),
                                          &out_len);
    assert(ok == 1);
    assert(backend == 202);
    assert(strcmp(out, "/devmgr/rules/default.rules") == 0);
}

static void test_backend_selection_relative_boot(void) {
    const char* mounts[] = {"boot", "init"};
    const int32_t backends[] = {101, 202};
    const char* path = "boot/apps/hello.wap";
    char out[64];
    int32_t out_len = 0;
    int32_t backend = -1;
    int32_t ok = route_and_select_backend(path,
                                          (int32_t)strlen(path),
                                          mounts,
                                          backends,
                                          2,
                                          1,
                                          &backend,
                                          out,
                                          (int32_t)sizeof(out),
                                          &out_len);
    assert(ok == 1);
    assert(backend == 101);
    assert(strcmp(out, "/apps/hello.wap") == 0);
}

static void test_backend_selection_unknown_mount_fails(void) {
    const char* mounts[] = {"boot", "init"};
    const int32_t backends[] = {101, 202};
    const char* path = "/user/docs/readme.txt";
    char out[64];
    int32_t out_len = 0;
    int32_t backend = -1;
    int32_t ok = route_and_select_backend(path,
                                          (int32_t)strlen(path),
                                          mounts,
                                          backends,
                                          2,
                                          1,
                                          &backend,
                                          out,
                                          (int32_t)sizeof(out),
                                          &out_len);
    assert(ok == 0);
    assert(backend == -1);
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_absolute_root_path_matches_boot),
        WASMOS_TEST_CASE(test_absolute_boot_path_is_routed_and_trimmed),
        WASMOS_TEST_CASE(test_absolute_init_path_is_routed_and_trimmed),
        WASMOS_TEST_CASE(test_absolute_mount_path_without_tail_routes_to_root),
        WASMOS_TEST_CASE(test_relative_boot_path_is_routed_and_trimmed),
        WASMOS_TEST_CASE(test_relative_mount_path_without_tail_routes_to_root),
        WASMOS_TEST_CASE(test_unknown_mount_is_not_routed),
        WASMOS_TEST_CASE(test_case_insensitive_mount_match),
        WASMOS_TEST_CASE(test_prefix_collision_does_not_match_mount),
        WASMOS_TEST_CASE(test_double_slash_tail_is_preserved),
        WASMOS_TEST_CASE(test_relative_is_rejected_when_disallowed),
        WASMOS_TEST_CASE(test_relative_non_mount_falls_through),
        WASMOS_TEST_CASE(test_mount_only_variants_map_to_root),
        WASMOS_TEST_CASE(test_out_buffer_size_boundaries),
        WASMOS_TEST_CASE(test_invalid_inputs_are_rejected),
        WASMOS_TEST_CASE(test_null_mount_entries_are_skipped),
        WASMOS_TEST_CASE(test_duplicate_mount_names_use_first_match),
        WASMOS_TEST_CASE(test_backend_selection_absolute_boot),
        WASMOS_TEST_CASE(test_backend_selection_absolute_init),
        WASMOS_TEST_CASE(test_backend_selection_relative_boot),
        WASMOS_TEST_CASE(test_backend_selection_unknown_mount_fails),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_fs_manager_path: ok\n");
    return 0;
}
