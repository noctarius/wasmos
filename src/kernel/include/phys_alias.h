#ifndef WASMOS_KERNEL_PHYS_ALIAS_H
#define WASMOS_KERNEL_PHYS_ALIAS_H

#include <stdint.h>

/*
 * phys_alias.h — whether a physical range can be read through the kernel's
 * higher-half alias.
 *
 * The alias names physical address p at alias_base | p, and only for p below
 * window_bytes (KERNEL_HIGHER_HALF_WINDOW_BYTES); inside the window a page may
 * still be a hole. lookup is paging_virt_to_phys: the physical address behind
 * a virtual one, or 0 when it is unmapped -- a 0 that a mapping of frame 0
 * also returns.
 */

/* Returns 1 when every byte of [phys, phys + size) lies inside the window and
 * every page it touches is mapped, else 0. phys must be nonzero and size at
 * least 1. */
int phys_alias_range_mapped(uint64_t phys, uint64_t size, uint64_t alias_base,
                            uint64_t window_bytes, uint64_t (*lookup)(uint64_t virt));

#endif
