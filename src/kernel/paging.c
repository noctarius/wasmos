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
#define PT_FLAG_LARGE_PAGE (1ULL << 7)

#define IDENTITY_PD_COUNT 4
#define HIGHER_HALF_PD_COUNT 2
#define HIGHER_HALF_PDPT_INDEX 510
#define USER_PML4_INDEX 1

static uint64_t g_pml4_phys;
static uint64_t g_current_pml4_phys;


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
ensure_table(uint64_t *entry, uint64_t *out_phys)
{
    if (*entry & PT_FLAG_PRESENT) {
        *out_phys = entry_phys(*entry);
        return 0;
    }
    uint64_t phys = 0;
    if (alloc_table(&phys) != 0) {
        return -1;
    }
    *entry = phys | PT_FLAG_PRESENT | PT_FLAG_WRITE;
    *out_phys = phys;
    return 0;
}

static int
ensure_pt_for_pd(uint64_t *pd_entry)
{
    if (*pd_entry & PT_FLAG_PRESENT) {
        if ((*pd_entry & PT_FLAG_LARGE_PAGE) == 0) {
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
    *pd_entry = pt_phys | PT_FLAG_PRESENT | PT_FLAG_WRITE;
    return 0;
}

int
paging_init(void)
{
    uint64_t pml4_phys = 0;
    uint64_t pdpt_low_phys = 0;
    uint64_t pdpt_high_phys = 0;
    uint64_t pd_phys[IDENTITY_PD_COUNT] = { 0 };

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

    volatile uint64_t *pml4 = (volatile uint64_t *)(uintptr_t)pml4_phys;
    volatile uint64_t *pdpt_low = (volatile uint64_t *)(uintptr_t)pdpt_low_phys;
    volatile uint64_t *pdpt_high =
        (volatile uint64_t *)(uintptr_t)pdpt_high_phys;

    pml4[0] = pdpt_low_phys | PT_FLAG_PRESENT | PT_FLAG_WRITE;
    pml4[511] = pdpt_high_phys | PT_FLAG_PRESENT | PT_FLAG_WRITE;

    for (uint32_t pdpt_idx = 0; pdpt_idx < IDENTITY_PD_COUNT; ++pdpt_idx) {
        volatile uint64_t *pd = (volatile uint64_t *)(uintptr_t)pd_phys[pdpt_idx];
        pdpt_low[pdpt_idx] = pd_phys[pdpt_idx] | PT_FLAG_PRESENT | PT_FLAG_WRITE;

        if (pdpt_idx < HIGHER_HALF_PD_COUNT) {
            uint32_t high_idx = HIGHER_HALF_PDPT_INDEX + pdpt_idx;
            pdpt_high[high_idx] =
                pd_phys[pdpt_idx] | PT_FLAG_PRESENT | PT_FLAG_WRITE;
        }

        uint64_t phys_base = ((uint64_t)pdpt_idx) * (1ULL << 30);
        for (uint32_t pde_idx = 0; pde_idx < ENTRIES_PER_TABLE; ++pde_idx) {
            uint64_t phys = phys_base + ((uint64_t)pde_idx) * PAGE_SIZE_2M;
            pd[pde_idx] =
                phys | PT_FLAG_PRESENT | PT_FLAG_WRITE | PT_FLAG_LARGE_PAGE;
        }
    }

    g_pml4_phys = pml4_phys;
    g_current_pml4_phys = g_pml4_phys;
    write_cr3(g_pml4_phys);

    serial_write("[paging] cr3=");
    serial_write_hex64(g_pml4_phys);
    serial_write("[paging] higher-half=");
    serial_write_hex64(KERNEL_HIGHER_HALF_BASE);
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
    if (alloc_table(&root) != 0) {
        return -1;
    }
    volatile uint64_t *dst = (volatile uint64_t *)(uintptr_t)root;
    volatile uint64_t *src = (volatile uint64_t *)(uintptr_t)g_pml4_phys;
    /* A child address space starts with only the shared kernel mappings:
     * low identity/direct-physical access in slot 0 and the higher-half alias
     * in slot 511. Slot 1 stays private for process-owned mappings. */
    dst[0] = src[0];
    dst[511] = src[511];
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

    volatile uint64_t *pml4 = (volatile uint64_t *)(uintptr_t)root_table;
    uint64_t pdpt_phys = 0;
    if (ensure_table((uint64_t *)&pml4[pml4_idx], &pdpt_phys) != 0) {
        return -1;
    }

    volatile uint64_t *pdpt = (volatile uint64_t *)(uintptr_t)pdpt_phys;
    uint64_t pd_phys = 0;
    if (ensure_table((uint64_t *)&pdpt[pdpt_idx], &pd_phys) != 0) {
        return -1;
    }

    volatile uint64_t *pd = (volatile uint64_t *)(uintptr_t)pd_phys;
    if (ensure_pt_for_pd((uint64_t *)&pd[pd_idx]) != 0) {
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
        if (ensure_pt_for_pd((uint64_t *)&pd[pd_idx]) != 0) {
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
