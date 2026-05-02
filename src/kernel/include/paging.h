#ifndef WASMOS_PAGING_H
#define WASMOS_PAGING_H

#include <stdint.h>

#define KERNEL_HIGHER_HALF_BASE 0xFFFFFFFF80000000ULL

int paging_init(void);
uint64_t paging_get_higher_half_base(void);
uint64_t paging_get_root_table(void);
uint64_t paging_get_current_root_table(void);
int paging_switch_root(uint64_t root_table);
int paging_create_address_space(uint64_t *out_root_table);
void paging_destroy_address_space(uint64_t root_table);
int paging_map_4k_in_root(uint64_t root_table, uint64_t virt, uint64_t phys, uint64_t flags);
int paging_unmap_4k_in_root(uint64_t root_table, uint64_t virt);
int paging_map_4k(uint64_t virt, uint64_t phys, uint64_t flags);
int paging_unmap_4k(uint64_t virt);
int paging_verify_user_root(uint64_t root_table, int log_failures);
void paging_dump_user_root_kernel_mappings(uint64_t root_table);

#endif
