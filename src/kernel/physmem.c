#include "physmem.h"
#include "paging.h"
#include "klog.h"
#include "spinlock.h"
#include "string.h"

#define PAGE_SIZE 0x1000ULL

/*
 * Small static array covering the first 64 MB of physical RAM.
 * Active from the moment physmem.c is loaded, before the EFI memory map
 * is available. pfa_upgrade_refcount() replaces it with a correctly-sized
 * dynamic allocation once the map has been scanned.
 */
#define PFA_STATIC_TRACKED_PAGES (64ULL * 1024 * 1024 / 4096)  /* 16384 pages, 16 KB BSS */
static uint8_t g_refcount_static[PFA_STATIC_TRACKED_PAGES];

static uint8_t *g_refcount      = g_refcount_static;
static uint64_t g_tracked_pages = PFA_STATIC_TRACKED_PAGES;

#define PFA_BUG(msg, addr) do { \
    klog_printf("[pfa] BUG: " msg " phys=0x%016llX\n", (unsigned long long)(addr)); \
    for (;;) { __asm__ volatile("hlt"); } \
} while (0)

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
static spinlock_t g_pfa_lock;

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

static void pfa_insert_range(uint64_t base, uint64_t pages) {
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

static uint64_t pfa_alloc_pages_nolock(uint64_t pages);

/*
 * Called once from pfa_init after the free-range list is built, while
 * g_pfa_lock is held.  Returns 0 if no upgrade was needed, fills *out_pages
 * and *out_alloc_pages on success (return > 0), or returns -1 on alloc failure
 * (fills *out_alloc_pages so the caller can log/panic without holding the lock).
 * Must NOT call klog_* — caller logs results after releasing g_pfa_lock.
 */
static int pfa_upgrade_refcount(uint64_t *out_pages, uint64_t *out_alloc_pages) {
    uint64_t max_phys = 0;
    for (uint32_t i = 0; i < g_range_count; i++) {
        uint64_t end = g_ranges[i].base + g_ranges[i].pages * PAGE_SIZE;
        if (end > max_phys) max_phys = end;
    }
    if (max_phys == 0) {
        return 0;
    }

    uint64_t needed_pages = (max_phys + PAGE_SIZE - 1) / PAGE_SIZE;
    if (needed_pages <= g_tracked_pages) {
        return 0;
    }

    uint64_t rc_alloc_pages = (needed_pages + PAGE_SIZE - 1) / PAGE_SIZE;
    if (out_alloc_pages) *out_alloc_pages = rc_alloc_pages;
    if (out_pages)       *out_pages       = needed_pages;

    uint64_t rc_phys = pfa_alloc_pages_nolock(rc_alloc_pages);
    if (rc_phys == 0) {
        return -1;
    }

    uint8_t *dyn = (uint8_t *)(uintptr_t)(rc_phys + KERNEL_HIGHER_HALF_BASE);
    memset(dyn, 0, (size_t)needed_pages);
    memcpy(dyn, g_refcount_static, (size_t)PFA_STATIC_TRACKED_PAGES);

    /* Mark the array's own backing pages as in-use regardless of where they landed. */
    for (uint64_t i = 0; i < rc_alloc_pages; i++) {
        uint64_t idx = (rc_phys + i * PAGE_SIZE) >> 12;
        if (idx < needed_pages)
            dyn[idx] = 1;
    }

    g_refcount      = dyn;
    g_tracked_pages = needed_pages;
    return 1;
}

void pfa_init(const boot_info_t *boot_info) {
    spinlock_init(&g_pfa_lock);
    g_range_count = 0;
    if (!boot_info || !boot_info->memory_map || boot_info->memory_desc_size == 0) {
        klog_write("[pfa] no memory map\n");
        return;
    }

    klog_write("[pfa] init\n");

    uint64_t desc_size = boot_info->memory_desc_size;
    uint64_t count = boot_info->memory_map_size / desc_size;
    if (count > 4096) {
        klog_write("[pfa] map too large, capping descriptors\n");
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
    /* Keep the fixed AP trampoline page out of the general allocator. */
    reserve_range(0x1000ULL, PAGE_SIZE);

    klog_printf("[pfa] ranges=0x%016llX\n", (unsigned long long)g_range_count);

    /* Upgrade refcount array and do a test alloc under lock, then log results
     * outside the lock to avoid klog_* → mm_shared_create → pfa_alloc_pages
     * re-entry on g_pfa_lock. */
    uint64_t rc_pages = 0, rc_alloc_pages = 0;
    spinlock_lock(&g_pfa_lock);
    int rc_status = pfa_upgrade_refcount(&rc_pages, &rc_alloc_pages);
    uint64_t test = pfa_alloc_pages_nolock(1);
    spinlock_unlock(&g_pfa_lock);

    if (rc_status < 0) {
        klog_printf("[pfa] refcount upgrade failed: needed %llu pages\n",
                    (unsigned long long)rc_alloc_pages);
        for (;;) { __asm__ volatile("hlt"); }
    }
    if (rc_status > 0) {
        klog_printf("[pfa] refcount upgraded: %llu pages tracked (%llu KB)\n",
                    (unsigned long long)rc_pages,
                    (unsigned long long)(rc_pages / 1024));
    }
    klog_printf("[pfa] test alloc=0x%016llX\n", (unsigned long long)test);
}

static uint64_t pfa_alloc_pages_nolock(uint64_t pages) {
    for (uint32_t i = 0; i < g_range_count; ++i) {
        pfa_range_t *range = &g_ranges[i];
        if (range->pages >= pages) {
            uint64_t addr = range->base;
            range->base += pages * PAGE_SIZE;
            range->pages -= pages;
            for (uint64_t j = 0; j < pages; j++) {
                uint64_t idx = (addr + j * PAGE_SIZE) >> 12;
                if (idx < g_tracked_pages)
                    g_refcount[idx] = 1;
            }
            return addr;
        }
    }
    return 0;
}

uint64_t pfa_alloc_pages(uint64_t pages) {
    if (pages == 0) {
        return 0;
    }
    spinlock_lock(&g_pfa_lock);
    uint64_t addr = pfa_alloc_pages_nolock(pages);
    spinlock_unlock(&g_pfa_lock);
    return addr;
}

uint64_t pfa_alloc_pages_below(uint64_t pages, uint64_t max_addr) {
    if (pages == 0 || max_addr == 0) {
        return 0;
    }
    spinlock_lock(&g_pfa_lock);
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
        for (uint64_t j = 0; j < pages; j++) {
            uint64_t idx = (addr + j * PAGE_SIZE) >> 12;
            if (idx < g_tracked_pages)
                g_refcount[idx] = 1;
        }
        spinlock_unlock(&g_pfa_lock);
        return addr;
    }
    spinlock_unlock(&g_pfa_lock);
    return 0;
}

void pfa_free_pages(uint64_t base, uint64_t pages) {
    if (base == 0 || pages == 0) {
        return;
    }
    spinlock_lock(&g_pfa_lock);
    uint64_t run_start = 0, run_len = 0;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = base + i * PAGE_SIZE;
        uint64_t idx  = phys >> 12;
        if (idx < g_tracked_pages) {
            if (g_refcount[idx] == 0)
                PFA_BUG("double-free", phys);
            if (--g_refcount[idx] == 0) {
                if (run_len == 0) run_start = phys;
                run_len++;
            } else {
                if (run_len) {
                    pfa_insert_range(run_start, run_len);
                    run_len = 0;
                }
            }
        } else {
            /* Outside tracked window — pass straight through to free pool. */
            if (run_len == 0) run_start = phys;
            run_len++;
        }
    }
    if (run_len) {
        pfa_insert_range(run_start, run_len);
    }
    spinlock_unlock(&g_pfa_lock);
}

void pfa_pin_pages(uint64_t base, uint64_t pages) {
    if (base == 0 || pages == 0) {
        return;
    }
    spinlock_lock(&g_pfa_lock);
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t idx = (base + i * PAGE_SIZE) >> 12;
        if (idx < g_tracked_pages) {
            if (g_refcount[idx] == 0)
                PFA_BUG("pin of free page", base + i * PAGE_SIZE);
            if (g_refcount[idx] == 255)
                PFA_BUG("refcount overflow", base + i * PAGE_SIZE);
            g_refcount[idx]++;
        }
    }
    spinlock_unlock(&g_pfa_lock);
}
