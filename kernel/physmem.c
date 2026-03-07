#include "physmem.h"
#include "serial.h"

#define PAGE_SIZE 0x1000ULL

#define EFI_MEMORY_TYPE_BOOT_SERVICES_CODE 3
#define EFI_MEMORY_TYPE_BOOT_SERVICES_DATA 4
#define EFI_MEMORY_TYPE_CONVENTIONAL 7

typedef struct {
    uint64_t base;
    uint64_t pages;
} pfa_range_t;

typedef struct {
    uint32_t Type;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} efi_memory_descriptor_t;

static pfa_range_t g_ranges[128];
static uint32_t g_range_count;

static void write_hex(uint64_t value) {
    char buf[21];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write(buf);
}

static int is_usable(uint32_t type) {
    return type == EFI_MEMORY_TYPE_CONVENTIONAL ||
           type == EFI_MEMORY_TYPE_BOOT_SERVICES_CODE ||
           type == EFI_MEMORY_TYPE_BOOT_SERVICES_DATA;
}

static void add_range(uint64_t base, uint64_t pages) {
    if (pages == 0 || g_range_count >= (sizeof(g_ranges) / sizeof(g_ranges[0]))) {
        return;
    }
    if (g_range_count > 0) {
        pfa_range_t *prev = &g_ranges[g_range_count - 1];
        uint64_t prev_end = prev->base + prev->pages * PAGE_SIZE;
        if (prev_end == base) {
            prev->pages += pages;
            return;
        }
    }
    g_ranges[g_range_count].base = base;
    g_ranges[g_range_count].pages = pages;
    g_range_count++;
}

void pfa_init(const boot_info_t *boot_info) {
    g_range_count = 0;
    if (!boot_info || !boot_info->memory_map || boot_info->memory_desc_size == 0) {
        serial_write("[pfa] no memory map\n");
        return;
    }

    serial_write("[pfa] desc size=");
    write_hex(boot_info->memory_desc_size);
    serial_write("[pfa] map size=");
    write_hex(boot_info->memory_map_size);

    uint64_t desc_size = boot_info->memory_desc_size;
    uint64_t count = boot_info->memory_map_size / desc_size;
    if (count > 4096) {
        serial_write("[pfa] map too large, capping descriptors\n");
        count = 4096;
    }

    uint8_t *cursor = (uint8_t *)boot_info->memory_map;
    for (uint64_t i = 0; i < count; ++i) {
        efi_memory_descriptor_t *desc = (efi_memory_descriptor_t *)cursor;
        if (is_usable(desc->Type)) {
            add_range(desc->PhysicalStart, desc->NumberOfPages);
        }
        cursor += desc_size;
    }

    serial_write("[pfa] ranges=");
    write_hex(g_range_count);
}

uint64_t pfa_alloc_pages(uint64_t pages) {
    if (pages == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < g_range_count; ++i) {
        pfa_range_t *range = &g_ranges[i];
        if (range->pages >= pages) {
            uint64_t addr = range->base;
            range->base += pages * PAGE_SIZE;
            range->pages -= pages;
            return addr;
        }
    }
    return 0;
}
