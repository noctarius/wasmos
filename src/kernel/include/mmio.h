/* mmio.h - Kernel-mediated 32-bit MMIO register writes.
 *
 * A ring-3 bus driver can already reach a device's I/O-port window, but nothing
 * let it touch a memory-mapped register: wasmos_phys_map COPIES physical bytes
 * into linear memory, which is right for reading ACPI tables and useless for
 * programming hardware. pci-bus needs exactly one memory-mapped write to place
 * an MSI-X table entry, so this is that, and nothing more.
 *
 * The access is deliberately narrow. It is gated by the mmio.map capability AND
 * refuses any address overlapping usable RAM (pfa_range_overlaps_ram), so it is
 * an MMIO poke, not a "write anywhere in physical memory" primitive. */
#ifndef WASMOS_MMIO_H
#define WASMOS_MMIO_H

#include <stdint.h>

/* Write `value` to the 32-bit device register at physical address `phys`, which
 * must be 4-byte aligned and outside system RAM. The page is mapped uncached for
 * the duration of the write. Returns 0, or a negative WASMOS_ERR_MSI_* code
 * (BAD_DEVICE for a bad/RAM address, MAP_FAILED if the page cannot be mapped). */
int mmio_write32_phys(uint64_t phys, uint32_t value);

#endif
