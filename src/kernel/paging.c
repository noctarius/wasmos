#include "paging.h"
#include "physmem.h"
#include "serial.h"
#include "memory.h"
#include <stdint.h>

#define PAGE_SIZE_4K 0x1000ULL
#define PAGE_SIZE_2M 0x200000ULL
#define ENTRIES_PER_TABLE 512

#define PT_FLAG_PRESENT (1ULL << 0)
#define PT_FLAG_WRITE (1ULL << 1)
#define PT_FLAG_USER (1ULL << 2)
#define PT_FLAG_LARGE_PAGE (1ULL << 7)
#define PT_FLAG_NX (1ULL << 63)

#define IDENTITY_PD_COUNT 4
/* Keep only the minimum higher-half span shared into child CR3 roots. */
#define HIGHER_HALF_PD_COUNT 1
/* Limit higher-half sharing to the first 32 MiB window by default. */
#define HIGHER_HALF_PDE_COUNT 16
/* TODO: If higher-half kernel allocations grow beyond the default 32 MiB
 * window, teach child roots to map only the specific additional windows they
 * need. */
#define HIGHER_HALF_PDPT_INDEX 510
#define USER_PML4_INDEX 1

static uint64_t g_pml4_phys;
static uint64_t g_current_pml4_phys;

static uint64_t entry_phys(uint64_t entry);

static uint8_t
is_user_slot_virt(uint64_t virt)
{
    return (uint8_t)(((virt >> 39) & 0x1FFULL) == USER_PML4_INDEX);
}

static int
paging_verify_user_root_impl(uint64_t root_table, int log_failures)
{
    if (!root_table || !g_pml4_phys) {
        return -1;
    }

    volatile uint64_t *root = (volatile uint64_t *)(uintptr_t)root_table;
    volatile uint64_t *kernel = (volatile uint64_t *)(uintptr_t)g_pml4_phys;

    if (root[511] != kernel[511]) {
        if (log_failures) {
            serial_write("[paging] verify fail: higher-half slot mismatch\n");
        }
        return -1;
    }

    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        if (i == 0 || i == 511 || i == USER_PML4_INDEX) {
            continue;
        }
        if (root[i] & PT_FLAG_PRESENT) {
            if (log_failures) {
                serial_printf("[paging] verify fail: unexpected pml4[%u]=%016llx\n",
                              (unsigned int)i,
                              (unsigned long long)root[i]);
            }
            return -1;
        }
    }

    uint64_t pdpt_high_phys = entry_phys(root[511]);
    volatile uint64_t *pdpt_high = (volatile uint64_t *)(uintptr_t)pdpt_high_phys;
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        uint8_t allowed = (i >= HIGHER_HALF_PDPT_INDEX &&
                           i < (HIGHER_HALF_PDPT_INDEX + HIGHER_HALF_PD_COUNT));
        uint8_t present = (uint8_t)((pdpt_high[i] & PT_FLAG_PRESENT) != 0);
        if (present != allowed) {
            if (log_failures) {
                serial_printf("[paging] verify fail: pdpt_high[%u]=%016llx allowed=%u\n",
                              (unsigned int)i,
                              (unsigned long long)pdpt_high[i],
                              (unsigned int)allowed);
            }
            return -1;
        }
        if (!allowed || !present) {
            continue;
        }
        uint64_t pd_phys = entry_phys(pdpt_high[i]);
        volatile uint64_t *pd = (volatile uint64_t *)(uintptr_t)pd_phys;
        for (uint32_t pde = 0; pde < ENTRIES_PER_TABLE; ++pde) {
            uint8_t pde_allowed = (uint8_t)(pde < HIGHER_HALF_PDE_COUNT);
            uint8_t pde_present = (uint8_t)((pd[pde] & PT_FLAG_PRESENT) != 0);
            if (pde_present != pde_allowed) {
                if (log_failures) {
                    serial_printf("[paging] verify fail: pd_high[%u][%u]=%016llx allowed=%u\n",
                                  (unsigned int)i,
                                  (unsigned int)pde,
                                  (unsigned long long)pd[pde],
                                  (unsigned int)pde_allowed);
                }
                return -1;
            }
        }
    }

    if (!(root[0] & PT_FLAG_PRESENT)) {
        if (log_failures) {
            serial_write("[paging] verify fail: pml4[0] absent\n");
        }
        return -1;
    }
    uint64_t pdpt_low_phys = entry_phys(root[0]);
    volatile uint64_t *pdpt_low = (volatile uint64_t *)(uintptr_t)pdpt_low_phys;
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        uint8_t allowed = (uint8_t)(i < IDENTITY_PD_COUNT);
        uint8_t present = (uint8_t)((pdpt_low[i] & PT_FLAG_PRESENT) != 0);
        if (present != allowed) {
            if (log_failures) {
                serial_printf("[paging] verify fail: pdpt_low[%u]=%016llx allowed=%u\n",
                              (unsigned int)i,
                              (unsigned long long)pdpt_low[i],
                              (unsigned int)allowed);
            }
            return -1;
        }
    }

    return 0;
}


