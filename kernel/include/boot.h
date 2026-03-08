#ifndef WASMOS_BOOT_H
#define WASMOS_BOOT_H

#include <stdint.h>

#define BOOT_INFO_VERSION 2u
#define BOOT_INFO_FLAG_GOP_PRESENT (1u << 0)
#define BOOT_INFO_FLAG_MODULES_PRESENT (1u << 1)

typedef enum {
    BOOT_MODULE_TYPE_NONE = 0,
    BOOT_MODULE_TYPE_WASMOS_APP = 1
} boot_module_type_t;

typedef struct {
    uint64_t base;
    uint64_t size;
    uint32_t type;
    uint32_t reserved;
    char name[48];
} boot_module_t;

typedef struct {
    uint32_t version;
    uint32_t size;
    uint32_t flags;
    void *memory_map;
    uint64_t memory_map_size;
    uint64_t memory_desc_size;
    uint32_t memory_desc_version;
    void *framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pixels_per_scanline;
    void *modules;
    uint32_t module_count;
    uint32_t module_entry_size;
} boot_info_t;

#endif
