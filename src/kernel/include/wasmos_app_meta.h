/* wasmos_app_meta.h - Boot module metadata helpers for .wap package scanning. */
#ifndef WASMOS_APP_META_H
#define WASMOS_APP_META_H

#include <stdint.h>
#include "boot.h"
#include "wasmos_app.h"

/* Translate the package's capability requests into the DEVMGR_CAP_* mask device-manager
 * uses, recognising "io.port", "irq.route" and "dma.buffer".  Any other requested
 * capability contributes no bit, so the result is a device-manager view of the request
 * set and not a complete one.  A NULL descriptor yields 0. */
uint32_t wasmos_app_driver_cap_flags(const wasmos_app_desc_t* desc);

/* Parse the boot module at `module_index` into *out_desc.  Returns 0 on success and -1
 * for a NULL argument, an index past module_count, a module that is not a .wap payload,
 * an empty or oversized module, or a package wasmos_app_parse rejects.  The descriptor
 * aliases into the boot module's bytes, which live for the lifetime of the kernel. */
int wasmos_app_module_desc(const boot_info_t* boot_info, uint32_t module_index,
                           wasmos_app_desc_t* out_desc);

/* Parse the WASMOS_APP initfs entry whose path equals `path` into *out_desc, and report
 * the index of the boot module of the same name in *out_module_index.  The descriptor
 * aliases the initfs image, not the module copy.  Returns 0 on success and -1 for a NULL
 * argument, an absent or malformed initfs, a path that matches no WASMOS_APP entry, an
 * entry whose payload escapes the image, or a package that fails to parse.  On success
 * *out_module_index is 0xFFFFFFFF when no boot module carries that name, so the caller
 * must test it before using it as an index. */
int wasmos_app_module_desc_by_initfs_path(const boot_info_t* boot_info, const char* path,
                                          uint32_t* out_module_index, wasmos_app_desc_t* out_desc);

#endif