static void
zero_page(uint64_t phys_addr)
{
    volatile uint64_t *table = (volatile uint64_t *)(uintptr_t)phys_addr;
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        table[i] = 0;
    }
}

static int
alloc_table(uint64_t *out_phys)
{
    if (!out_phys) {
        return -1;
    }

    uint64_t phys = pfa_alloc_pages(1);
    if (!phys) {
        return -1;
    }

    zero_page(phys);
    *out_phys = phys;
    return 0;
}

static void
write_cr3(uint64_t value)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

static void
invlpg(uint64_t virt)
{
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

static uint64_t
entry_phys(uint64_t entry)
{
    return entry & ~0xFFFULL;
}

static int
ensure_table(uint64_t *entry, uint64_t *out_phys, uint64_t table_flags)
{
    if (*entry & PT_FLAG_PRESENT) {
        if ((*entry & table_flags) != table_flags) {
            *entry |= table_flags;
        }
        *out_phys = entry_phys(*entry);
        return 0;
    }
    uint64_t phys = 0;
    if (alloc_table(&phys) != 0) {
        return -1;
    }
    *entry = phys | PT_FLAG_PRESENT | PT_FLAG_WRITE | table_flags;
    *out_phys = phys;
    return 0;
}

static int
ensure_pt_for_pd(uint64_t *pd_entry, uint64_t table_flags)
{
    if (*pd_entry & PT_FLAG_PRESENT) {
        if ((*pd_entry & PT_FLAG_LARGE_PAGE) == 0) {
            if ((*pd_entry & table_flags) != table_flags) {
                *pd_entry |= table_flags;
            }
            return 0;
        }
        uint64_t base = *pd_entry & ~0x1FFFFFULL;
        uint64_t pt_phys = 0;
        if (alloc_table(&pt_phys) != 0) {
            return -1;
        }
        volatile uint64_t *pt = (volatile uint64_t *)(uintptr_t)pt_phys;
        uint64_t flags = PT_FLAG_PRESENT;
        if (*pd_entry & PT_FLAG_WRITE) {
            flags |= PT_FLAG_WRITE;
        }
        flags |= table_flags;
        for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
            pt[i] = (base + ((uint64_t)i * PAGE_SIZE_4K)) | flags;
        }
        *pd_entry = pt_phys | flags;
        return 0;
    }

    uint64_t pt_phys = 0;
    if (alloc_table(&pt_phys) != 0) {
        return -1;
    }
    *pd_entry = pt_phys | PT_FLAG_PRESENT | PT_FLAG_WRITE | table_flags;
    return 0;
}

