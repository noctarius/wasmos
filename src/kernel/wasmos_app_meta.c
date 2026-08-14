/* wasmos_app_meta.c - Boot module metadata helpers for WASMOS-APP packages.
 * Scans the boot_info_t module list to find .wap blobs and resolve app
 * metadata (name, WASM section offset) without copying the binary payload. */
#include "wasmos_app_meta.h"
#include "string.h"
#include "wasmos_driver_abi.h"

static const boot_module_t* app_meta_module_at(const boot_info_t* boot_info, uint32_t index) {
    const uint8_t* mods = 0;
    if (!boot_info || !(boot_info->flags & BOOT_INFO_FLAG_MODULES_PRESENT)) {
        return 0;
    }
    if (!boot_info->modules || boot_info->module_entry_size < sizeof(boot_module_t)) {
        return 0;
    }
    if (index >= boot_info->module_count) {
        return 0;
    }
    mods = (const uint8_t*)boot_info->modules;
    return (const boot_module_t*)(mods + index * boot_info->module_entry_size);
}

/* Folds a package's declared capability names into the DEVMGR_CAP_* bitmask the
 * device manager understands.  Only "io.port", "irq.route" and "dma.buffer" have
 * a DEVMGR bit; every other capability the package declares — including
 * "mmio.map" and "system.control" — is silently absent from the result, so this
 * mask is not a faithful summary of the package's capabilities.  A NULL desc, or
 * one declaring none of the three, yields 0. */
uint32_t wasmos_app_driver_cap_flags(const wasmos_app_desc_t* desc) {
    uint32_t cap_flags = 0;
    if (!desc) {
        return 0;
    }
    for (uint32_t i = 0; i < desc->cap_count; ++i) {
        if (str_eq_bytes(desc->caps[i].name, desc->caps[i].name_len, "io.port")) {
            cap_flags |= DEVMGR_CAP_IO_PORT;
        } else if (str_eq_bytes(desc->caps[i].name, desc->caps[i].name_len, "irq.route")) {
            cap_flags |= DEVMGR_CAP_IRQ;
        } else if (str_eq_bytes(desc->caps[i].name, desc->caps[i].name_len, "dma.buffer")) {
            cap_flags |= DEVMGR_CAP_DMA;
        }
    }
    return cap_flags;
}

/* Parses the boot module at `module_index` as a WASMOS-APP package.
 *
 * The blob is read in place at mod->base, so the caller must be running under a
 * root where that address is mapped, and *out_desc holds pointers INTO it — the
 * payload is never copied.  The descriptor is therefore valid only while the
 * module's memory is.
 *
 * Returns 0 on success and -1 for a missing or out-of-range module, a module
 * that is not of the WASMOS-APP type, one with a zero base or size or a size
 * above 4 GiB, a NULL out_desc, or a blob wasmos_app_parse rejects. */
int wasmos_app_module_desc(const boot_info_t* boot_info, uint32_t module_index,
                           wasmos_app_desc_t* out_desc) {
    const boot_module_t* mod = app_meta_module_at(boot_info, module_index);
    if (!mod || !out_desc || mod->type != BOOT_MODULE_TYPE_WASMOS_APP || mod->base == 0 ||
        mod->size == 0 || mod->size > 0xFFFFFFFFULL) {
        return -1;
    }
    return wasmos_app_parse(ptr_cast(uint8_t, mod->base), (uint32_t)mod->size, out_desc);
}

/* Finds a WASMOS-APP package by its initfs path and parses it.
 *
 * The descriptor comes from the INITFS image, and *out_desc points into it —
 * again no copy.  *out_module_index is a convenience cross-reference to the boot
 * module list, matched by name, and is set to 0xFFFFFFFF when no module carries
 * that path; a 0 return therefore does NOT guarantee a usable module index.
 *
 * The initfs header is validated before it is walked (magic, entry size, and an
 * entry count that fits the image), and each candidate entry's offset and size
 * are bounds-checked, so a malformed image is refused rather than walked off the
 * end.  The path must match exactly.
 *
 * Returns 0 on success, -1 for a NULL argument, an absent or malformed initfs,
 * no entry with that path, an out-of-bounds entry, or a blob wasmos_app_parse
 * rejects. */
int wasmos_app_module_desc_by_initfs_path(const boot_info_t* boot_info, const char* path,
                                          uint32_t* out_module_index, wasmos_app_desc_t* out_desc) {
    const uint8_t* initfs_base = 0;
    const uint8_t* entries_base = 0;
    const wasmos_initfs_header_t* hdr = 0;
    uint32_t module_index = 0xFFFFFFFFu;

    if (!boot_info || !path || !out_module_index || !out_desc || !boot_info->initfs ||
        boot_info->initfs_size == 0 || (boot_info->flags & BOOT_INFO_FLAG_INITFS_PRESENT) == 0) {
        return -1;
    }

    initfs_base = ptr_cast(uint8_t, boot_info->initfs);
    if (boot_info->initfs_size < sizeof(wasmos_initfs_header_t)) {
        return -1;
    }
    hdr = (const wasmos_initfs_header_t*)initfs_base;
    if (hdr->magic[0] != 'W' || hdr->magic[1] != 'M' || hdr->magic[2] != 'I' ||
        hdr->magic[3] != 'N' || hdr->magic[4] != 'I' || hdr->magic[5] != 'T' ||
        hdr->magic[6] != 'F' || hdr->magic[7] != 'S') {
        return -1;
    }
    if (hdr->entry_size < sizeof(wasmos_initfs_entry_t) ||
        hdr->header_size > boot_info->initfs_size) {
        return -1;
    }
    if (hdr->entry_count > ((boot_info->initfs_size - hdr->header_size) / hdr->entry_size)) {
        return -1;
    }
    entries_base = initfs_base + hdr->header_size;

    for (uint32_t i = 0; i < hdr->entry_count; ++i) {
        const wasmos_initfs_entry_t* entry =
            (const wasmos_initfs_entry_t*)(entries_base + (i * hdr->entry_size));
        if (entry->type != WASMOS_INITFS_ENTRY_WASMOS_APP) {
            continue;
        }
        if (strcmp(entry->path, path) != 0) {
            continue;
        }
        if (entry->offset >= boot_info->initfs_size || entry->size == 0 ||
            entry->size > (boot_info->initfs_size - entry->offset)) {
            return -1;
        }
        if (wasmos_app_parse(initfs_base + entry->offset, entry->size, out_desc) != 0) {
            return -1;
        }
        for (uint32_t m = 0; m < boot_info->module_count; ++m) {
            const boot_module_t* mod = app_meta_module_at(boot_info, m);
            if (mod && strcmp(mod->name, path) == 0) {
                module_index = m;
                break;
            }
        }
        *out_module_index = module_index;
        return 0;
    }
    return -1;
}
