/* physmem.h - Physical page frame allocator (PFA).
 * Manages the free pool of 4 KB physical pages using a bitmap derived from
 * the UEFI memory map.  All returned addresses are 4 KB-aligned physical frames. */
#ifndef WASMOS_PHYSMEM_H
#define WASMOS_PHYSMEM_H

#include <stdint.h>
#include "boot.h"

/* Initialize the PFA from the UEFI memory map provided at boot. */
void pfa_init(const boot_info_t *boot_info);

/* Allocate a contiguous run of pages physical pages.  Returns the physical base address,
 * or 0 on failure. */
uint64_t pfa_alloc_pages(uint64_t pages);

/* Like pfa_alloc_pages but constrains the allocation below max_addr (e.g. for DMA
 * that requires addresses below 4 GB). */
uint64_t pfa_alloc_pages_below(uint64_t pages, uint64_t max_addr);

/* Return pages frames starting at base to the free pool. */
void pfa_free_pages(uint64_t base, uint64_t pages);

/* Mark pages frames as permanently reserved (never returned to the free pool).
 * Used to pin the kernel image, ACPI tables, and trampoline pages. */
void pfa_pin_pages(uint64_t base, uint64_t pages);

#endif
