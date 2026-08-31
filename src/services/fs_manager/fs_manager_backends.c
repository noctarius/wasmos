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

int32_t fsmgr_mount_name_from_reported(const char* reported, char* out, uint32_t out_cap) {
    const char* src;
    uint32_t len = 0;

    if (!reported || !out || out_cap < 2u) {
        return 0;
    }
    out[0] = '\0';
    src = (reported[0] == '/') ? &reported[1] : reported;
    /* Empty after the strip: either the backend named nothing, or it named the
     * root, which is a mount PATH and carries no mount name. */
    if (src[0] == '\0') {
        return 0;
    }
    while (src[len] != '\0') {
        if (len + 1u >= out_cap) {
            out[0] = '\0';
            return 0;
        }
        out[len] = ascii_tolower(src[len]);
        len++;
    }
    out[len] = '\0';
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
