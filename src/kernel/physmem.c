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

extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

static int is_usable(uint32_t type) {
    return type == EFI_MEMORY_TYPE_CONVENTIONAL ||
           type == EFI_MEMORY_TYPE_BOOT_SERVICES_CODE ||
           type == EFI_MEMORY_TYPE_BOOT_SERVICES_DATA;
}

static void add_range(uint64_t base, uint64_t pages) {
    if (pages == 0 || g_range_count >= (sizeof(g_ranges) / sizeof(g_ranges[0]))) {
        return;
    }
    if (base == 0) {
        if (pages <= 1) {
            return;
        }
        base += PAGE_SIZE;
        pages -= 1;
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

static void reserve_range(uint64_t base, uint64_t size) {
    if (size == 0) {
        return;
    }
    uint64_t start = base & ~(PAGE_SIZE - 1ULL);
    uint64_t end = (base + size + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if (end <= start) {
        return;
    }

    uint32_t i = 0;
    while (i < g_range_count) {
        pfa_range_t *range = &g_ranges[i];
        uint64_t range_start = range->base;
        uint64_t range_end = range->base + range->pages * PAGE_SIZE;
        if (end <= range_start || start >= range_end) {
            i++;
            continue;
        }

        if (start <= range_start && end >= range_end) {
            for (uint32_t j = i; j + 1 < g_range_count; ++j) {
                g_ranges[j] = g_ranges[j + 1];
            }
            g_range_count--;
            continue;
        }

        if (start > range_start && end < range_end) {
            if (g_range_count >= (sizeof(g_ranges) / sizeof(g_ranges[0]))) {
                range->pages = (start - range_start) / PAGE_SIZE;
                return;
            }
            uint64_t tail_pages = (range_end - end) / PAGE_SIZE;
            for (uint32_t j = g_range_count; j > i + 1; --j) {
                g_ranges[j] = g_ranges[j - 1];
            }
            g_ranges[i + 1].base = end;
            g_ranges[i + 1].pages = tail_pages;
            range->pages = (start - range_start) / PAGE_SIZE;
            g_range_count++;
            i += 2;
            continue;
        }

        if (start <= range_start) {
            range->base = end;
            range->pages = (range_end - end) / PAGE_SIZE;
            i++;
            continue;
        }

        range->pages = (start - range_start) / PAGE_SIZE;
        i++;
    }
}

void pfa_init(const boot_info_t *boot_info) {
    g_range_count = 0;
    if (!boot_info || !boot_info->memory_map || boot_info->memory_desc_size == 0) {
        serial_write("[pfa] no memory map\n");
        return;
    }

    serial_write("[pfa] init\n");

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

    uint64_t kernel_base = (uint64_t)(uintptr_t)&__kernel_start;
    uint64_t kernel_size = (uint64_t)(uintptr_t)&__kernel_end - kernel_base;
    reserve_range(kernel_base, kernel_size);

    serial_printf("[pfa] ranges=0x%016llX\n", (unsigned long long)g_range_count);

    uint64_t test = pfa_alloc_pages(1);
    serial_printf("[pfa] test alloc=0x%016llX\n", (unsigned long long)test);
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

uint64_t pfa_alloc_pages_below(uint64_t pages, uint64_t max_addr) {
    if (pages == 0 || max_addr == 0) {
        return 0;
    }
    uint64_t limit = max_addr & ~(PAGE_SIZE - 1ULL);
    for (uint32_t i = 0; i < g_range_count; ++i) {
        pfa_range_t *range = &g_ranges[i];
        if (range->pages < pages) {
            continue;
        }
        uint64_t addr = range->base;
        uint64_t end = addr + range->pages * PAGE_SIZE;
        if (addr >= limit) {
            continue;
        }
        if (end > limit) {
            uint64_t usable_pages = (limit - addr) / PAGE_SIZE;
            if (usable_pages < pages) {
                continue;
            }
        }
        range->base += pages * PAGE_SIZE;
        range->pages -= pages;
        return addr;
    }
    return 0;
}

void pfa_free_pages(uint64_t base, uint64_t pages) {
    if (base == 0 || pages == 0) {
        return;
    }
    uint32_t insert = 0;
    while (insert < g_range_count && g_ranges[insert].base < base) {
        insert++;
    }

    if (g_range_count >= (sizeof(g_ranges) / sizeof(g_ranges[0]))) {
        return;
    }

    for (uint32_t i = g_range_count; i > insert; --i) {
        g_ranges[i] = g_ranges[i - 1];
    }
    g_ranges[insert].base = base;
    g_ranges[insert].pages = pages;
    g_range_count++;

    if (insert > 0) {
        pfa_range_t *prev = &g_ranges[insert - 1];
        if (prev->base + prev->pages * PAGE_SIZE == g_ranges[insert].base) {
            prev->pages += g_ranges[insert].pages;
            for (uint32_t i = insert; i + 1 < g_range_count; ++i) {
                g_ranges[i] = g_ranges[i + 1];
            }
            g_range_count--;
            insert--;
        }
    }

    if (insert + 1 < g_range_count) {
        pfa_range_t *cur = &g_ranges[insert];
        pfa_range_t *next = &g_ranges[insert + 1];
        if (cur->base + cur->pages * PAGE_SIZE == next->base) {
            cur->pages += next->pages;
            for (uint32_t i = insert + 1; i + 1 < g_range_count; ++i) {
                g_ranges[i] = g_ranges[i + 1];
            }
            g_range_count--;
        }
    }
}
