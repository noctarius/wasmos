#ifndef WASMOS_BOOT_H
#define WASMOS_BOOT_H

#include <stdint.h>

#define BOOT_INFO_VERSION 1u
#define BOOT_INFO_FLAG_GOP_PRESENT (1u << 0)
#define BOOT_INFO_FLAG_MODULES_PRESENT (1u << 1)

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
} boot_info_t;

#endif
