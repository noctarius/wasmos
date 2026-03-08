#ifndef WASMOS_PAGING_H
#define WASMOS_PAGING_H

#include <stdint.h>

#define KERNEL_HIGHER_HALF_BASE 0xFFFFFFFF80000000ULL

int paging_init(void);
uint64_t paging_get_higher_half_base(void);
uint64_t paging_get_root_table(void);
int paging_map_4k(uint64_t virt, uint64_t phys, uint64_t flags);
int paging_unmap_4k(uint64_t virt);

#endif
