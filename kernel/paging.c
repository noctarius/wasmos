#include "paging.h"
#include "physmem.h"
#include "serial.h"
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

static uint64_t g_pml4_phys;

static void
serial_write_hex64(uint64_t value)
{
    char buf[21];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write(buf);
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
