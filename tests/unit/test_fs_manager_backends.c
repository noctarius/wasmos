/* test_fs_manager_backends.c — backend-table decisions for the FS manager
 * (fs_manager_backends.h): what the filesystem serving a mount is called, and
 * which backend serves paths that name no mount.
 *
 * src/services/fs_manager/fs_manager_backends.c is the only source linked in:
 * both decisions are pure functions of the registration table, so the table is
 * built per case here and nothing is stubbed.
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

#include "fs_manager_backends.h"
#include "wasmos_driver_abi.h"

/* One registered backend. `kind` is FSMGR_BACKEND_BLOCK for every block-backed
 * backend regardless of filesystem, which is the distinction these cases turn
 * on: it is `fs_type` that says which filesystem is mounted. */
static fs_backend_t make_backend(uint8_t slot, uint8_t kind, uint32_t fs_type,
                                 const char* mount_name) {
    fs_backend_t backend;
    memset(&backend, 0, sizeof(backend));
    backend.in_use = 1;
    backend.slot = slot;
    backend.kind = kind;
    backend.fs_type = fs_type;
    backend.endpoint = 100 + (int32_t)slot;
    backend.unit = slot;
    snprintf(backend.mount_name, sizeof(backend.mount_name), "%s", mount_name);
    return backend;
}

/* Regression: 2026-08-30-fsmgr-backend-identity — every block-backed backend
 * registers as FSMGR_BACKEND_BLOCK, and `mount` derived its filesystem label
 * from that value, so both WFS mounts were reported as fs-fat. The label was
 * the visible half of a backend table that carried no filesystem identity at
 * all. */
static void test_wfs_backend_is_not_named_fat(void) {
    fs_backend_t wfs = make_backend(2, FSMGR_BACKEND_BLOCK, FS_TYPE_WFS, "wfs");
    const char* name = fsmgr_backend_fs_name(&wfs);
    assert(strcmp(name, "fs-wfs") == 0);
}

static void test_fat_backend_is_named_fat(void) {
    fs_backend_t fat = make_backend(0, FSMGR_BACKEND_BLOCK, FS_TYPE_FAT, "boot");
    assert(strcmp(fsmgr_backend_fs_name(&fat), "fs-fat") == 0);
}

static void test_init_backend_is_named_init(void) {
    fs_backend_t init = make_backend(0, FSMGR_BACKEND_INIT, FS_TYPE_INITFS, "init");
    assert(strcmp(fsmgr_backend_fs_name(&init), "fs-init") == 0);
}

/* A pseudo-filesystem is named from its reported type like any other, so no
 * call site carries a branch for one. Naming initfs from its `kind` instead
 * worked only because initfs was the single pseudo-filesystem; a devfs or sysfs
 * would each have needed another case. These two cases cross kind and fs_type
 * deliberately: a backend is named by WHAT IT SERVES, and `kind` must not
 * influence the answer in either direction. */
static void test_name_comes_from_fs_type_not_kind(void) {
    fs_backend_t initfs_kind_boot = make_backend(0, FSMGR_BACKEND_BLOCK, FS_TYPE_INITFS, "init");
    fs_backend_t wfs_kind_init = make_backend(1, FSMGR_BACKEND_INIT, FS_TYPE_WFS, "wfs");
    assert(strcmp(fsmgr_backend_fs_name(&initfs_kind_boot), "fs-init") == 0);
    assert(strcmp(fsmgr_backend_fs_name(&wfs_kind_init), "fs-wfs") == 0);
}

/* An unrecognised type is the only miss, and it is a miss for a block-backed
 * and a non-block-backed backend alike. */
static void test_unknown_type_is_a_miss_for_any_kind(void) {
    fs_backend_t init_kind = make_backend(0, FSMGR_BACKEND_INIT, FS_TYPE_UNKNOWN, "init");
    assert(strcmp(fsmgr_backend_fs_name(&init_kind), "fs") == 0);
}

/* A backend that reports no filesystem type is named generically rather than
 * being guessed at: naming it after a filesystem it may not be is what this
 * suite exists to prevent. */
static void test_unknown_fs_type_is_named_generically(void) {
    fs_backend_t unknown = make_backend(3, FSMGR_BACKEND_BLOCK, FS_TYPE_UNKNOWN, "fs");
    assert(strcmp(fsmgr_backend_fs_name(&unknown), "fs") == 0);
}

