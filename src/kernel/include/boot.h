#ifndef WASMOS_BOOT_H
#define WASMOS_BOOT_H

#include <stdint.h>

#define BOOT_INFO_VERSION 4u
#define BOOT_INFO_FLAG_GOP_PRESENT (1u << 0)
#define BOOT_INFO_FLAG_MODULES_PRESENT (1u << 1)
#define BOOT_INFO_FLAG_INITFS_PRESENT (1u << 2)

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

#define WASMOS_INITFS_MAGIC "WMINITFS"
#define WASMOS_INITFS_VERSION 1u

typedef enum {
    WASMOS_INITFS_ENTRY_NONE = 0,
    WASMOS_INITFS_ENTRY_WASMOS_APP = 1,
    WASMOS_INITFS_ENTRY_CONFIG = 2,
    WASMOS_INITFS_ENTRY_DATA = 3
} wasmos_initfs_entry_type_t;

#define WASMOS_INITFS_ENTRY_FLAG_BOOTSTRAP (1u << 0)

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t total_size;
    uint32_t reserved;
} wasmos_initfs_header_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint32_t offset;
    uint32_t size;
    char path[96];
} wasmos_initfs_entry_t;

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
    void *rsdp;
    uint32_t rsdp_length;
    void *initfs;
    uint32_t initfs_size;
    void *boot_config;
    uint32_t boot_config_size;
} boot_info_t;

#endif
