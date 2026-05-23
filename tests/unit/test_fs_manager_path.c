#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fs_manager_path.h"

static int32_t
route_and_select_backend(const char *path,
                         int32_t path_len,
                         const char *const *mounts,
                         const int32_t *backends,
                         int32_t mount_count,
                         int32_t allow_relative,
                         int32_t *out_backend,
                         char *out_path,
                         int32_t out_path_cap,
                         int32_t *out_path_len)
{
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

static void
test_absolute_root_path_matches_boot(void)
{
    const char *mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts("/", 1, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 0);
}

static void
test_absolute_boot_path_is_routed_and_trimmed(void)
{
    const char *mounts[] = {"fatfs", "boot", "initfs", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts("/boot/xyz", 9, mounts, 4, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 1);
    assert(out_len == 4);
    assert(strcmp(out, "/xyz") == 0);
}

static void
test_absolute_init_path_is_routed_and_trimmed(void)
{
    const char *mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts("/init/xyz", 9, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 1);
    assert(out_len == 4);
    assert(strcmp(out, "/xyz") == 0);
}

static void
test_absolute_mount_path_without_tail_routes_to_root(void)
{
    const char *mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts("/boot", 5, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 0);
    assert(out_len == 1);
    assert(strcmp(out, "/") == 0);
}

static void
test_relative_boot_path_is_routed_and_trimmed(void)
{
    const char *mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts("boot/xyz", 8, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 0);
    assert(out_len == 4);
    assert(strcmp(out, "/xyz") == 0);
}

static void
test_relative_mount_path_without_tail_routes_to_root(void)
{
    const char *mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts("boot", 4, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 0);
    assert(out_len == 1);
    assert(strcmp(out, "/") == 0);
}

static void
test_unknown_mount_is_not_routed(void)
{
    const char *mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts("/user/xyz", 9, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 0);
}

static void
test_case_insensitive_mount_match(void)
{
    const char *mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts("/BOOT/xyz", 9, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 0);
    assert(strcmp(out, "/xyz") == 0);
}

static void
test_prefix_collision_does_not_match_mount(void)
{
    const char *mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts("/bootx/xyz", 10, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 0);
}

static void
test_double_slash_tail_is_preserved(void)
{
    const char *mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts("/boot//xyz", 10, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 0);
    assert(out_len == 5);
    assert(strcmp(out, "//xyz") == 0);
}

static void
test_relative_is_rejected_when_disallowed(void)
{
    const char *mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts("boot/xyz", 8, mounts, 2, 0, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 0);
}

static void
test_relative_non_mount_falls_through(void)
{
    const char *mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;
    int32_t ok = fsmgr_route_path_for_mounts("foo/bar", 7, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 0);
}

static void
test_mount_only_variants_map_to_root(void)
{
    const char *mounts[] = {"boot", "init"};
    char out[64];
    int32_t out_len = 0;
    int32_t mount_idx = -1;

    int32_t ok = fsmgr_route_path_for_mounts("/boot/", 6, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(strcmp(out, "/") == 0);

    ok = fsmgr_route_path_for_mounts("boot/", 5, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(strcmp(out, "/") == 0);

    ok = fsmgr_route_path_for_mounts("/init/", 6, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(strcmp(out, "/") == 0);
}

static void
test_out_buffer_size_boundaries(void)
{
    const char *mounts[] = {"boot"};
    char out_exact[5];
    char out_small[4];
    int32_t out_len = 0;
    int32_t mount_idx = -1;

    int32_t ok = fsmgr_route_path_for_mounts("/boot/xyz", 9, mounts, 1, 1, &mount_idx, out_exact, (int32_t)sizeof(out_exact), &out_len);
    assert(ok == 1);
    assert(out_len == 4);
    assert(strcmp(out_exact, "/xyz") == 0);

    ok = fsmgr_route_path_for_mounts("/boot/xyz", 9, mounts, 1, 1, &mount_idx, out_small, (int32_t)sizeof(out_small), &out_len);
    assert(ok == 0);
}

static void
test_invalid_inputs_are_rejected(void)
{
    const char *mounts[] = {"boot"};
    char out[8];
    int32_t out_len = 0;
    int32_t mount_idx = -1;

    assert(fsmgr_route_path_for_mounts("/boot", 0, mounts, 1, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len) == 0);
    assert(fsmgr_route_path_for_mounts("", 0, mounts, 1, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len) == 0);
    assert(fsmgr_route_path_for_mounts("/boot", 5, mounts, 0, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len) == 0);
    assert(fsmgr_route_path_for_mounts("/boot", 5, mounts, 1, 1, &mount_idx, out, 1, &out_len) == 0);
}

static void
test_null_mount_entries_are_skipped(void)
{
    const char *mounts[] = {0, "boot"};
    char out[16];
    int32_t out_len = 0;
    int32_t mount_idx = -1;

    int32_t ok = fsmgr_route_path_for_mounts("/boot/xyz", 9, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 1);
    assert(strcmp(out, "/xyz") == 0);
}

static void
test_duplicate_mount_names_use_first_match(void)
{
    const char *mounts[] = {"boot", "boot"};
    char out[16];
    int32_t out_len = 0;
    int32_t mount_idx = -1;

    int32_t ok = fsmgr_route_path_for_mounts("/boot/xyz", 9, mounts, 2, 1, &mount_idx, out, (int32_t)sizeof(out), &out_len);
    assert(ok == 1);
    assert(mount_idx == 0);
}

static void
test_backend_selection_absolute_boot(void)
{
    const char *mounts[] = {"boot", "init"};
    const int32_t backends[] = {101, 202};
    const char *path = "/boot/system/fonts/roboto.ttf";
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

static void
test_backend_selection_absolute_init(void)
{
    const char *mounts[] = {"boot", "init"};
    const int32_t backends[] = {101, 202};
    const char *path = "/init/devmgr/rules/default.rules";
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

static void
test_backend_selection_relative_boot(void)
{
    const char *mounts[] = {"boot", "init"};
    const int32_t backends[] = {101, 202};
    const char *path = "boot/apps/hello.wap";
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

static void
test_backend_selection_unknown_mount_fails(void)
{
    const char *mounts[] = {"boot", "init"};
    const int32_t backends[] = {101, 202};
    const char *path = "/user/docs/readme.txt";
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

int
main(void)
{
    test_absolute_root_path_matches_boot();
    test_absolute_boot_path_is_routed_and_trimmed();
    test_absolute_init_path_is_routed_and_trimmed();
    test_absolute_mount_path_without_tail_routes_to_root();
    test_relative_boot_path_is_routed_and_trimmed();
    test_relative_mount_path_without_tail_routes_to_root();
    test_unknown_mount_is_not_routed();
    test_case_insensitive_mount_match();
    test_prefix_collision_does_not_match_mount();
    test_double_slash_tail_is_preserved();
    test_relative_is_rejected_when_disallowed();
    test_relative_non_mount_falls_through();
    test_mount_only_variants_map_to_root();
    test_out_buffer_size_boundaries();
    test_invalid_inputs_are_rejected();
    test_null_mount_entries_are_skipped();
    test_duplicate_mount_names_use_first_match();
    test_backend_selection_absolute_boot();
    test_backend_selection_absolute_init();
    test_backend_selection_relative_boot();
    test_backend_selection_unknown_mount_fails();
    printf("test_fs_manager_path: ok\n");
    return 0;
}
