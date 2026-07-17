/* linmem_slots.c - Reserved-VA, lazy-commit linear-memory slot allocator.
 *
 * See linmem_slots.h for the model.  Extracted from the WARP shim so both the
 * WARP and wasm3 backends share one non-relocating linear-memory mechanism. */
#include "linmem_slots.h"

#include "physmem.h"
#include "paging.h"
#include "memory.h"
#include "sync/spinlock.h"

#define LINMEM_PAGE_SIZE 4096ULL

/* One 2 GiB slot per bit.  WARP_LINMEM_PDPT_COUNT/2 slots fit the VA window.
 * TODO(linmem-pool): the bitmap width (64) is the hard ceiling on concurrent
 * slots (bounds concurrent WASM apps, not total spawned).  If a boot ever needs
 * more than LINMEM_SLOT_COUNT concurrent WASM processes, widen the VA window
 * (WARP_LINMEM_PDPT_COUNT) and make the bitmap an array. */
#define LINMEM_SLOT_COUNT (WARP_LINMEM_PDPT_COUNT / 2u)

static uint64_t g_linmem_slot_bitmap = 0;
/* Guards the slot bitmap.  WARP runs one CPU at a time inside the runtime, but
 * wasm3 executes concurrently across CPUs, so the pool must be SMP-safe. */
static ksync_spinlock_t g_linmem_slot_lock;

uint32_t linmem_slot_count(void) {
    return LINMEM_SLOT_COUNT;
}

int linmem_slot_alloc(void) {
    ksync_spinlock_lock(&g_linmem_slot_lock);
    for (uint32_t i = 0; i < LINMEM_SLOT_COUNT; ++i) {
        if (!(g_linmem_slot_bitmap & (1ULL << i))) {
            g_linmem_slot_bitmap |= (1ULL << i);
            ksync_spinlock_unlock(&g_linmem_slot_lock);
            return (int)i;
        }
    }
    ksync_spinlock_unlock(&g_linmem_slot_lock);
    return -1;
}

void linmem_slot_release(uint32_t slot) {
    if (slot >= LINMEM_SLOT_COUNT) {
        return;
    }
    ksync_spinlock_lock(&g_linmem_slot_lock);
    g_linmem_slot_bitmap &= ~(1ULL << slot);
    ksync_spinlock_unlock(&g_linmem_slot_lock);
}

uint64_t linmem_slot_va(uint32_t slot) {
    return WARP_LINMEM_VA_BASE + (uint64_t)slot * WARP_LINMEM_VA_STRIDE;
}

int linmem_slot_commit(uint64_t va_base, uint64_t from_page, uint64_t to_page) {
    for (uint64_t p = from_page; p < to_page; ++p) {
        uint64_t va = va_base + p * LINMEM_PAGE_SIZE;
        uint64_t phys = pfa_alloc_pages(1);
        if (!phys) {
            return -1;
        }
        /* SUPERVISOR only (no MEM_REGION_FLAG_USER): this kernel alias must be
         * unreachable from ring 3.  paging_map_4k invlpg's the mapping CPU; the
         * single-CPU-at-a-time invariant means no cross-CPU shootdown is needed
         * for a just-committed page. */
        if (paging_map_4k(va, phys, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE) != 0) {
            pfa_free_pages(phys, 1);
            return -1;
        }
        /* Zero the possibly-recycled frame via its now-mapped VA: fresh linear
         * memory must read as zero, and this prevents cross-app data leakage. */
        __builtin_memset(ptr_cast(void, va), 0, (size_t)LINMEM_PAGE_SIZE);
    }
    return 0;
}

void linmem_slot_decommit(uint64_t va_base, uint64_t pages) {
    for (uint64_t p = 0; p < pages; ++p) {
        uint64_t va = va_base + p * LINMEM_PAGE_SIZE;
        uint64_t phys = paging_virt_to_phys(va);
        (void)paging_unmap_4k(va);
        if (phys) {
            pfa_free_pages(phys & ~0xFFFULL, 1);
        }
    }
}