int
paging_init(void)
{
    uint64_t pml4_phys = 0;
    uint64_t pdpt_low_phys = 0;
    uint64_t pdpt_high_phys = 0;
    uint64_t pd_phys[IDENTITY_PD_COUNT] = { 0 };
    uint64_t pd_high_phys[HIGHER_HALF_PD_COUNT] = { 0 };

    if (alloc_table(&pml4_phys) != 0 || alloc_table(&pdpt_low_phys) != 0
        || alloc_table(&pdpt_high_phys) != 0) {
        serial_write("[paging] table alloc failed\n");
        return -1;
    }

    for (uint32_t i = 0; i < IDENTITY_PD_COUNT; ++i) {
        if (alloc_table(&pd_phys[i]) != 0) {
            serial_write("[paging] pd alloc failed\n");
            return -1;
        }
    }
    for (uint32_t i = 0; i < HIGHER_HALF_PD_COUNT; ++i) {
        if (alloc_table(&pd_high_phys[i]) != 0) {
            serial_write("[paging] high pd alloc failed\n");
            return -1;
        }
    }

    volatile uint64_t *pml4 = (volatile uint64_t *)(uintptr_t)pml4_phys;
    volatile uint64_t *pdpt_low = (volatile uint64_t *)(uintptr_t)pdpt_low_phys;
    volatile uint64_t *pdpt_high =
        (volatile uint64_t *)(uintptr_t)pdpt_high_phys;

    pml4[0] = pdpt_low_phys | PT_FLAG_PRESENT | PT_FLAG_WRITE;
    pml4[511] = pdpt_high_phys | PT_FLAG_PRESENT | PT_FLAG_WRITE;

    for (uint32_t pdpt_idx = 0; pdpt_idx < IDENTITY_PD_COUNT; ++pdpt_idx) {
        volatile uint64_t *pd = (volatile uint64_t *)(uintptr_t)pd_phys[pdpt_idx];
        pdpt_low[pdpt_idx] = pd_phys[pdpt_idx] | PT_FLAG_PRESENT | PT_FLAG_WRITE;

        uint64_t phys_base = ((uint64_t)pdpt_idx) * (1ULL << 30);
        for (uint32_t pde_idx = 0; pde_idx < ENTRIES_PER_TABLE; ++pde_idx) {
            uint64_t phys = phys_base + ((uint64_t)pde_idx) * PAGE_SIZE_2M;
            pd[pde_idx] =
                phys | PT_FLAG_PRESENT | PT_FLAG_WRITE | PT_FLAG_LARGE_PAGE;
        }
    }
    for (uint32_t high_pdpt_idx = 0; high_pdpt_idx < HIGHER_HALF_PD_COUNT; ++high_pdpt_idx) {
        uint32_t high_idx = HIGHER_HALF_PDPT_INDEX + high_pdpt_idx;
        volatile uint64_t *high_pd = (volatile uint64_t *)(uintptr_t)pd_high_phys[high_pdpt_idx];
        uint64_t phys_base = ((uint64_t)high_pdpt_idx) * (1ULL << 30);
        pdpt_high[high_idx] = pd_high_phys[high_pdpt_idx] | PT_FLAG_PRESENT | PT_FLAG_WRITE;
        for (uint32_t pde_idx = 0; pde_idx < HIGHER_HALF_PDE_COUNT; ++pde_idx) {
            uint64_t phys = phys_base + ((uint64_t)pde_idx) * PAGE_SIZE_2M;
            high_pd[pde_idx] = phys | PT_FLAG_PRESENT | PT_FLAG_WRITE | PT_FLAG_LARGE_PAGE;
        }
    }

    g_pml4_phys = pml4_phys;
    g_current_pml4_phys = g_pml4_phys;
    write_cr3(g_pml4_phys);

    serial_printf("[paging] cr3=%016llx\n[paging] higher-half=%016llx\n",
        (unsigned long long)g_pml4_phys,
        (unsigned long long)KERNEL_HIGHER_HALF_BASE);
    return 0;
}

uint64_t
paging_get_higher_half_base(void)
{
    return KERNEL_HIGHER_HALF_BASE;
}

uint64_t
paging_get_root_table(void)
{
    return g_pml4_phys;
}

uint64_t
paging_get_current_root_table(void)
{
    return g_current_pml4_phys;
}

int
paging_switch_root(uint64_t root_table)
{
    if (!root_table) {
        return -1;
    }
    g_current_pml4_phys = root_table;
    write_cr3(root_table);
    return 0;
}

