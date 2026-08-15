/* physmem.c - Physical page frame allocator.
 *
 * Two structures, not one bitmap: g_ranges is a sorted list of FREE extents
 * (allocation carves from it, freeing merges back into it), and g_refcount is a
 * per-frame byte reference count used to catch double frees and to let several
 * owners pin the same frame.  Both are derived from the UEFI memory map, taking
 * EfiConventionalMemory plus the boot-services code/data regions the firmware
 * no longer needs after ExitBootServices (is_usable).  The kernel image and the
 * AP trampoline page are carved out of the free list by pfa_init; everything the
 * firmware did not report as usable (ACPI tables, MMIO, runtime services) is
 * simply never added. */
#include "physmem.h"
#include "paging.h"
#include "klog.h"
#include "kpanic.h"
#include "sync/spinlock.h"
#include "string.h"

#define PAGE_SIZE 0x1000ULL

/*
 * Small static array covering the first 64 MB of physical RAM.
 * Active from the moment physmem.c is loaded, before the EFI memory map
 * is available. pfa_upgrade_refcount() replaces it with a correctly-sized
 * dynamic allocation once the map has been scanned.
 */
#define PFA_STATIC_TRACKED_PAGES (64ULL * 1024 * 1024 / 4096) /* 16384 pages, 16 KB BSS */
static uint8_t g_refcount_static[PFA_STATIC_TRACKED_PAGES];

static uint8_t* g_refcount = g_refcount_static;
static uint64_t g_tracked_pages = PFA_STATIC_TRACKED_PAGES;