static void test_null_backend_is_named_generically(void) {
    assert(fsmgr_backend_fs_name(0) != 0);
    assert(strcmp(fsmgr_backend_fs_name(0), "fs") == 0);
}

/* Two mounts differing only in filesystem must not be reported identically:
 * this is the case the boot log showed, where /boot and /wfs both read fs-fat. */
static void test_two_block_backends_are_distinguished(void) {
    fs_backend_t backends[2];
    backends[0] = make_backend(0, FSMGR_BACKEND_BLOCK, FS_TYPE_FAT, "boot");
    backends[1] = make_backend(1, FSMGR_BACKEND_BLOCK, FS_TYPE_WFS, "wfs");
    assert(strcmp(fsmgr_backend_fs_name(&backends[0]), fsmgr_backend_fs_name(&backends[1])) != 0);
}

/* Regression: 2026-08-30-fsmgr-root-by-slot-order — the root filesystem was the
 * first block-backed backend in slot order. Several register per boot (one per
 * mounted volume), so the volume serving every unrouted absolute path
 * ("/system/utils/ip", "/apps/calculator", the spawn path) was decided by
 * registration order. It resolved correctly only because the boot FAT happened
 * to register first. */
static void test_root_is_not_decided_by_registration_order(void) {
    fs_backend_t backends[3];
    /* A WFS volume registers into slot 0, ahead of the boot volume. */
    backends[0] = make_backend(0, FSMGR_BACKEND_BLOCK, FS_TYPE_WFS, "wfs");
    backends[1] = make_backend(1, FSMGR_BACKEND_BLOCK, FS_TYPE_FAT, "boot");
    backends[2] = make_backend(2, FSMGR_BACKEND_INIT, FS_TYPE_UNKNOWN, "init");

    int32_t root = fsmgr_select_root_backend(backends, 3);
    assert(root == 1);
    assert(strcmp(backends[root].mount_name, "boot") == 0);
}

static void test_root_is_found_in_slot_order_too(void) {
    fs_backend_t backends[2];
    backends[0] = make_backend(0, FSMGR_BACKEND_BLOCK, FS_TYPE_FAT, "boot");
    backends[1] = make_backend(1, FSMGR_BACKEND_BLOCK, FS_TYPE_WFS, "wfs");
    assert(fsmgr_select_root_backend(backends, 2) == 0);
}

/* Before the boot volume registers there is no root filesystem, and the caller
 * must be able to tell that from "index 0". */
static void test_no_root_before_the_boot_volume_registers(void) {
    fs_backend_t backends[2];
    backends[0] = make_backend(0, FSMGR_BACKEND_INIT, FS_TYPE_UNKNOWN, "init");
    backends[1] = make_backend(1, FSMGR_BACKEND_BLOCK, FS_TYPE_WFS, "wfs");
    assert(fsmgr_select_root_backend(backends, 2) == -1);
}

static void test_unused_slots_are_skipped(void) {
    fs_backend_t backends[3];
    memset(backends, 0, sizeof(backends));
    backends[2] = make_backend(2, FSMGR_BACKEND_BLOCK, FS_TYPE_FAT, "boot");
    assert(fsmgr_select_root_backend(backends, 3) == 2);
}

static void test_empty_table_has_no_root(void) {
    fs_backend_t backends[2];
    memset(backends, 0, sizeof(backends));
    assert(fsmgr_select_root_backend(backends, 2) == -1);
    assert(fsmgr_select_root_backend(0, 2) == -1);
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_wfs_backend_is_not_named_fat),
        WASMOS_TEST_CASE(test_fat_backend_is_named_fat),
        WASMOS_TEST_CASE(test_init_backend_is_named_init),
        WASMOS_TEST_CASE(test_name_comes_from_fs_type_not_kind),
        WASMOS_TEST_CASE(test_unknown_type_is_a_miss_for_any_kind),
        WASMOS_TEST_CASE(test_unknown_fs_type_is_named_generically),
        WASMOS_TEST_CASE(test_null_backend_is_named_generically),
        WASMOS_TEST_CASE(test_two_block_backends_are_distinguished),
        WASMOS_TEST_CASE(test_root_is_not_decided_by_registration_order),
        WASMOS_TEST_CASE(test_root_is_found_in_slot_order_too),
        WASMOS_TEST_CASE(test_no_root_before_the_boot_volume_registers),
        WASMOS_TEST_CASE(test_unused_slots_are_skipped),
        WASMOS_TEST_CASE(test_empty_table_has_no_root),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_fs_manager_backends: ok\n");
    return 0;
}
