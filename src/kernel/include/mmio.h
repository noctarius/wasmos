/* mmio.h - Kernel-mediated 32-bit MMIO register writes.
 *
 * An unprivileged bus driver reaches a device's I/O-port window directly, but
 * has no way to touch a memory-mapped register: wasmos_phys_map COPIES physical
 * bytes into linear memory, which serves reading ACPI tables and does nothing
 * for programming hardware. pci-bus needs exactly one memory-mapped write, to
 * place an MSI-X table entry; this is that, and nothing more.
 *
 * The access is deliberately narrow: it refuses any address overlapping usable
 * RAM (pfa_range_overlaps_ram), so it is an MMIO poke, not a "write anywhere in
 * physical memory" primitive. The mmio.map capability gate lives one level up,
 * in the mmio_write32 host-call handler -- this entry point itself performs no
 * capability check, so kernel callers must not expose it unguarded. */
#ifndef WASMOS_MMIO_H
#define WASMOS_MMIO_H

#include <stdint.h>

/* Write `value` to the 32-bit device register at physical address `phys`, which
 * must be nonzero, 4-byte aligned, and outside system RAM. The containing page
 * is temporarily mapped uncached at a single global scratch VA, so concurrent
 * callers serialize on a spinlock held with interrupts disabled. The write is
 * read back before the mapping is torn down, so it has left the CPU on return.
 * Returns 0, or a negative WASMOS_ERR_MSI_* code (BAD_DEVICE for a misaligned,
 * zero, or RAM-overlapping address; MAP_FAILED if the page cannot be mapped). */
int mmio_write32_phys(uint64_t phys, uint32_t value);

#endif
