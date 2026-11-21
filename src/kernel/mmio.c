/* mmio.c - Kernel-mediated MMIO register writes. See mmio.h for why this exists
 * and why it is scoped the way it is. */
#include "mmio.h"
#include "memory.h"
#include "paging.h"
#include "physmem.h"
#include "sync/spinlock.h"
#include "wasmos_status.h"

/* Reserved kernel VA at PT_A index 253, physical 0xFD000 — the BIOS ROM region,
 * guaranteed PFA-unreachable. Sits directly below the LAPIC (0x...FE000) and
 * IOAPIC (0x...FF000) windows; see the LAPIC_VIRT_BASE rationale in lapic.c. */
#define MMIO_SCRATCH_VIRT_BASE 0xFFFFFFFF800FD000ULL

/* FIXME: MEM_REGION_FLAG_* has no device/uncached memory type, so the scratch
 * mapping is built as ordinary write-back memory and the effective memory type
 * of this device page is left to the platform (the MTRRs on x86_64). paging
 * needs a device-memory flag to express it; an arm64 port needs the same flag
 * to select a Device-nGnRE attribute instead of Normal memory. */

/* One scratch VA is shared by every caller, so the map/write/unmap sequence must
 * not interleave. The lock also masks interrupts, keeping the window from being seen
 * by an interrupt that lands mid-sequence on this CPU. */
static ksync_spinlock_t g_mmio_scratch_lock;
static uint8_t g_mmio_scratch_ready;

/* Writes one 32-bit register at a raw PHYSICAL device address by mapping it into
 * the shared kernel scratch VA, storing, reading back, and unmapping again.
 *
 * phys must be non-zero and 4-byte aligned, and must not overlap system RAM;
 * each of those is refused with WASMOS_ERR_MSI_BAD_DEVICE.  A mapping failure
 * gives WASMOS_ERR_MSI_MAP_FAILED.  Success returns 0.
 *
 * The scratch VA is a single global, so this serialises every caller on
 * g_mmio_scratch_lock and spins there under contention.  The lock is initialised
 * lazily on first use, which assumes the first call is not racing another CPU's
 * first call.  The mapping is torn down before returning, so no caller-visible
 * mapping survives the call. */
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
    uint64_t flags = MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE;

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
