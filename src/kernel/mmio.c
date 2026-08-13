/* mmio.c - Kernel-mediated MMIO register writes. See mmio.h for why this exists
 * and why it is scoped the way it is. */
#include "mmio.h"
#include "paging.h"
#include "physmem.h"
#include "sync/spinlock.h"
#include "wasmos_status.h"

/* Reserved kernel VA at PT_A index 253, physical 0xFD000 — the BIOS ROM region,
 * guaranteed PFA-unreachable. Sits directly below the LAPIC (0x...FE000) and
 * IOAPIC (0x...FF000) windows; see the LAPIC_VIRT_BASE rationale in lapic.c. */
#define MMIO_SCRATCH_VIRT_BASE 0xFFFFFFFF800FD000ULL

/* Raw x86_64 PTE bits.  paging_map_4k() does NOT take PTE bits — it takes
 * MEM_REGION_FLAG_* and builds the PTE itself — and the two encodings only
 * happen to agree on PRESENT/WRITE (bits 0 and 1 are READ/WRITE there).
 * FIXME: PT_FLAG_PCD is therefore dropped on the way in and the scratch PTE is
 * built without cache-disable, leaving the effective memory type of this device
 * page to the MTRRs alone. paging_map_4k needs a cache-disable flag to express
 * it. (PT_FLAG_NX is dropped too, but paging_map_4k sets NX for any non-EXEC
 * mapping, so that bit ends up correct anyway.) */
#define PT_FLAG_PRESENT (1ULL << 0)
#define PT_FLAG_WRITE (1ULL << 1)
#define PT_FLAG_PCD (1ULL << 4)
#define PT_FLAG_NX (1ULL << 63)

/* One scratch VA is shared by every caller, so the map/write/unmap sequence must
 * not interleave. The lock also holds IF=0, keeping the window from being seen
 * by an interrupt that lands mid-sequence on this CPU. */
static ksync_spinlock_t g_mmio_scratch_lock;
static uint8_t g_mmio_scratch_ready;

int mmio_write32_phys(uint64_t phys, uint32_t value) {
    if (phys == 0 || (phys & 0x3u) != 0) {
        return WASMOS_ERR_MSI_BAD_DEVICE;
    }
    /* Device registers are never system memory. Refusing the overlap is what
     * keeps a compromised or buggy bus driver from turning this into an
     * arbitrary kernel-memory write. */
    if (pfa_range_overlaps_ram(phys, 4)) {
        return WASMOS_ERR_MSI_BAD_DEVICE;
    }

    if (!g_mmio_scratch_ready) {
        ksync_spinlock_init(&g_mmio_scratch_lock);
        g_mmio_scratch_ready = 1;
    }

    uint64_t page = phys & ~0xFFFULL;
    uint64_t offset = phys & 0xFFFULL;
    uint64_t flags = PT_FLAG_PRESENT | PT_FLAG_WRITE | PT_FLAG_PCD | PT_FLAG_NX;

    ksync_spinlock_lock(&g_mmio_scratch_lock);
    int rc = paging_map_4k(MMIO_SCRATCH_VIRT_BASE, page, flags);
    if (rc != 0) {
        ksync_spinlock_unlock(&g_mmio_scratch_lock);
        return WASMOS_ERR_MSI_MAP_FAILED;
    }
    paging_invalidate(MMIO_SCRATCH_VIRT_BASE);
    *(volatile uint32_t*)(MMIO_SCRATCH_VIRT_BASE + offset) = value;
    /* Read the write back so it has left the CPU before the mapping goes away.
     * The value is discarded; only the ordering matters. */
    (void)*(volatile uint32_t*)(MMIO_SCRATCH_VIRT_BASE + offset);
    (void)paging_unmap_4k(MMIO_SCRATCH_VIRT_BASE);
    paging_invalidate(MMIO_SCRATCH_VIRT_BASE);
    ksync_spinlock_unlock(&g_mmio_scratch_lock);
    return 0;
}
