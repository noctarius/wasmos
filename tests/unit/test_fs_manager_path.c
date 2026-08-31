/* test_fs_manager_path.c — mount routing for the FS manager
 * (fs_manager_path.h): which mount owns a path, and what is left of the path
 * once that mount is stripped.
 *
 * src/services/fs_manager/fs_manager_path.c is the only source linked in. It is
 * split out of fs_manager.c precisely because it touches no IPC, no xfer buffer
 * and no backend table, so nothing is stubbed and the mount list is passed in
 * per call.
 *
 * A mount is an absolute canonical PATH ("/", "/boot", "/mnt/usb"), not a
 * top-level name, and the match is the longest such path that prefixes the
 * request on a whole-segment boundary. "/" therefore prefixes everything and is
 * the mount of last resort rather than a case of its own.
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

/* Route `path` and report the mount index plus the tail, so a case reads as one
 * call rather than five out-parameters. Returns the function's own polarity. */
static int32_t route(const char* path, const char* const* mounts, int32_t mount_count,
                     int32_t* out_mount_index, char* out, int32_t out_cap, int32_t* out_len) {
    return fsmgr_route_path_for_mounts(
        path, (int32_t)strlen(path), mounts, mount_count, out_mount_index, out, out_cap, out_len);
}

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
static int32_t route_and_select_backend(const char* path, const char* const* mounts,
                                        const int32_t* backends, int32_t mount_count,
                                        int32_t* out_backend, char* out_path, int32_t out_path_cap,
                                        int32_t* out_path_len) {
    int32_t mount_idx = -1;
    if (!route(path, mounts, mount_count, &mount_idx, out_path, out_path_cap, out_path_len)) {
        return 0;
    }
    if (mount_idx < 0 || mount_idx >= mount_count) {
        return 0;
    }
    *out_backend = backends[mount_idx];
    return 1;
}

/* --- the root mount ------------------------------------------------------- */

/* "/" prefixes every absolute path, so a system with a root filesystem routes
 * everything somewhere. This is what makes a path that names no other mount
 * reachable at all: before the root was a mount, it was NOT_FOUND. */
