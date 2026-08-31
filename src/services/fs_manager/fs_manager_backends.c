/* fs_manager_backends.c - backend-table decisions for the FS manager service */
#include "fs_manager_backends.h"

/* FS_TYPE_* arrives through this header, which pulls the generated
 * abi/constants.yaml values. */
#include "wasmos_driver_abi.h"
/* One row per FS_TYPE_*. A filesystem is named here and in abi/constants.yaml
 * and nowhere else, so a future devfs or sysfs costs a row rather than a branch
 * at each site that names one.
 *
 * FS_TYPE_UNKNOWN is deliberately absent -- it is the miss, not a row. */
static const struct {
    uint32_t fs_type;
    const char* name;
} k_fs_types[] = {
    {(uint32_t)FS_TYPE_FAT, "fs-fat"},
    {(uint32_t)FS_TYPE_WFS, "fs-wfs"},
    {(uint32_t)FS_TYPE_INITFS, "fs-init"},
    {(uint32_t)FS_TYPE_TMPFS, "fs-tmpfs"},
};

#define K_FS_TYPE_COUNT ((uint32_t)(sizeof(k_fs_types) / sizeof(k_fs_types[0])))

/* ASCII-only tolower. Local rather than libsys's wasmos_sys_to_lower_ascii so
 * this translation unit stays linkable on the host, which is what lets the
 * decisions in it be unit-tested (tests/unit/test_fs_manager_backends.c). */
static char ascii_tolower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

int32_t fsmgr_mount_path_from_reported(const char* reported, char* out, uint32_t out_cap) {
    const char* src;
    uint32_t len = 0;
    uint32_t i;

    if (!reported || !out || out_cap < 3u) {
        return 0;
    }
    out[0] = '\0';
    /* One leading slash, however many the report carried: "boot", "/boot" and
     * "//boot" are the same mount. */
    src = reported;
    while (*src == '/') {
        src++;
    }
    out[len++] = '/';
    for (i = 0u; src[i] != '\0'; ++i) {
        if (len + 1u >= out_cap) {
            out[0] = '\0';
            return 0;
        }
        out[len++] = ascii_tolower(src[i]);
    }
    out[len] = '\0';
    /* Drop a trailing slash so "/boot/" is not a second mount; the root keeps its
     * single one, which is the only path that legitimately ends in a slash. */
    while (len > 1u && out[len - 1u] == '/') {
        len--;
        out[len] = '\0';
    }
    /* A report of nothing but slashes normalizes to the root, which IS a mount --
     * but an EMPTY report does not name one, and neither does a path carrying a
     * relative component, which nothing could route to. */
    if (reported[0] == '\0') {
        out[0] = '\0';
        return 0;
    }
    for (i = 1u; i < len; ++i) {
        if (out[i] != '.') {
            continue;
        }
        if ((out[i - 1u] == '/') &&
            (out[i + 1u] == '\0' || out[i + 1u] == '/' ||
             (out[i + 1u] == '.' && (out[i + 2u] == '\0' || out[i + 2u] == '/')))) {
            out[0] = '\0';
            return 0;
        }
    }
    return 1;
}

const char* fsmgr_backend_fs_name(const fs_backend_t* backend) {
    if (!backend) {
        return "fs";
    }
    for (uint32_t i = 0; i < K_FS_TYPE_COUNT; ++i) {
        if (k_fs_types[i].fs_type == backend->fs_type) {
            return k_fs_types[i].name;
        }
    }
    return "fs";
}
