/* boot.h - Bootloader-to-kernel handoff contract.
 *
 * The UEFI bootloader fills a boot_info_t and passes its physical address in RCX
 * to _start.  The kernel must preserve and re-map this structure before accessing
 * any field after paging is enabled.  BOOT_INFO_VERSION guards against mismatches
 * if the bootloader and kernel are built separately. */
#ifndef WASMOS_BOOT_H
#define WASMOS_BOOT_H

#include <stdint.h>

#define BOOT_INFO_VERSION 4u /* increment on any boot_info_t field change */
/* boot_info_t.flags.  The three PRESENT bits say whether the corresponding
 * pointer/size pair is meaningful; a clear bit means the kernel must not read the
 * pointer at all.  The top nibble of the low 12 bits carries the UEFI
 * EFI_GRAPHICS_PIXEL_FORMAT of the GOP mode, extracted with the SHIFT/MASK pair, and is
 * meaningful only when GOP_PRESENT is set. */
#define BOOT_INFO_FLAG_GOP_PRESENT (1u << 0)
#define BOOT_INFO_FLAG_MODULES_PRESENT (1u << 1)
#define BOOT_INFO_FLAG_INITFS_PRESENT (1u << 2)
#define BOOT_INFO_FLAG_GOP_PIXEL_FORMAT_SHIFT 8u
#define BOOT_INFO_FLAG_GOP_PIXEL_FORMAT_MASK (0xFu << BOOT_INFO_FLAG_GOP_PIXEL_FORMAT_SHIFT)

/* Payload kind of a boot_module_t.  Only WASMOS_APP modules are produced today; the
 * bootloader builds one per initfs entry of that type. */
typedef enum {
    BOOT_MODULE_TYPE_NONE = 0,
    BOOT_MODULE_TYPE_WASMOS_APP = 1 /* embedded .wap payload */
} boot_module_type_t;

/* One payload handed to the kernel at boot.  `base` is a PHYSICAL address (the module
 * bytes live in the boot-allocated block boot_info_t points into) and `size` is in bytes.
 * `type` is a boot_module_type_t.  `name` is the initfs path of the entry, truncated to
 * fit and NUL-terminated. */
typedef struct {
    uint64_t base;
    uint64_t size;
    uint32_t type;
    uint32_t reserved;
    char name[48];
} boot_module_t;

/* initfs image: an 8-byte magic (not NUL-terminated on the wire), a fixed header, a flat
 * entry table, then the payload bytes the entries point into.  The bootloader validates
 * the whole image before copying it, rejecting anything whose version, header_size or
 * entry_size differs from these definitions or whose entries reach outside total_size —
 * so the sizes of the two structs below are part of the format, not an implementation
 * detail. */
#define WASMOS_INITFS_MAGIC "WMINITFS"
#define WASMOS_INITFS_VERSION 1u

typedef enum {
    WASMOS_INITFS_ENTRY_NONE = 0,
    WASMOS_INITFS_ENTRY_WASMOS_APP = 1,
    WASMOS_INITFS_ENTRY_CONFIG = 2,
    WASMOS_INITFS_ENTRY_DATA = 3
} wasmos_initfs_entry_type_t;

/* Marks the entry the boot chain should start from before a filesystem exists. */
#define WASMOS_INITFS_ENTRY_FLAG_BOOTSTRAP (1u << 0)

/* entry_count entries of entry_size bytes follow immediately after header_size bytes.
 * total_size is the length of the whole image, payload included, and must not exceed the
 * file the bootloader read. */
typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t total_size;
    uint32_t reserved;
} wasmos_initfs_header_t;

/* `type` is a wasmos_initfs_entry_type_t and `flags` a mask of
 * WASMOS_INITFS_ENTRY_FLAG_*.  `offset` is a byte offset from the start of the initfs
 * image (never into the header or entry table) and `size` the payload length; the two
 * must stay inside total_size.  `path` is the entry's absolute path, NUL-terminated. */
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t flags;
    uint32_t offset;
    uint32_t size;
    char path[96];
} wasmos_initfs_entry_t;

/* Everything the bootloader hands the kernel.  All pointer fields are PHYSICAL addresses
 * valid at the moment of the handoff — the kernel must re-map them (or read them while
 * the identity map is still live) before dereferencing any of them once paging is on.
 * The whole structure and everything it points at live in one boot-allocated block that
 * the kernel must not free.  Fields guarded by a BOOT_INFO_FLAG_*_PRESENT bit are 0/NULL
 * when the bit is clear. */
typedef struct {
    uint32_t version; /* BOOT_INFO_VERSION the bootloader was built against */
    uint32_t size;    /* sizeof(boot_info_t) as the bootloader saw it */
    uint32_t flags;   /* BOOT_INFO_FLAG_* mask */
    /* UEFI memory map snapshot taken immediately before ExitBootServices.  The map is an
     * array of memory_map_size / memory_desc_size descriptors, each memory_desc_size
     * bytes — which is NOT necessarily sizeof(EFI_MEMORY_DESCRIPTOR), so descriptors must
     * be strided by memory_desc_size, never indexed as an array. */
    void* memory_map;
    uint64_t memory_map_size;
    uint64_t memory_desc_size;
    uint32_t memory_desc_version;
    /* GOP framebuffer, valid only with BOOT_INFO_FLAG_GOP_PRESENT.  Base is physical,
     * size is in bytes, width/height in pixels, and pixels_per_scanline is the stride in
     * PIXELS (>= width), not bytes.  The pixel format is in the flags field. */
    void* framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pixels_per_scanline;
    /* module_count boot_module_t records of module_entry_size bytes each, valid only with
     * BOOT_INFO_FLAG_MODULES_PRESENT.  Stride by module_entry_size so a kernel built
     * against a different struct size still walks the table correctly. */
    void* modules;
    uint32_t module_count;
    uint32_t module_entry_size;
    /* ACPI RSDP found in the UEFI configuration table (ACPI 2.0 preferred, 1.0 as
     * fallback), and its length in bytes.  NULL/0 when the firmware exposed neither. */
    void* rsdp;
    uint32_t rsdp_length;
    /* Copy of the validated initfs image and its length in bytes, valid only with
     * BOOT_INFO_FLAG_INITFS_PRESENT. */
    void* initfs;
    uint32_t initfs_size;
    /* Boot configuration payload, pointing into the initfs image at the CONFIG entry,
     * with its length in bytes.  NULL/0 when the initfs carries no such entry. */
    void* boot_config;
    uint32_t boot_config_size;
} boot_info_t;

#endif
