/* physmem.h - Physical page frame allocator (PFA).
 * Two structures, not a bitmap: a sorted list of FREE 4 KB extents that
 * allocation carves from and freeing merges back into, and a per-frame
 * reference count that catches double frees and lets several owners hold the
 * same frame.  Both derive from the UEFI memory map.  All returned addresses
 * are 4 KB-aligned physical frames. */
#ifndef WASMOS_PHYSMEM_H
#define WASMOS_PHYSMEM_H

#include <stdint.h>
#include "boot.h"

/* Initialize the PFA from the UEFI memory map provided at boot. */
void pfa_init(const boot_info_t* boot_info);

/* Allocate a contiguous run of `pages` physical pages.  Returns the physical
 * base address, or 0 on failure. */
uint64_t pfa_alloc_pages(uint64_t pages);

/* Like pfa_alloc_pages but constrains the allocation below max_addr (e.g. for DMA
 * that requires addresses below 4 GB). */
uint64_t pfa_alloc_pages_below(uint64_t pages, uint64_t max_addr);

/* Allocate a contiguous run of pages with physical base >= min_addr.
 * Used to keep WARP linear memory above the shmem physical zone so that
 * WARP's ensureLinearSize zero-fill cannot alias active shmem pages. */
uint64_t pfa_alloc_pages_above(uint64_t pages, uint64_t min_addr);

/* Drop one reference on each of `pages` frames starting at base. A frame
 * rejoins the free pool only when its count reaches zero, so a frame that was
 * pinned needs as many frees as it has references. Freeing a frame whose count
 * is already zero panics as a double-free. */
void pfa_free_pages(uint64_t base, uint64_t pages);

/* Take one additional reference on each of `pages` frames starting at base, so
 * a second owner's free does not release the frame while the first still uses
 * it. The frames must already be allocated: pinning a free frame panics.
 * Frames outside the tracked refcount window are silently skipped. */
void pfa_pin_pages(uint64_t base, uint64_t pages);

/* Total usable physical bytes reported by the UEFI memory map at boot. */
uint64_t pfa_total_bytes(void);

/* Current free physical bytes (sum of all free ranges under lock). */
uint64_t pfa_free_bytes(void);

/* Non-zero if [base, base+length) overlaps any region the UEFI memory map
 * reported as usable RAM. Device MMIO never does, so this is the predicate that
 * keeps a bus driver's MMIO access (mmio_write32) off system memory. Fails
 * closed: a wrapped range, an unparsed map, and a truncated RAM list all report
 * an overlap, because in none of those cases is "no overlap found" knowable. */
int pfa_range_overlaps_ram(uint64_t base, uint64_t length);

#endif