int
paging_create_address_space(uint64_t *out_root_table)
{
    if (!out_root_table || !g_pml4_phys) {
        return -1;
    }
    uint64_t root = 0;
    uint64_t child_pdpt_low = 0;
    if (alloc_table(&root) != 0) {
        return -1;
    }
    volatile uint64_t *dst = (volatile uint64_t *)(uintptr_t)root;
    volatile uint64_t *src = (volatile uint64_t *)(uintptr_t)g_pml4_phys;
    volatile uint64_t *src_pdpt_low = 0;
    volatile uint64_t *dst_pdpt_low = 0;
    if (!(src[0] & PT_FLAG_PRESENT) || alloc_table(&child_pdpt_low) != 0) {
        pfa_free_pages(root, 1);
        return -1;
    }
    src_pdpt_low = (volatile uint64_t *)(uintptr_t)entry_phys(src[0]);
    dst_pdpt_low = (volatile uint64_t *)(uintptr_t)child_pdpt_low;
    for (uint32_t i = 0; i < IDENTITY_PD_COUNT; ++i) {
        dst_pdpt_low[i] = src_pdpt_low[i];
    }

    /* A child address space starts with only the shared kernel mappings:
     * low identity/direct-physical access in slot 0 and the higher-half alias
     * in slot 511. Slot 1 stays private for process-owned mappings. */
    dst[0] = child_pdpt_low | PT_FLAG_PRESENT | PT_FLAG_WRITE;
    dst[511] = src[511];
    if (paging_verify_user_root_impl(root, 1) != 0) {
        pfa_free_pages(child_pdpt_low, 1);
        pfa_free_pages(root, 1);
        return -1;
    }
    *out_root_table = root;
    return 0;
}

void
paging_destroy_address_space(uint64_t root_table)
{
    if (!root_table || root_table == g_pml4_phys) {
        return;
    }

    volatile uint64_t *pml4 = (volatile uint64_t *)(uintptr_t)root_table;
    volatile uint64_t *kernel = (volatile uint64_t *)(uintptr_t)g_pml4_phys;
    if (pml4[USER_PML4_INDEX] & PT_FLAG_PRESENT) {
        uint64_t pdpt_phys = entry_phys(pml4[USER_PML4_INDEX]);
        volatile uint64_t *pdpt = (volatile uint64_t *)(uintptr_t)pdpt_phys;
        for (uint32_t pdpt_idx = 0; pdpt_idx < ENTRIES_PER_TABLE; ++pdpt_idx) {
            if (!(pdpt[pdpt_idx] & PT_FLAG_PRESENT)) {
                continue;
            }
            uint64_t pd_phys = entry_phys(pdpt[pdpt_idx]);
            volatile uint64_t *pd = (volatile uint64_t *)(uintptr_t)pd_phys;
            for (uint32_t pd_idx = 0; pd_idx < ENTRIES_PER_TABLE; ++pd_idx) {
                if (!(pd[pd_idx] & PT_FLAG_PRESENT)) {
                    continue;
                }
                if ((pd[pd_idx] & PT_FLAG_LARGE_PAGE) == 0) {
                    pfa_free_pages(entry_phys(pd[pd_idx]), 1);
                }
            }
            pfa_free_pages(pd_phys, 1);
        }
        pfa_free_pages(pdpt_phys, 1);
    }
    if ((pml4[0] & PT_FLAG_PRESENT) && (!kernel || entry_phys(pml4[0]) != entry_phys(kernel[0]))) {
        pfa_free_pages(entry_phys(pml4[0]), 1);
    }
    pfa_free_pages(root_table, 1);
}

int
paging_map_4k_in_root(uint64_t root_table, uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (!root_table) {
        return -1;
    }

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx = (virt >> 21) & 0x1FF;
    uint64_t pt_idx = (virt >> 12) & 0x1FF;

    uint8_t user_slot = is_user_slot_virt(virt);
    if (user_slot && !(flags & MEM_REGION_FLAG_USER)) {
        /* Compatibility bridge while callers are migrated: addresses in the
         * dedicated user slot are always user-accessible mappings. */
        flags |= MEM_REGION_FLAG_USER;
    }
    if (!user_slot && (flags & MEM_REGION_FLAG_USER)) {
        return -1;
    }
    if ((flags & MEM_REGION_FLAG_USER) &&
        (flags & MEM_REGION_FLAG_WRITE) &&
        (flags & MEM_REGION_FLAG_EXEC)) {
        /* Enforce W^X policy for user mappings. */
        return -1;
    }

    uint64_t table_flags = 0;
    if (flags & MEM_REGION_FLAG_USER) {
        table_flags |= PT_FLAG_USER;
    }

    volatile uint64_t *pml4 = (volatile uint64_t *)(uintptr_t)root_table;
    uint64_t pdpt_phys = 0;
    if (ensure_table((uint64_t *)&pml4[pml4_idx], &pdpt_phys, table_flags) != 0) {
        return -1;
    }

    volatile uint64_t *pdpt = (volatile uint64_t *)(uintptr_t)pdpt_phys;
    uint64_t pd_phys = 0;
    if (ensure_table((uint64_t *)&pdpt[pdpt_idx], &pd_phys, table_flags) != 0) {
        return -1;
    }

    volatile uint64_t *pd = (volatile uint64_t *)(uintptr_t)pd_phys;
    if (ensure_pt_for_pd((uint64_t *)&pd[pd_idx], table_flags) != 0) {
        return -1;
    }

    uint64_t pt_phys = entry_phys(pd[pd_idx]);
    volatile uint64_t *pt = (volatile uint64_t *)(uintptr_t)pt_phys;

    if (pt[pt_idx] & PT_FLAG_PRESENT) {
        return 1;
    }

    uint64_t map_flags = PT_FLAG_PRESENT;
    if (flags & MEM_REGION_FLAG_WRITE) {
        map_flags |= PT_FLAG_WRITE;
    }
    if (flags & MEM_REGION_FLAG_USER) {
        map_flags |= PT_FLAG_USER;
    }
    if (!(flags & MEM_REGION_FLAG_EXEC)) {
        map_flags |= PT_FLAG_NX;
    }
    pt[pt_idx] = (phys & ~0xFFFULL) | map_flags;
    invlpg(virt);
    return 0;
}