static void test_root_mount_owns_every_path(void) {
    const char* mounts[] = {"/"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;

    assert(route("/", mounts, 1, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 0);
    assert(strcmp(out, "/") == 0);
    assert(out_len == 1);

    assert(route("/foo/bar", mounts, 1, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 0);
    assert(strcmp(out, "/foo/bar") == 0);
    assert(out_len == 8);
}

/* The root is the mount of LAST resort: any longer mount that prefixes the path
 * takes it instead. */
static void test_a_named_mount_beats_the_root(void) {
    const char* mounts[] = {"/", "/boot"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;

    assert(route("/boot/xyz", mounts, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 1);
    assert(strcmp(out, "/xyz") == 0);

    /* Registration order must not decide it, so the same pair reversed. */
    const char* reversed[] = {"/boot", "/"};
    assert(route("/boot/xyz", reversed, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 0);
    assert(strcmp(out, "/xyz") == 0);

    /* A path under no named mount still lands on the root. */
    assert(route("/other/xyz", mounts, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 0);
    assert(strcmp(out, "/other/xyz") == 0);
}

/* Without a root mount registered, a path matching nothing is not routed --
 * the caller reports NOT_FOUND rather than guessing a backend. */
static void test_no_root_mount_leaves_a_stray_path_unrouted(void) {
    const char* mounts[] = {"/boot", "/user"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;
    assert(route("/other/xyz", mounts, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 0);
    assert(route("/", mounts, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 0);
}

/* --- longest prefix ------------------------------------------------------- */

/* A mount at depth wins over the shallower one it sits inside, which is the
 * whole point of routing on paths: /mnt/usb is its own filesystem even though
 * /mnt is one too. */
static void test_deeper_mount_wins_over_shallower(void) {
    const char* mounts[] = {"/", "/mnt", "/mnt/usb"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;

    assert(route("/mnt/usb/file", mounts, 3, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 2);
    assert(strcmp(out, "/file") == 0);

    assert(route("/mnt/other", mounts, 3, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 1);
    assert(strcmp(out, "/other") == 0);

    assert(route("/mnt", mounts, 3, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 1);
    assert(strcmp(out, "/") == 0);
}

/* Depth, not registration order, decides. Declared shallowest-first above and
 * deepest-first here. */
static void test_longest_prefix_ignores_registration_order(void) {
    const char* mounts[] = {"/mnt/usb", "/mnt", "/"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;
    assert(route("/mnt/usb/file", mounts, 3, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 0);
    assert(strcmp(out, "/file") == 0);
}

/* --- whole-segment matching ----------------------------------------------- */

/* A mount owns whole segments only. "/wfs" must not swallow "/wfsx", which is
 * the failure a plain string-prefix test would introduce: the tail would come
 * out as "x" and address a file nobody named. */
static void test_mount_matches_whole_segments_only(void) {
    const char* mounts[] = {"/", "/wfs"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;

    assert(route("/wfsx", mounts, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 0); /* the root, not /wfs */
    assert(strcmp(out, "/wfsx") == 0);

    assert(route("/wfsx/file", mounts, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 0);
    assert(strcmp(out, "/wfsx/file") == 0);

    /* And the real mount still matches. */
    assert(route("/wfs/file", mounts, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 1);
    assert(strcmp(out, "/file") == 0);
}

/* The same collision with no root to fall back to is simply not routed. */
static void test_partial_segment_is_not_routed_without_a_root(void) {
    const char* mounts[] = {"/wfs"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;
    assert(route("/wfsx", mounts, 1, &idx, out, (int32_t)sizeof(out), &out_len) == 0);
}

/* A deep mount matches on segments too: "/mnt/usbx" is not "/mnt/usb". */
static void test_deep_mount_matches_whole_segments_only(void) {
    const char* mounts[] = {"/mnt", "/mnt/usb"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;
    assert(route("/mnt/usbx/f", mounts, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 0);
    assert(strcmp(out, "/usbx/f") == 0);
}

/* --- the tail handed to the backend -------------------------------------- */

/* A path that IS its mount names that filesystem's own root. */
static void test_path_equal_to_its_mount_yields_the_backend_root(void) {
    const char* mounts[] = {"/", "/boot", "/mnt/usb"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;

    assert(route("/boot", mounts, 3, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 1);
    assert(strcmp(out, "/") == 0);
    assert(out_len == 1);

    assert(route("/mnt/usb", mounts, 3, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 2);
    assert(strcmp(out, "/") == 0);
}

/* A trailing slash names the same directory, so it yields the same tail. */
static void test_trailing_slash_yields_the_backend_root(void) {
    const char* mounts[] = {"/", "/boot"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;
    assert(route("/boot/", mounts, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 1);
    assert(strcmp(out, "/") == 0);
}

/* A mount declared with a trailing slash is the same mount as one without: the
 * registration side canonicalizes, and this is the belt to that braces. */
static void test_mount_declared_with_a_trailing_slash_still_matches(void) {
    const char* mounts[] = {"/boot/"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;
    assert(route("/boot/xyz", mounts, 1, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 0);
    assert(strcmp(out, "/xyz") == 0);
}

/* Redundant slashes inside the tail are handed on as they arrived: the backend
 * resolves its own path, and collapsing them here would be a second
 * canonicalizer to keep in step with it. */
static void test_double_slash_tail_is_preserved(void) {
    const char* mounts[] = {"/boot"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;
    assert(route("/boot//xyz", mounts, 1, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(out_len == 5);
    assert(strcmp(out, "//xyz") == 0);
}

/* --- matching rules ------------------------------------------------------ */

/* Mount matching is case-insensitive, so "/BOOT" and "/boot" are one mount. */
static void test_case_insensitive_mount_match(void) {
    const char* mounts[] = {"/", "/boot"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;
    assert(route("/BOOT/xyz", mounts, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 1);
    assert(strcmp(out, "/xyz") == 0);
}

/* Routing acts on absolute paths only. Every client path is joined onto the
 * client's working directory before it gets here, so a relative one arriving is
 * a caller bug and is refused rather than guessed at. */
static void test_relative_path_is_never_routed(void) {
    const char* mounts[] = {"/", "/boot"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;
    assert(route("boot/xyz", mounts, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 0);
    assert(route("foo", mounts, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 0);
}

/* A mount entry that is not an absolute path cannot own anything, and must not
 * be matched as if the leading slash were implied. */
static void test_mount_without_a_leading_slash_is_ignored(void) {
    const char* mounts[] = {"boot"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;
    assert(route("/boot/xyz", mounts, 1, &idx, out, (int32_t)sizeof(out), &out_len) == 0);
}

/* A ONE-CHARACTER mount that is not "/" must not be taken for the root. The
 * root is recognised by being a single '/', so a length test alone would promote
 * any one-byte entry to owning every path. */
static void test_single_character_mount_is_not_the_root(void) {
    const char* mounts[] = {"x"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;
    assert(route("/x/y", mounts, 1, &idx, out, (int32_t)sizeof(out), &out_len) == 0);
    assert(route("/anything", mounts, 1, &idx, out, (int32_t)sizeof(out), &out_len) == 0);
}

/* Two roots are as much a duplicate as two named mounts, and resolve the same
 * way: to the first registered. The root is the entry most likely to be declared
 * twice, since every backend that names no mount point would claim it. */
static void test_duplicate_root_mounts_use_the_first(void) {
    const char* mounts[] = {"/", "/"};
    char out[64];
    int32_t out_len = 0;
    int32_t idx = -1;
    assert(route("/foo", mounts, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 0);
    assert(strcmp(out, "/foo") == 0);
}

static void test_null_mount_entries_are_skipped(void) {
    const char* mounts[] = {0, "/boot"};
    char out[16];
    int32_t out_len = 0;
    int32_t idx = -1;
    assert(route("/boot/xyz", mounts, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 1);
    assert(strcmp(out, "/xyz") == 0);
}

/* Two mounts of equal depth cannot both own a path; the first registered wins,
 * so the answer is at least deterministic. The registration side refuses a
 * duplicate, which is where the condition belongs. */
static void test_duplicate_mount_paths_use_the_first(void) {
    const char* mounts[] = {"/boot", "/boot"};
    char out[16];
    int32_t out_len = 0;
    int32_t idx = -1;
    assert(route("/boot/xyz", mounts, 2, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(idx == 0);
}

/* --- refusals ------------------------------------------------------------- */

/* The tail is refused rather than truncated: a shortened path names a different
 * file, which the caller would then open without knowing. */
static void test_out_buffer_size_boundaries(void) {
    const char* mounts[] = {"/boot"};
    char out_exact[5];
    char out_small[4];
    int32_t out_len = 0;
    int32_t idx = -1;

    assert(route("/boot/xyz", mounts, 1, &idx, out_exact, (int32_t)sizeof(out_exact), &out_len) ==
           1);
    assert(out_len == 4);
    assert(strcmp(out_exact, "/xyz") == 0);

    assert(route("/boot/xyz", mounts, 1, &idx, out_small, (int32_t)sizeof(out_small), &out_len) ==
           0);
}

/* A root mount has to respect the same ceiling: its tail is the whole path, so
 * it is the longest tail any mount produces. */
static void test_root_mount_tail_respects_the_out_capacity(void) {
    const char* mounts[] = {"/"};
    char out[5];
    int32_t out_len = 0;
    int32_t idx = -1;
    assert(route("/abc", mounts, 1, &idx, out, (int32_t)sizeof(out), &out_len) == 1);
    assert(strcmp(out, "/abc") == 0);
    assert(route("/abcd", mounts, 1, &idx, out, (int32_t)sizeof(out), &out_len) == 0);
}

static void test_invalid_inputs_are_rejected(void) {
    const char* mounts[] = {"/boot"};
    char out[8];
    int32_t out_len = 0;
    int32_t idx = -1;

    assert(fsmgr_route_path_for_mounts(
               "/boot", 0, mounts, 1, &idx, out, (int32_t)sizeof(out), &out_len) == 0);
    assert(fsmgr_route_path_for_mounts(
               "", 0, mounts, 1, &idx, out, (int32_t)sizeof(out), &out_len) == 0);
    assert(fsmgr_route_path_for_mounts(
               "/boot", 5, mounts, 0, &idx, out, (int32_t)sizeof(out), &out_len) == 0);
    assert(fsmgr_route_path_for_mounts("/boot", 5, mounts, 1, &idx, out, 1, &out_len) == 0);
    assert(fsmgr_route_path_for_mounts(
               0, 5, mounts, 1, &idx, out, (int32_t)sizeof(out), &out_len) == 0);
    assert(fsmgr_route_path_for_mounts(
               "/boot", 5, 0, 1, &idx, out, (int32_t)sizeof(out), &out_len) == 0);
}

/* --- backend selection --------------------------------------------------- */

static void test_backend_selection_picks_the_owning_mount(void) {
    const char* mounts[] = {"/", "/boot", "/mnt/usb"};
    const int32_t backends[] = {101, 202, 303};
    char out[64];
    int32_t out_len = 0;
    int32_t backend = -1;

    assert(route_and_select_backend("/boot/system/fonts/roboto.ttf",
                                    mounts,
                                    backends,
                                    3,
                                    &backend,
                                    out,
                                    (int32_t)sizeof(out),
                                    &out_len) == 1);
    assert(backend == 202);
    assert(strcmp(out, "/system/fonts/roboto.ttf") == 0);

    assert(route_and_select_backend("/mnt/usb/notes.txt",
                                    mounts,
                                    backends,
                                    3,
                                    &backend,
                                    out,
                                    (int32_t)sizeof(out),
                                    &out_len) == 1);
    assert(backend == 303);
    assert(strcmp(out, "/notes.txt") == 0);

    /* Owned by nothing more specific, so the root serves it. */
    assert(
        route_and_select_backend(
            "/tmp/scratch", mounts, backends, 3, &backend, out, (int32_t)sizeof(out), &out_len) ==
        1);
    assert(backend == 101);
    assert(strcmp(out, "/tmp/scratch") == 0);
}

static void test_backend_is_untouched_when_not_routed(void) {
    const char* mounts[] = {"/boot"};
    const int32_t backends[] = {101};
    char out[64];
    int32_t out_len = 0;
    int32_t backend = -777;
    assert(route_and_select_backend(
               "/user/x", mounts, backends, 1, &backend, out, (int32_t)sizeof(out), &out_len) == 0);
    assert(backend == -777);
}

/* --- fsmgr_cwd_join ------------------------------------------------------- */

static void test_cwd_join_absolute_replaces_and_relative_extends(void) {
    char out[64];
    assert(fsmgr_cwd_join("/wfs/docs", "/boot/x", out, (int32_t)sizeof(out)) == 1);
    assert(strcmp(out, "/boot/x") == 0);
    assert(fsmgr_cwd_join("/wfs/docs", "notes.txt", out, (int32_t)sizeof(out)) == 1);
    assert(strcmp(out, "/wfs/docs/notes.txt") == 0);
    assert(fsmgr_cwd_join("/", "boot", out, (int32_t)sizeof(out)) == 1);
    assert(strcmp(out, "/boot") == 0);
}

/* Relative resolution belongs to the JOIN, not to routing: ".." walks up from
 * where the client stands, and the result is what gets routed. This is the case
 * the retired `allow_relative` was mistaken for -- that flag matched a
 * slash-less path's first segment against the mount table instead, ignoring the
 * working directory altogether. */
static void test_cwd_join_walks_up_from_the_working_directory(void) {
    char out[64];
    assert(fsmgr_cwd_join("/home/foo/bar", "../baz", out, (int32_t)sizeof(out)) == 1);
    assert(strcmp(out, "/home/foo/baz") == 0);
    assert(fsmgr_cwd_join("/home/foo/bar", "../../baz", out, (int32_t)sizeof(out)) == 1);
    assert(strcmp(out, "/home/baz") == 0);
    assert(fsmgr_cwd_join("/home/foo/bar", "./sib/../baz", out, (int32_t)sizeof(out)) == 1);
    assert(strcmp(out, "/home/foo/bar/baz") == 0);
}

static void test_cwd_join_resolves_dots_and_cannot_escape_the_root(void) {
    char out[64];
    assert(fsmgr_cwd_join("/wfs/docs", "..", out, (int32_t)sizeof(out)) == 1);
    assert(strcmp(out, "/wfs") == 0);
    assert(fsmgr_cwd_join("/wfs/docs", "./.", out, (int32_t)sizeof(out)) == 1);
    assert(strcmp(out, "/wfs/docs") == 0);
    assert(fsmgr_cwd_join("/wfs", "../../..", out, (int32_t)sizeof(out)) == 1);
    assert(strcmp(out, "/") == 0);
    assert(fsmgr_cwd_join("/", "..", out, (int32_t)sizeof(out)) == 1);
    assert(strcmp(out, "/") == 0);
}

static void test_cwd_join_refuses_rather_than_truncating(void) {
    char out[8];
    assert(fsmgr_cwd_join("/wfs", "a/very/long/tail", out, (int32_t)sizeof(out)) == 0);
    /* A refusal leaves no PARTIAL path behind: a caller ignoring the return sees
     * an empty string rather than a prefix naming a different file. */
    assert(out[0] == '\0');
}

static void test_cwd_join_rejects_a_relative_cwd(void) {
    char out[64];
    assert(fsmgr_cwd_join("wfs/docs", "x", out, (int32_t)sizeof(out)) == 0);
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_root_mount_owns_every_path),
        WASMOS_TEST_CASE(test_a_named_mount_beats_the_root),
        WASMOS_TEST_CASE(test_no_root_mount_leaves_a_stray_path_unrouted),
        WASMOS_TEST_CASE(test_deeper_mount_wins_over_shallower),
        WASMOS_TEST_CASE(test_longest_prefix_ignores_registration_order),
        WASMOS_TEST_CASE(test_mount_matches_whole_segments_only),
        WASMOS_TEST_CASE(test_partial_segment_is_not_routed_without_a_root),
        WASMOS_TEST_CASE(test_deep_mount_matches_whole_segments_only),
        WASMOS_TEST_CASE(test_path_equal_to_its_mount_yields_the_backend_root),
        WASMOS_TEST_CASE(test_trailing_slash_yields_the_backend_root),
        WASMOS_TEST_CASE(test_mount_declared_with_a_trailing_slash_still_matches),
        WASMOS_TEST_CASE(test_double_slash_tail_is_preserved),
        WASMOS_TEST_CASE(test_case_insensitive_mount_match),
        WASMOS_TEST_CASE(test_relative_path_is_never_routed),
        WASMOS_TEST_CASE(test_mount_without_a_leading_slash_is_ignored),
        WASMOS_TEST_CASE(test_single_character_mount_is_not_the_root),
        WASMOS_TEST_CASE(test_duplicate_root_mounts_use_the_first),
        WASMOS_TEST_CASE(test_null_mount_entries_are_skipped),
        WASMOS_TEST_CASE(test_duplicate_mount_paths_use_the_first),
        WASMOS_TEST_CASE(test_out_buffer_size_boundaries),
        WASMOS_TEST_CASE(test_root_mount_tail_respects_the_out_capacity),
        WASMOS_TEST_CASE(test_invalid_inputs_are_rejected),
        WASMOS_TEST_CASE(test_backend_selection_picks_the_owning_mount),
        WASMOS_TEST_CASE(test_backend_is_untouched_when_not_routed),
        WASMOS_TEST_CASE(test_cwd_join_absolute_replaces_and_relative_extends),
        WASMOS_TEST_CASE(test_cwd_join_walks_up_from_the_working_directory),
        WASMOS_TEST_CASE(test_cwd_join_resolves_dots_and_cannot_escape_the_root),
        WASMOS_TEST_CASE(test_cwd_join_refuses_rather_than_truncating),
        WASMOS_TEST_CASE(test_cwd_join_rejects_a_relative_cwd),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_fs_manager_path: ok\n");
    return 0;
}
