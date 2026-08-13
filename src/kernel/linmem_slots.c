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

/* One slot per bit of g_linmem_slot_bitmap.  A slot spans WARP_LINMEM_VA_STRIDE
 * (2 GiB) of reserved VA, i.e. two 1 GiB PDPT entries, so the window holds
 * WARP_LINMEM_PDPT_COUNT/2 slots.
 * TODO: LINMEM_SLOT_COUNT caps CONCURRENT slot holders (concurrent WASM apps,
 * not total spawned); allocation fails once they are all taken.  Raising it past
 * 64 needs the bitmap widened to an array as well as a wider VA window. */
#define LINMEM_SLOT_COUNT (WARP_LINMEM_PDPT_COUNT / 2u)

static uint64_t g_linmem_slot_bitmap = 0;
/* Owning pid per slot, for fault reporting only; 0 = untagged. */
static uint32_t g_linmem_slot_owner[LINMEM_SLOT_COUNT];
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
    g_linmem_slot_owner[slot] = 0;
    ksync_spinlock_unlock(&g_linmem_slot_lock);
}

int linmem_slot_contains(uint64_t va) {
    uint64_t window = (uint64_t)LINMEM_SLOT_COUNT * WARP_LINMEM_VA_STRIDE;
    return (va >= WARP_LINMEM_VA_BASE && va < WARP_LINMEM_VA_BASE + window) ? 1 : 0;
}

void linmem_slot_set_owner(uint32_t slot, uint32_t pid) {
    if (slot >= LINMEM_SLOT_COUNT) {
        return;
    }
    ksync_spinlock_lock(&g_linmem_slot_lock);
    g_linmem_slot_owner[slot] = pid;
    ksync_spinlock_unlock(&g_linmem_slot_lock);
}

int linmem_slot_fault_info(uint64_t va, uint32_t* out_slot, uint32_t* out_owner_pid,
                           uint64_t* out_slot_offset, int* out_reserved, int* out_present) {
    uint64_t window_bytes = (uint64_t)LINMEM_SLOT_COUNT * WARP_LINMEM_VA_STRIDE;
    if (va < WARP_LINMEM_VA_BASE || va >= WARP_LINMEM_VA_BASE + window_bytes) {
        return -1;
    }
    uint64_t rel = va - WARP_LINMEM_VA_BASE;
    uint32_t slot = (uint32_t)(rel / WARP_LINMEM_VA_STRIDE);
    if (out_slot) {
        *out_slot = slot;
    }
    if (out_slot_offset) {
        *out_slot_offset = rel % WARP_LINMEM_VA_STRIDE;
    }
    /* Read without the lock: this runs from the exception path, where the lock
     * holder may be the interrupted CPU itself. */
    if (out_owner_pid) {
        *out_owner_pid = g_linmem_slot_owner[slot];
    }
    if (out_reserved) {
        *out_reserved = (g_linmem_slot_bitmap & (1ULL << slot)) ? 1 : 0;
    }
    if (out_present) {
        *out_present = paging_virt_to_phys(va & ~0xFFFULL) ? 1 : 0;
    }
    return 0;
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
         * unreachable from ring 3, and paging_map_4k_in_root rejects the USER
         * bit on higher-half VAs anyway.
         * TODO: paging_map_4k invalidates the new mapping only on the CPU that
         * installs it, and there is no cross-CPU TLB shootdown here or in
         * linmem_slot_decommit, so a CPU that touched a released slot VA can
         * retain a stale translation once the slot is reused. */
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