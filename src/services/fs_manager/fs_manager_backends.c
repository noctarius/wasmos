/* fs_manager_backends.c - backend-table decisions for the FS manager service */
#include "fs_manager_backends.h"

/* FS_TYPE_* arrives through this header, which pulls the generated
 * abi/constants.yaml values. */
#include "wasmos_driver_abi.h"
/* wasmos_sys_strcasecmp: NULL-safe ASCII compare, the same helper fs_manager.c
 * matches mount names with. */
#include "wasmos/libsys_string.h"

/* One row per FS_TYPE_*. A filesystem is named here and in abi/constants.yaml
 * and nowhere else: a pseudo-filesystem such as initfs is a row like any other,
 * so a future devfs or sysfs costs a row rather than a branch.
 * FS_TYPE_UNKNOWN is deliberately absent -- it is the miss, not a row. */
static const struct {
    uint32_t fs_type;
    const char* name;
} k_fs_names[] = {
    {(uint32_t)FS_TYPE_FAT, "fs-fat"},
    {(uint32_t)FS_TYPE_WFS, "fs-wfs"},
    {(uint32_t)FS_TYPE_INITFS, "fs-init"},
};

const char* fsmgr_backend_fs_name(const fs_backend_t* backend) {
    if (!backend) {
        return "fs";
    }
    for (uint32_t i = 0; i < (uint32_t)(sizeof(k_fs_names) / sizeof(k_fs_names[0])); ++i) {
        if (k_fs_names[i].fs_type == backend->fs_type) {
            return k_fs_names[i].name;
        }
    }
    return "fs";
}

int32_t fsmgr_select_root_backend(const fs_backend_t* backends, uint32_t count) {
    if (!backends) {
        return -1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (backends[i].in_use &&
            wasmos_sys_strcasecmp(backends[i].mount_name, FSMGR_ROOT_MOUNT_NAME) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}