int
paging_unmap_4k_in_root(uint64_t root_table, uint64_t virt)
{
    if (!root_table) {
        return -1;
    }

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx = (virt >> 21) & 0x1FF;
    uint64_t pt_idx = (virt >> 12) & 0x1FF;

    volatile uint64_t *pml4 = (volatile uint64_t *)(uintptr_t)root_table;
    if (!(pml4[pml4_idx] & PT_FLAG_PRESENT)) {
        return -1;
    }
    volatile uint64_t *pdpt = (volatile uint64_t *)(uintptr_t)entry_phys(pml4[pml4_idx]);
    if (!(pdpt[pdpt_idx] & PT_FLAG_PRESENT)) {
        return -1;
    }
    volatile uint64_t *pd = (volatile uint64_t *)(uintptr_t)entry_phys(pdpt[pdpt_idx]);
    if (!(pd[pd_idx] & PT_FLAG_PRESENT)) {
        return -1;
    }
    if (pd[pd_idx] & PT_FLAG_LARGE_PAGE) {
        if (ensure_pt_for_pd((uint64_t *)&pd[pd_idx], 0) != 0) {
            return -1;
        }
    }
    volatile uint64_t *pt = (volatile uint64_t *)(uintptr_t)entry_phys(pd[pd_idx]);
    if (!(pt[pt_idx] & PT_FLAG_PRESENT)) {
        return -1;
    }
    pt[pt_idx] = 0;
    invlpg(virt);
    return 0;
}

int
paging_map_4k(uint64_t virt, uint64_t phys, uint64_t flags)
{
    return paging_map_4k_in_root(g_current_pml4_phys, virt, phys, flags);
}

int
paging_unmap_4k(uint64_t virt)
{
    return paging_unmap_4k_in_root(g_current_pml4_phys, virt);
}

int
paging_verify_user_root(uint64_t root_table, int log_failures)
{
    return paging_verify_user_root_impl(root_table, log_failures ? 1 : 0);
}

void
paging_dump_user_root_kernel_mappings(uint64_t root_table)
{
    if (!root_table) {
        return;
    }
    volatile uint64_t *root = (volatile uint64_t *)(uintptr_t)root_table;
    serial_printf("[paging] dump root=%016llx pml4[0]=%016llx pml4[1]=%016llx pml4[511]=%016llx\n",
                  (unsigned long long)root_table,
                  (unsigned long long)root[0],
                  (unsigned long long)root[1],
                  (unsigned long long)root[511]);
    if (!(root[511] & PT_FLAG_PRESENT)) {
        serial_write("[paging] dump: pml4[511] not present\n");
        return;
    }
    volatile uint64_t *pdpt_high = (volatile uint64_t *)(uintptr_t)entry_phys(root[511]);
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        if (!(pdpt_high[i] & PT_FLAG_PRESENT)) {
            continue;
        }
        serial_printf("[paging] dump: pdpt_high[%u]=%016llx\n",
                      (unsigned int)i,
                      (unsigned long long)pdpt_high[i]);
    }
}
