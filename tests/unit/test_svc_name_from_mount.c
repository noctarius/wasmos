/* test_svc_name_from_mount.c — the service NAME a filesystem backend registers,
 * built by wasmos_svc_name_from_mount() in wasmos_driver_abi.h.
 *
 * Header-only: the builder and the fingerprint under it are static inlines, so
 * nothing is linked and nothing is stubbed.
 *
 * What these cases pin is a CONTRACT SHARED WITH ZIG. driver.zig mirrors this
 * builder as serviceNameFromMount for fs_tmpfs, and the two must produce the
 * same bytes for the same input — the vectors below are the reference. The Zig
 * side is not compiled here, so a divergence shows up as a tmpfs service name
 * that differs from a fat or wfs one for the same mount, not as a failure of
 * this suite.
 *
 * Cases report through assert(), so the first failure aborts and main() prints
 * "ok" only if every case ran to completion. Compiled without -DNDEBUG.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wasmos_driver_abi.h"

/* The capacity every backend passes: the registry's own, NUL included. */
#define CAP WASMOS_SVC_NAME_MAX

/* A mount that fits is spelled out, because the readable form is the whole
 * point: it is the only copy of a mount's identity the kernel's panic dump has. */
static void a_short_mount_is_spelled_in_the_name(void) {
    char out[CAP];
    assert(wasmos_svc_name_from_mount(out, sizeof(out), "fs.fat", "/user") == 11u);
    assert(strcmp(out, "fs.fat/user") == 0);

    assert(wasmos_svc_name_from_mount(out, sizeof(out), "tmpfs", "/home/user") == 15u);
    assert(strcmp(out, "tmpfs/home/user") == 0);
}

/* The boundary is exact: prefix + mount + NUL must fit the capacity. One byte
 * either side of it picks a different form, and a name that filled the buffer
 * with no room to terminate would be the bug the fallback exists to avoid. */
static void the_readable_form_is_taken_up_to_the_last_byte_that_fits(void) {
    char out[CAP];
    /* 6 + 29 = 35 characters, plus NUL = 36 = CAP. */
    const char* fits = "/aaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    /* One character longer. */
    const char* over = "/aaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

    assert(strlen(fits) == 29u);
    assert(wasmos_svc_name_from_mount(out, sizeof(out), "fs.fat", fits) == 35u);
    assert(strcmp(out, "fs.fat/aaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);

    assert(wasmos_svc_name_from_mount(out, sizeof(out), "fs.fat", over) == 14u);
    assert(strncmp(out, "fs.fat", 6u) == 0);
    assert(strlen(out) == 14u);
}

/* The reason the fallback is a fingerprint rather than a truncation: two deep
 * paths that agree over the width a truncated name would keep must still get
 * DIFFERENT names. Sharing one would have the second backend refused and its
 * mount fail on the spawn deadline. */
static void two_deep_paths_sharing_a_prefix_do_not_share_a_name(void) {
    char a[CAP];
    char b[CAP];
    const char* left = "/srv/data/volumes/project-alpha/one";
    const char* right = "/srv/data/volumes/project-alpha/two";

    assert(strlen(left) > (size_t)(CAP - 1u - 6u));
    assert(wasmos_svc_name_from_mount(a, sizeof(a), "fs.wfs", left) == 14u);
    assert(wasmos_svc_name_from_mount(b, sizeof(b), "fs.wfs", right) == 14u);
    assert(strcmp(a, b) != 0);
}

/* The hex is the mount path's own fingerprint, so the name a backend registers
 * and the class instance it claims are derived from the same value -- one
 * identity, spelled two ways, rather than two that can disagree. */
static void the_fallback_spells_the_mount_fingerprint(void) {
    char out[CAP];
    const char* deep = "/srv/data/volumes/project-alpha/one";
    uint32_t hash = wasmos_class_instance_fingerprint(deep);
    char expect[CAP];

    (void)snprintf(expect, sizeof(expect), "fs.wfs%08x", (unsigned)hash);
    assert(wasmos_svc_name_from_mount(out, sizeof(out), "fs.wfs", deep) == 14u);
    assert(strcmp(out, expect) == 0);
}

/* A buffer too small for even the fallback yields nothing rather than a partial
 * name: a caller that registered a half-built name would bind something no
 * reader could resolve. */
static void a_capacity_below_the_fallback_yields_nothing(void) {
    char out[10];
    out[0] = 'x';
    /* 6 prefix + 8 hex + NUL = 15, which 10 cannot hold. */
    assert(wasmos_svc_name_from_mount(out, sizeof(out), "fs.fat", "/a/very/deep/path") == 0u);
    assert(out[0] == '\0');
    assert(wasmos_svc_name_from_mount(out, 0u, "fs.fat", "/user") == 0u);
}

/* A NULL prefix or mount is refused rather than treated as empty: an empty name
 * registers into the flat table and collides with the next one that does. */
static void a_missing_argument_yields_nothing(void) {
    char out[CAP];
    out[0] = 'x';
    assert(wasmos_svc_name_from_mount(out, sizeof(out), 0, "/user") == 0u);
    assert(out[0] == '\0');
    out[0] = 'x';
    assert(wasmos_svc_name_from_mount(out, sizeof(out), "fs.fat", 0) == 0u);
    assert(out[0] == '\0');
    assert(wasmos_svc_name_from_mount(0, sizeof(out), "fs.fat", "/user") == 0u);
}

int main(void) {
    a_short_mount_is_spelled_in_the_name();
    the_readable_form_is_taken_up_to_the_last_byte_that_fits();
    two_deep_paths_sharing_a_prefix_do_not_share_a_name();
    the_fallback_spells_the_mount_fingerprint();
    a_capacity_below_the_fallback_yields_nothing();
    a_missing_argument_yields_nothing();
    printf("test_svc_name_from_mount: ok\n");
    return 0;
}
