/* device_window.h - kernel mappings of device register windows.
 *
 * A memory-mapped register window a driver is granted (WASMOS_REGION_GRANT_MMIO)
 * is reached through a kernel VA: the io_region_* host calls run in the kernel
 * and touch the register on the driver's behalf, so the driver never holds a
 * mapping of its own. This module owns those kernel mappings. */
#ifndef WASMOS_DEVICE_WINDOW_H
#define WASMOS_DEVICE_WINDOW_H

#include <stdint.h>

/* Distinct device windows the kernel maps over its lifetime, and the largest
 * one it maps. A window is a few registers or a few pages of them; anything
 * larger is a frame buffer or a memory aperture, which this does not serve. */
#define DEVICE_WINDOW_LIMIT 64u
#define DEVICE_WINDOW_MAX_BYTES (2ULL * 1024ULL * 1024ULL)

/* Map [phys, phys + length) uncached (MEM_REGION_FLAG_DEVICE), supervisor-only,
 * into the kernel's device-window span and store the VA of `phys` in *out_kva.
 * phys and length must be page-aligned, length non-zero and at most
 * DEVICE_WINDOW_MAX_BYTES, and the range must not overlap system RAM
 * (pfa_range_overlaps_ram), so a grant cannot turn into a window onto kernel
 * memory.
 *
 * A window is mapped once and stays mapped for the life of the kernel: a range
 * inside one already mapped is answered with the existing mapping. A device's
 * registers do not move once enumerated, so the VA-to-physical translation of a
 * window never changes, which is what lets a driver restart without a TLB
 * shootdown on every CPU that touched its window. Safe to call from any CPU.
 *
 * Returns 0, or -1 for a range that breaks the rules above, a full window
 * table or span, or a failed page-table update (in which case nothing of the
 * new window stays mapped). This is an in-kernel helper; the process manager
 * turns a failure into a refused spawn. */
int device_window_map(uint64_t phys, uint64_t length, uint64_t* out_kva);

#endif