#define PFA_BUG(msg, addr)                                                                         \
    do {                                                                                           \
        klog_printf("[pfa] BUG: " msg " phys=0x%016llX\n", (unsigned long long)(addr));            \
        kpanic(msg, (uintptr_t)addr, 0ULL);                                                        \
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
static ksync_spinlock_t g_pfa_lock;
static uint64_t g_initial_total_pages;

/* The usable-RAM extents as the firmware reported them, kept separately from
 * g_ranges (which is the FREE list and shrinks as pages are handed out). Callers
 * asking "is this physical address system memory?" need the original map, not
 * what happens to be free right now. */
static pfa_range_t g_ram_ranges[128];
/* Set if any usable-RAM extent could not be recorded; see add_ram_range. */
static uint8_t g_ram_ranges_truncated;
static uint32_t g_ram_range_count;

extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

static int is_usable(uint32_t type) {
    return type == EFI_MEMORY_TYPE_CONVENTIONAL || type == EFI_MEMORY_TYPE_BOOT_SERVICES_CODE ||
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
    g_initial_total_pages += pages;
    if (g_range_count > 0) {
        pfa_range_t* prev = &g_ranges[g_range_count - 1];
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

/* Record a usable-RAM extent verbatim (no page-0 trimming, no reservations
 * carved out): this list answers "was this ever system memory?", so it must stay
 * a superset of everything the allocator might hand out. */
static void add_ram_range(uint64_t base, uint64_t pages) {
    if (pages == 0) {
        return;
    }
    if (g_ram_range_count >= (sizeof(g_ram_ranges) / sizeof(g_ram_ranges[0]))) {
        /* Dropping an extent makes this list an INCOMPLETE answer to "was this
         * ever system memory?", and the gate below is only safe while it is a
         * superset.  Record the truncation so pfa_range_overlaps_ram can refuse
         * instead of reporting "not RAM" for memory it simply failed to note. */
        g_ram_ranges_truncated = 1;
        return;
    }
    if (g_ram_range_count > 0) {
        pfa_range_t* prev = &g_ram_ranges[g_ram_range_count - 1];
        if (prev->base + prev->pages * PAGE_SIZE == base) {
            prev->pages += pages;
            return;
        }
    }
    g_ram_ranges[g_ram_range_count].base = base;
    g_ram_ranges[g_ram_range_count].pages = pages;
    g_ram_range_count++;
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
        pfa_range_t* range = &g_ranges[i];
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
        pfa_range_t* prev = &g_ranges[insert - 1];
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
        pfa_range_t* cur = &g_ranges[insert];
        pfa_range_t* next = &g_ranges[insert + 1];
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
static int pfa_upgrade_refcount(uint64_t* out_pages, uint64_t* out_alloc_pages) {
    uint64_t max_phys = 0;
    for (uint32_t i = 0; i < g_range_count; i++) {
        uint64_t end = g_ranges[i].base + g_ranges[i].pages * PAGE_SIZE;
        if (end > max_phys)
            max_phys = end;
    }
    if (max_phys == 0) {
        return 0;
    }

    uint64_t needed_pages = (max_phys + PAGE_SIZE - 1) / PAGE_SIZE;
    if (needed_pages <= g_tracked_pages) {
        return 0;
    }

    /* One byte of refcount per tracked frame, so the array is needed_pages BYTES
     * long and occupies that many bytes rounded up to whole frames. */
    uint64_t rc_alloc_pages = (needed_pages + PAGE_SIZE - 1) / PAGE_SIZE;
    if (out_alloc_pages)
        *out_alloc_pages = rc_alloc_pages;
    if (out_pages)
        *out_pages = needed_pages;

    uint64_t rc_phys = pfa_alloc_pages_nolock(rc_alloc_pages);
    if (rc_phys == 0) {
        return -1;
    }

    uint8_t* dyn = ptr_cast(uint8_t, (rc_phys + KERNEL_HIGHER_HALF_BASE));
    memset(dyn, 0, (size_t)needed_pages);
    memcpy(dyn, g_refcount_static, (size_t)PFA_STATIC_TRACKED_PAGES);

    /* Mark the array's own backing pages as in-use regardless of where they landed. */
    for (uint64_t i = 0; i < rc_alloc_pages; i++) {
        uint64_t idx = (rc_phys + i * PAGE_SIZE) >> 12;
        if (idx < needed_pages)
            dyn[idx] = 1;
    }

    g_refcount = dyn;
    g_tracked_pages = needed_pages;
    return 1;
}

/* Builds both structures from the UEFI memory map handed over by the bootloader
 * and must run before any other pfa_* call.  Runs once on the BSP; the lock it
 * initialises here protects everything afterwards.
 *
 * boot_info->memory_map is borrowed for the duration of the call and is read
 * through whatever mapping is active at the time (the identity map during early
 * boot).  A missing or empty map leaves the allocator with zero ranges, so every
 * later allocation returns 0 rather than failing loudly.  At most 4096
 * descriptors are examined; a larger map is truncated, and extents beyond
 * g_ranges' 128 entries are silently dropped by add_range.
 *
 * Physical page 0 is trimmed off so a NULL-looking frame is never handed out,
 * and the kernel image and the 0x1000 AP trampoline page are carved back out of
 * the free list.
 *
 * Panics if the refcount array cannot be upgraded to cover all of RAM, since
 * every later free would then be unchecked past the static 64 MiB window. */
void pfa_init(const boot_info_t* boot_info) {
    ksync_spinlock_init(&g_pfa_lock);
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

    uint8_t* cursor = (uint8_t*)boot_info->memory_map;
    for (uint64_t i = 0; i < count; ++i) {
        efi_memory_descriptor_t* desc = (efi_memory_descriptor_t*)cursor;
        if (is_usable(desc->Type)) {
            add_range(desc->PhysicalStart, desc->NumberOfPages);
            add_ram_range(desc->PhysicalStart, desc->NumberOfPages);
        }
        cursor += desc_size;
    }

    uint64_t kernel_base = addr_cast(uint64_t, &__kernel_start);
    uint64_t kernel_size = addr_cast(uint64_t, &__kernel_end) - kernel_base;
    reserve_range(kernel_base, kernel_size);
    /* Keep the fixed AP trampoline page out of the general allocator. */
    reserve_range(0x1000ULL, PAGE_SIZE);

    klog_printf("[pfa] ranges=0x%016llX\n", (unsigned long long)g_range_count);

    /* Upgrade refcount array and do a test alloc under lock, then log results
     * outside the lock to avoid klog_* → mm_shared_create → pfa_alloc_pages
     * re-entry on g_pfa_lock. */
    uint64_t rc_pages = 0, rc_alloc_pages = 0;
    ksync_spinlock_lock(&g_pfa_lock);
    int rc_status = pfa_upgrade_refcount(&rc_pages, &rc_alloc_pages);
    uint64_t test = pfa_alloc_pages_nolock(1);
    ksync_spinlock_unlock(&g_pfa_lock);

    if (rc_status < 0) {
        klog_printf("[pfa] refcount upgrade failed: needed %llu pages\n",
                    (unsigned long long)rc_alloc_pages);
        kpanic("refcount_upgrade_failed", rc_pages, rc_alloc_pages);
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
        pfa_range_t* range = &g_ranges[i];
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

/* First-fit from the front of the first free extent that is large enough, so the
 * result is `pages` PHYSICALLY CONTIGUOUS frames.  The returned value is a
 * physical address with no mapping attached: reach it through the kernel
 * higher-half alias, or map it, before dereferencing.
 *
 * Each frame's refcount is SET to 1 rather than incremented, so a caller holds
 * exactly one reference and one pfa_free_pages of the same range releases it.
 * Frames beyond the tracked window carry no count at all.
 *
 * Returns 0 for a zero page count and when no extent can satisfy the request;
 * there is no partial allocation.  Takes g_pfa_lock, so this must not be called
 * from anything already holding it (klog_* can reach the allocator — see the
 * deferred logging in pfa_init). */
uint64_t pfa_alloc_pages(uint64_t pages) {
    if (pages == 0) {
        return 0;
    }
    ksync_spinlock_lock(&g_pfa_lock);
    uint64_t addr = pfa_alloc_pages_nolock(pages);
    ksync_spinlock_unlock(&g_pfa_lock);
    return addr;
}

/* pfa_alloc_pages restricted to a physical ceiling: the whole run lies below
 * max_addr, which is rounded down to a page boundary.  Callers use it for memory
 * that must be reachable through a limited window — page tables must land inside
 * the shared higher-half window, and some devices cannot address high memory.
 *
 * Allocation always carves from the FRONT of a candidate extent, so an extent
 * that starts at or above the ceiling is skipped entirely even if it is the only
 * one large enough.  Returns 0 for a zero page count, a zero ceiling, or when no
 * extent qualifies. */
uint64_t pfa_alloc_pages_below(uint64_t pages, uint64_t max_addr) {
    if (pages == 0 || max_addr == 0) {
        return 0;
    }
    ksync_spinlock_lock(&g_pfa_lock);
    uint64_t limit = max_addr & ~(PAGE_SIZE - 1ULL);
    for (uint32_t i = 0; i < g_range_count; ++i) {
        pfa_range_t* range = &g_ranges[i];
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
        ksync_spinlock_unlock(&g_pfa_lock);
        return addr;
    }
    ksync_spinlock_unlock(&g_pfa_lock);
    return 0;
}

/* pfa_alloc_pages restricted to a physical floor: the whole run starts at or
 * above min_addr, rounded up to a page boundary.
 *
 * Unlike the _below variant this can carve from the front, the back, or the
 * MIDDLE of an extent, splitting it into two free extents.  When the 128-entry
 * extent table is already full the split is not possible and the lower remnant
 * is discarded instead (front-allocation), which loses those frames from the
 * free list rather than failing the request.
 *
 * Returns 0 for a zero page count and when no extent has a run of `pages`
 * contiguous frames at or above the floor. */
uint64_t pfa_alloc_pages_above(uint64_t pages, uint64_t min_addr) {
    if (pages == 0) {
        return 0;
    }
    ksync_spinlock_lock(&g_pfa_lock);
    uint64_t floor = (min_addr + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    for (uint32_t i = 0; i < g_range_count; ++i) {
        pfa_range_t* range = &g_ranges[i];
        uint64_t rbase = range->base;
        uint64_t rend = rbase + range->pages * PAGE_SIZE;
        uint64_t start = rbase < floor ? floor : rbase;
        if (start + pages * PAGE_SIZE > rend) {
            continue;
        }
        uint64_t alloc_end = start + pages * PAGE_SIZE;
        if (start == rbase) {
            /* Allocate from the front of the range. */
            range->base = alloc_end;
            range->pages = (rend - alloc_end) / PAGE_SIZE;
            if (range->pages == 0) {
                for (uint32_t j = i; j + 1 < g_range_count; ++j)
                    g_ranges[j] = g_ranges[j + 1];
                g_range_count--;
            }
        } else if (alloc_end == rend) {
            /* Allocate from the back of the range. */
            range->pages = (start - rbase) / PAGE_SIZE;
        } else {
            /* Allocate from the middle — split into lower + upper remnants.
             * Reuse the current slot for the lower part; insert upper after it. */
            if (g_range_count >= (sizeof(g_ranges) / sizeof(g_ranges[0]))) {
                /* Range table full: fall back to front-allocating (trims lower). */
                range->base = alloc_end;
                range->pages = (rend - alloc_end) / PAGE_SIZE;
            } else {
                uint64_t upper_pages = (rend - alloc_end) / PAGE_SIZE;
                /* Shift entries after i to make room for the upper remnant. */
                for (uint32_t j = g_range_count; j > i + 1; --j)
                    g_ranges[j] = g_ranges[j - 1];
                g_ranges[i + 1].base = alloc_end;
                g_ranges[i + 1].pages = upper_pages;
                g_range_count++;
                range->pages = (start - rbase) / PAGE_SIZE;
            }
        }
        for (uint64_t j = 0; j < pages; j++) {
            uint64_t idx = (start + j * PAGE_SIZE) >> 12;
            if (idx < g_tracked_pages)
                g_refcount[idx] = 1;
        }
        ksync_spinlock_unlock(&g_pfa_lock);
        return start;
    }
    ksync_spinlock_unlock(&g_pfa_lock);
    return 0;
}

/* Drop one reference to each frame in [base, base+pages).  A frame returns to
 * the free list only when its count reaches 0, so this doubles as the unpin
 * operation paired with pfa_pin_pages.  Freeing a frame whose count is already 0
 * is a bug and panics.  Frames outside the tracked window carry no count and go
 * straight back to the free list. */
void pfa_free_pages(uint64_t base, uint64_t pages) {
    if (base == 0 || pages == 0) {
        return;
    }
    ksync_spinlock_lock(&g_pfa_lock);
    uint64_t run_start = 0, run_len = 0;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = base + i * PAGE_SIZE;
        uint64_t idx = phys >> 12;
        if (idx < g_tracked_pages) {
            if (g_refcount[idx] == 0)
                PFA_BUG("double-free", phys);
            if (--g_refcount[idx] == 0) {
                if (run_len == 0)
                    run_start = phys;
                run_len++;
            } else {
                if (run_len) {
                    pfa_insert_range(run_start, run_len);
                    run_len = 0;
                }
            }
        } else {
            /* Outside tracked window — pass straight through to free pool. */
            if (run_len == 0)
                run_start = phys;
            run_len++;
        }
    }
    if (run_len) {
        pfa_insert_range(run_start, run_len);
    }
    ksync_spinlock_unlock(&g_pfa_lock);
}

/* Take one additional reference on each frame in [base, base+pages), so the
 * frame survives until every holder calls pfa_free_pages.  The frames must
 * already be allocated: pinning a free frame, or overflowing the 8-bit count,
 * panics.  Frames outside the tracked window are ignored. */
void pfa_pin_pages(uint64_t base, uint64_t pages) {
    if (base == 0 || pages == 0) {
        return;
    }
    ksync_spinlock_lock(&g_pfa_lock);
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
    ksync_spinlock_unlock(&g_pfa_lock);
}

/* Usable RAM as pfa_init accounted it: the sum of every extent that entered the
 * free list, so page 0 is excluded and extents dropped for want of a g_ranges
 * slot are not counted.  This is a boot-time constant — allocation does not
 * reduce it — and it is read without the lock. */
uint64_t pfa_total_bytes(void) {
    return g_initial_total_pages * PAGE_SIZE;
}

/* Fail-closed test of whether [base, base+length) touches firmware-reported
 * system RAM.  Non-zero means "treat this as RAM"; 0 means "definitely not RAM",
 * and only that answer permits a caller such as mmio_write32_phys to proceed.
 *
 * Non-zero is therefore also the answer for cases where no honest answer exists:
 * an address range that wraps, a memory map that was never parsed, and a RAM
 * list that overflowed its 128 entries.  A zero length is the one benign case
 * and returns 0.
 *
 * Answered from the original usable-RAM extents, not the free list, so a frame
 * currently handed out still counts as RAM. */
int pfa_range_overlaps_ram(uint64_t base, uint64_t length) {
    if (length == 0) {
        return 0;
    }
    uint64_t end = base + length;
    if (end < base) {
        return 1; /* wrapped — treat as overlapping so the caller refuses */
    }
    /* An unparsed map carries no information, so refuse rather than allow.
     * A truncated list is the same situation with the same answer: the retained
     * ranges are not a superset of system RAM, so "no overlap found" would be a
     * guess, and this gate exists to stop mmio_write32_phys writing into RAM. */
    if (g_ram_range_count == 0 || g_ram_ranges_truncated) {
        return 1;
    }
    for (uint32_t i = 0; i < g_ram_range_count; ++i) {
        uint64_t r_base = g_ram_ranges[i].base;
        uint64_t r_end = r_base + g_ram_ranges[i].pages * PAGE_SIZE;
        if (base < r_end && r_base < end) {
            return 1;
        }
    }
    return 0;
}

/* Sums the free extents under the lock, so the figure is consistent but stale
 * the moment it is returned.  Counts only what is on the free list, so neither a
 * frame whose refcount has yet to reach 0 nor one dropped for want of an extent
 * slot appears here. */
uint64_t pfa_free_bytes(void) {
    ksync_spinlock_lock(&g_pfa_lock);
    uint64_t free_pages = 0;
    for (uint32_t i = 0; i < g_range_count; ++i) {
        free_pages += g_ranges[i].pages;
    }
    ksync_spinlock_unlock(&g_pfa_lock);
    return free_pages * PAGE_SIZE;
}
