/* paging.h - 4-level x86_64 page table management: map, unmap, and context switch. */
#ifndef WASMOS_PAGING_H
#define WASMOS_PAGING_H

#include <stdint.h>

#define KERNEL_HIGHER_HALF_BASE  0xFFFFFFFF80000000ULL
#define KERNEL_MMIO_PDPT_INDEX   509u
#define KERNEL_MMIO_FB_VA        0xFFFFFFFF40000000ULL

/* Dedicated per-app WARP linear-memory kernel-VA window, carved from free PDPT
 * slots in the shared higher-half (PML4[511]).  2 GiB (2 PDPT slots) per app,
 * up to 48 apps -> slots [64, 160), all free (only 509=MMIO, 510=direct-map are
 * used).  The higher-half PDPT is a single shared page, so entries installed
 * here appear in every CR3 automatically (the kernel-side linmem alias).
 * Populated on demand by the WARP linmem allocator, so the verifier permits
 * these slots present-or-absent.  Isolation: this alias is mapped SUPERVISOR
 * ONLY (never MEM_REGION_FLAG_USER - enforced in paging_map_4k_in_root's
 * higher-half no-USER check), so ring-3 code cannot reach it; ring-3 wasm uses
 * the separate per-app user mapping at WARP_R3_LINMEM_BASE (PML4 slot 1). */
#define WARP_LINMEM_PDPT_INDEX   64u
#define WARP_LINMEM_PDPT_COUNT   96u
#define WARP_LINMEM_VA_BASE      0xFFFFFF9000000000ULL /* 0xFFFFFF8000000000 + 64*1GiB */
#define WARP_LINMEM_VA_STRIDE    (2ULL * 1024ULL * 1024ULL * 1024ULL) /* 2 GiB / app */

int paging_init(void);
uint64_t paging_get_higher_half_base(void);
uint64_t paging_get_root_table(void);
uint64_t paging_get_current_root_table(void);
int paging_switch_root(uint64_t root_table);
int paging_create_address_space(uint64_t *out_root_table);
void paging_destroy_address_space(uint64_t root_table);
int paging_clone_low_slot_in_root(uint64_t root_table);
int paging_map_4k_in_root(uint64_t root_table, uint64_t virt, uint64_t phys, uint64_t flags);
int paging_unmap_4k_in_root(uint64_t root_table, uint64_t virt);
int paging_strip_low_slot_in_root(uint64_t root_table);
int paging_map_4k(uint64_t virt, uint64_t phys, uint64_t flags);
int paging_unmap_4k(uint64_t virt);

/* Read-only page-table walk: physical address backing `virt` (root_table 0 =
 * current root), including in-page offset, or 0 if unmapped.  Handles large
 * pages.  Used to recover scattered phys from a dedicated-VA linmem pointer. */
uint64_t paging_virt_to_phys_in_root(uint64_t root_table, uint64_t virt);
uint64_t paging_virt_to_phys(uint64_t virt);
int paging_verify_user_root(uint64_t root_table, int log_failures);
int paging_verify_user_root_no_low_slot(uint64_t root_table, int log_failures);
void paging_dump_user_root_kernel_mappings(uint64_t root_table);

#endif
