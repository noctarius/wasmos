#ifndef WASMOS_APP_META_H
#define WASMOS_APP_META_H

#include <stdint.h>
#include "boot.h"
#include "wasmos_app.h"

uint32_t wasmos_app_driver_cap_flags(const wasmos_app_desc_t *desc);
int wasmos_app_module_desc(const boot_info_t *boot_info,
                           uint32_t module_index,
                           wasmos_app_desc_t *out_desc);
int wasmos_app_module_desc_by_initfs_path(const boot_info_t *boot_info,
                                          const char *path,
                                          uint32_t *out_module_index,
                                          wasmos_app_desc_t *out_desc);

#endif
