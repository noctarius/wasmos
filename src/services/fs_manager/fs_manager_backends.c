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
