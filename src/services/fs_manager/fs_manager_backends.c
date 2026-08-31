/* fs_manager_backends.c - backend-table decisions for the FS manager service */
#include "fs_manager_backends.h"

/* FS_TYPE_* arrives through this header, which pulls the generated
 * abi/constants.yaml values. */
#include "wasmos_driver_abi.h"
/* One row per FS_TYPE_*. A filesystem is described here and in
 * abi/constants.yaml and nowhere else, so a future devfs or sysfs costs a row
 * rather than a branch at each site that names something.
 *
 * `default_mount` is the mount name a backend of this type takes when it
 * reports none of its own, and is NULL for a filesystem that always does. Only
 * a non-block backend needs one: a block backend's mount comes from the rule
 * that spawned it, while a pseudo-filesystem is spawned with no rule and no
 * volume, so its name has to be known from what it is.
 *
 * FS_TYPE_UNKNOWN is deliberately absent -- it is the miss, not a row. */
static const struct {
    uint32_t fs_type;
    const char* name;
    const char* default_mount;
} k_fs_types[] = {
    {(uint32_t)FS_TYPE_FAT, "fs-fat", 0},
    {(uint32_t)FS_TYPE_WFS, "fs-wfs", 0},
    {(uint32_t)FS_TYPE_INITFS, "fs-init", "init"},
};

#define K_FS_TYPE_COUNT ((uint32_t)(sizeof(k_fs_types) / sizeof(k_fs_types[0])))

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

const char* fsmgr_default_mount_name(uint32_t fs_type) {
    for (uint32_t i = 0; i < K_FS_TYPE_COUNT; ++i) {
        if (k_fs_types[i].fs_type == fs_type) {
            return k_fs_types[i].default_mount;
        }
    }
    return 0;
}
