/* paging.c - 4-level x86_64 page table management.
 * Provides map/unmap/walk operations for PML4/PDPT/PD/PT page tables.
 * Page tables themselves are read and written through the kernel higher-half
 * alias (table_ptr), and their frames come from physmem.c. */
#include "paging.h"
#include "klog.h"
#include "physmem.h"
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

/* Number of low identity-mapped PDs (1 GiB each) an address space gets.  0 means
 * user roots carry no low identity mapping at all; paging_init still builds one
 * for the kernel root so the first CR3 load can reach the higher-half alias. */
#define IDENTITY_PD_COUNT 0u
/* Upper bound used to size paging_init's scratch array of PD frames. */
#define IDENTITY_PD_COUNT_MAX 4u
/* Keep only the minimum higher-half span shared into child CR3 roots. */
#define HIGHER_HALF_PD_COUNT 1
/* Limit higher-half sharing to the first 512 MiB window by default. */
#define HIGHER_HALF_PDE_COUNT 256
/* Page-table allocations must stay inside the mapped shared higher-half
 * window so zero_page/table_ptr always access mapped memory. */
/* Keep this consistent with HIGHER_HALF_PDE_COUNT (2 MiB per PDE). */
#define KERNEL_SHARED_HIGHER_HALF_WINDOW_BYTES (512u * 1024u * 1024u)
/* TODO: If higher-half kernel allocations grow beyond the default 512 MiB
 * window, teach child roots to map only the specific additional windows they
 * need. */
#define HIGHER_HALF_PDPT_INDEX 510
#define USER_PML4_INDEX 1

static uint64_t g_pml4_phys;
uint64_t g_current_pml4_phys;

static uint64_t entry_phys(uint64_t entry);
static volatile uint64_t* table_ptr(uint64_t phys_addr);

static uint8_t is_user_slot_virt(uint64_t virt) {
    return (uint8_t)(((virt >> 39) & 0x1FFULL) == USER_PML4_INDEX);
}

static int paging_verify_user_root_impl(uint64_t root_table, int log_failures) {
    if (!root_table || !g_pml4_phys) {
        return -1;
    }

    volatile uint64_t* root = table_ptr(root_table);
    volatile uint64_t* kernel = table_ptr(g_pml4_phys);

    if (root[511] != kernel[511]) {
        if (log_failures) {
            klog_write("[paging] verify fail: higher-half slot mismatch\n");
        }
        return -1;
    }

    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        if (i == 0 || i == 511 || i == USER_PML4_INDEX) {
            continue;
        }
        if (root[i] & PT_FLAG_PRESENT) {
            if (log_failures) {
                klog_printf("[paging] verify fail: unexpected pml4[%u]=%016llx\n", (unsigned int)i,
                            (unsigned long long)root[i]);
            }
            return -1;
        }
    }

    uint64_t pdpt_high_phys = entry_phys(root[511]);
    volatile uint64_t* pdpt_high = table_ptr(pdpt_high_phys);
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        /* Dedicated WARP linmem window: populated on demand in this shared
         * higher-half PDPT, so its slots may be present OR absent - skip the
         * strict present==allowed check and the PD descent.  No validation is
         * needed: paging_map_4k_in_root forbids the USER bit on higher-half
         * VAs, so this supervisor-only alias is unreachable from ring-3, and a
         * PD descent would spuriously fail as pages commit incrementally. */
        if (i >= WARP_LINMEM_PDPT_INDEX && i < WARP_LINMEM_PDPT_INDEX + WARP_LINMEM_PDPT_COUNT) {
            continue;
        }
        uint8_t is_kernel_slot =
            (i >= HIGHER_HALF_PDPT_INDEX && i < (HIGHER_HALF_PDPT_INDEX + HIGHER_HALF_PD_COUNT));
        uint8_t is_mmio_slot = (i == KERNEL_MMIO_PDPT_INDEX);
        uint8_t allowed = is_kernel_slot || is_mmio_slot;
        uint8_t present = (uint8_t)((pdpt_high[i] & PT_FLAG_PRESENT) != 0);
        if (present != allowed) {
            if (log_failures) {
                klog_printf("[paging] verify fail: pdpt_high[%u]=%016llx allowed=%u\n",
                            (unsigned int)i, (unsigned long long)pdpt_high[i],
                            (unsigned int)allowed);
            }
            return -1;
        }
        if (!allowed || !present || is_mmio_slot) {
            continue;
        }
        uint64_t pd_phys = entry_phys(pdpt_high[i]);
        volatile uint64_t* pd = table_ptr(pd_phys);
        for (uint32_t pde = 0; pde < ENTRIES_PER_TABLE; ++pde) {
            uint8_t pde_allowed = (uint8_t)(pde < HIGHER_HALF_PDE_COUNT);
            uint8_t pde_present = (uint8_t)((pd[pde] & PT_FLAG_PRESENT) != 0);
            if (pde_present != pde_allowed) {
                if (log_failures) {
                    klog_printf("[paging] verify fail: pd_high[%u][%u]=%016llx allowed=%u\n",
                                (unsigned int)i, (unsigned int)pde, (unsigned long long)pd[pde],
                                (unsigned int)pde_allowed);
                }
                return -1;
            }
        }
    }

    if (root[0] & PT_FLAG_PRESENT) {
        uint64_t pdpt_low_phys = entry_phys(root[0]);
        volatile uint64_t* pdpt_low = table_ptr(pdpt_low_phys);
        for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
            uint8_t allowed = (uint8_t)(i < IDENTITY_PD_COUNT);
            uint8_t present = (uint8_t)((pdpt_low[i] & PT_FLAG_PRESENT) != 0);
            if (present != allowed) {
                if (log_failures) {
                    klog_printf("[paging] verify fail: pdpt_low[%u]=%016llx allowed=%u\n",
                                (unsigned int)i, (unsigned long long)pdpt_low[i],
                                (unsigned int)allowed);
                }
                return -1;
            }
        }
    }

    return 0;
}

static void zero_page(uint64_t phys_addr) {
    volatile uint64_t* table = table_ptr(phys_addr);
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        table[i] = 0;
    }
}

static int alloc_table(uint64_t* out_phys) {
    if (!out_phys) {
        return -1;
    }

    uint64_t phys = pfa_alloc_pages_below(1, KERNEL_SHARED_HIGHER_HALF_WINDOW_BYTES);
    if (!phys) {
        return -1;
    }

    zero_page(phys);
    *out_phys = phys;
    return 0;
}

#define WRITE_CR3(value) __asm__ volatile("mov %0, %%cr3" : : "r"(value) : "memory")

static void invlpg(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

/* invlpg is a per-CPU instruction: this flushes the entry on the executing CPU
 * only.  There is no TLB shootdown IPI, so a mapping changed on one CPU stays
 * stale in another CPU's TLB until that CPU reloads CR3. */
void paging_invalidate(uint64_t virt) {
    invlpg(virt);
}

static uint64_t entry_phys(uint64_t entry) {
    return entry & ~0xFFFULL;
}

/* Address a page table by its physical frame.  Before the first CR3 load the
 * firmware identity map is still active, so the physical address is used
 * directly; afterwards every table is reached through the higher-half alias,
 * which is present in every root.  Valid only for frames inside the shared
 * higher-half window (see alloc_table). */
static volatile uint64_t* table_ptr(uint64_t phys_addr) {
    if (g_current_pml4_phys == 0) {
        return ptr_cast(uint64_t, phys_addr);
    }
    return ptr_cast(uint64_t, (phys_addr | KERNEL_HIGHER_HALF_BASE));
}

static int ensure_table(uint64_t* entry, uint64_t* out_phys, uint64_t table_flags) {
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

/* Ensure that a Page Directory entry points to a valid 4 KiB Page Table.
 *
 * Three cases:
 *   1. Entry is absent (not present): allocate a new PT and install it.
 *   2. Entry already points to a PT (no PT_FLAG_LARGE_PAGE): already fine;
 *      merge any missing table_flags into the existing entry and return.
 *   3. Entry covers a 2 MiB large page (PT_FLAG_LARGE_PAGE set): the large
 *      page must be "exploded" into 512 × 4 KiB PT entries so that a single
 *      4 KiB page within the 2 MiB region can be remapped independently.
 *      The decomposition preserves W and NX bits from the original large-page
 *      entry so the new 4 KiB pages have the same access permissions. */
static int ensure_pt_for_pd(uint64_t* pd_entry, uint64_t table_flags) {
    if (*pd_entry & PT_FLAG_PRESENT) {
        if ((*pd_entry & PT_FLAG_LARGE_PAGE) == 0) {
            /* Case 2: PT already present; propagate any new flags. */
            if ((*pd_entry & table_flags) != table_flags) {
                *pd_entry |= table_flags;
            }
            return 0;
        }
        /* Case 3: explode 2 MiB large page into 512 × 4 KiB entries.
         * base = physical address of the start of the 2 MiB region. */
        uint64_t base = *pd_entry & ~0x1FFFFFULL;
        uint64_t pt_phys = 0;
        if (alloc_table(&pt_phys) != 0) {
            return -1;
        }
        volatile uint64_t* pt = table_ptr(pt_phys);
        uint64_t flags = PT_FLAG_PRESENT;
        if (*pd_entry & PT_FLAG_WRITE) {
            flags |= PT_FLAG_WRITE;
        }
        if (*pd_entry & PT_FLAG_NX) {
            flags |= PT_FLAG_NX;
        }
        flags |= table_flags;
        for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
            pt[i] = (base + ((uint64_t)i * PAGE_SIZE_4K)) | flags;
        }
        *pd_entry = pt_phys | flags; /* replace 2 MiB PD entry with new PT */
        return 0;
    }

    /* Case 1: entry absent — allocate a fresh PT. */
    uint64_t pt_phys = 0;
    if (alloc_table(&pt_phys) != 0) {
        return -1;
    }
    *pd_entry = pt_phys | PT_FLAG_PRESENT | PT_FLAG_WRITE | table_flags;
    return 0;
}

/* Builds the kernel root from scratch and installs it in CR3, publishing it as
 * both g_pml4_phys and g_current_pml4_phys.  Runs once on the BSP while the
 * firmware identity map is still the active mapping, which is what lets
 * table_ptr address the freshly allocated frames by physical address.
 *
 * The higher-half alias (PML4 slot 511, PDPT slots 510..) is built from 2 MiB
 * large pages covering the first HIGHER_HALF_PDE_COUNT * 2 MiB of physical RAM;
 * that window is what every later table_ptr/alias access depends on, which is
 * why alloc_table refuses frames above it.
 *
 * With IDENTITY_PD_COUNT == 0 a low identity PD is still built, because the
 * instruction stream is executing from a low VA at the moment CR3 is loaded.
 * The `mov cr3` and the `jmp` to the higher-half address of the label that
 * follows are emitted as one asm block so no compiler-generated low-VA
 * instruction can sit between them.  The kernel root then keeps that low slot
 * (see the TODO at the label); user roots do not.
 *
 * Returns 0 on success, -1 if any table frame could not be allocated.  Frames
 * already taken on a failure path are not returned to the allocator; a failure
 * here is fatal to boot anyway. */
int paging_init(void) {
    uint64_t pml4_phys = 0;
    uint64_t pdpt_low_phys = 0;
    uint64_t pdpt_high_phys = 0;
    uint64_t pd_phys[IDENTITY_PD_COUNT_MAX] = {0};
    uint64_t pd_high_phys[HIGHER_HALF_PD_COUNT] = {0};
    uint8_t bootstrap_low_slot = 0;
    uint32_t identity_pd_count = IDENTITY_PD_COUNT;

    if (alloc_table(&pml4_phys) != 0 || alloc_table(&pdpt_low_phys) != 0 ||
        alloc_table(&pdpt_high_phys) != 0) {
        klog_write("[paging] table alloc failed\n");
        return -1;
    }

    if (identity_pd_count == 0) {
        /* When dropping low identity mappings entirely, keep a temporary single
         * low PD alive only long enough to switch CR3 and jump execution into
         * the higher-half alias. */
        identity_pd_count = 1;
        bootstrap_low_slot = 1;
    }

    for (uint32_t i = 0; i < identity_pd_count; ++i) {
        if (alloc_table(&pd_phys[i]) != 0) {
            klog_write("[paging] pd alloc failed\n");
            return -1;
        }
    }
    for (uint32_t i = 0; i < HIGHER_HALF_PD_COUNT; ++i) {
        if (alloc_table(&pd_high_phys[i]) != 0) {
            klog_write("[paging] high pd alloc failed\n");
            return -1;
        }
    }

    volatile uint64_t* pml4 = table_ptr(pml4_phys);
    volatile uint64_t* pdpt_low = table_ptr(pdpt_low_phys);
    volatile uint64_t* pdpt_high = table_ptr(pdpt_high_phys);

    if (identity_pd_count > 0) {
        pml4[0] = pdpt_low_phys | PT_FLAG_PRESENT | PT_FLAG_WRITE;
    } else {
        pml4[0] = 0;
    }
    pml4[511] = pdpt_high_phys | PT_FLAG_PRESENT | PT_FLAG_WRITE;

    for (uint32_t pdpt_idx = 0; pdpt_idx < identity_pd_count; ++pdpt_idx) {
        volatile uint64_t* pd = table_ptr(pd_phys[pdpt_idx]);
        pdpt_low[pdpt_idx] = pd_phys[pdpt_idx] | PT_FLAG_PRESENT | PT_FLAG_WRITE;

        uint64_t phys_base = ((uint64_t)pdpt_idx) * (1ULL << 30);
        for (uint32_t pde_idx = 0; pde_idx < ENTRIES_PER_TABLE; ++pde_idx) {
            uint64_t phys = phys_base + ((uint64_t)pde_idx) * PAGE_SIZE_2M;
            pd[pde_idx] = phys | PT_FLAG_PRESENT | PT_FLAG_WRITE | PT_FLAG_LARGE_PAGE;
        }
    }
    for (uint32_t high_pdpt_idx = 0; high_pdpt_idx < HIGHER_HALF_PD_COUNT; ++high_pdpt_idx) {
        uint32_t high_idx = HIGHER_HALF_PDPT_INDEX + high_pdpt_idx;
        volatile uint64_t* high_pd = table_ptr(pd_high_phys[high_pdpt_idx]);
        uint64_t phys_base = ((uint64_t)high_pdpt_idx) * (1ULL << 30);
        pdpt_high[high_idx] = pd_high_phys[high_pdpt_idx] | PT_FLAG_PRESENT | PT_FLAG_WRITE;
        for (uint32_t pde_idx = 0; pde_idx < HIGHER_HALF_PDE_COUNT; ++pde_idx) {
            uint64_t phys = phys_base + ((uint64_t)pde_idx) * PAGE_SIZE_2M;
            high_pd[pde_idx] = phys | PT_FLAG_PRESENT | PT_FLAG_WRITE | PT_FLAG_LARGE_PAGE;
        }
    }

    g_pml4_phys = pml4_phys;
    g_current_pml4_phys = g_pml4_phys;

    if (bootstrap_low_slot) {
        uint64_t high_target = addr_cast(uint64_t, &&paging_init_after_bootstrap);
        if (high_target < KERNEL_HIGHER_HALF_BASE) {
            high_target += KERNEL_HIGHER_HALF_BASE;
        }
        __asm__ volatile("mov %0, %%cr3\n"
                         "jmp *%1\n"
                         :
                         : "r"(g_pml4_phys), "r"(high_target)
                         : "memory");
    } else {
        WRITE_CR3(g_pml4_phys);
    }

paging_init_after_bootstrap:
    if (bootstrap_low_slot) {
        /* TODO: The kernel root keeps this low identity slot for good, because
         * early bring-up (boot_shadow_alloc_low and friends) still writes
         * through low physical addresses after the CR3 handoff.  Until those
         * callers use the higher-half alias, the kernel root is weaker than the
         * user roots, which already honour IDENTITY_PD_COUNT = 0. */
    }

    klog_printf("[paging] cr3=%016llx\n[paging] higher-half=%016llx\n",
                (unsigned long long)g_pml4_phys, (unsigned long long)KERNEL_HIGHER_HALF_BASE);
    return 0;
}

uint64_t paging_get_higher_half_base(void) {
    return KERNEL_HIGHER_HALF_BASE;
}

/* Physical frame of the kernel root, or 0 before paging_init has run. */
uint64_t paging_get_root_table(void) {
    return g_pml4_phys;
}

/* Reads the executing CPU's live CR3.  Under SMP this is the only authoritative
 * source of "which root am I on": g_current_pml4_phys is a single global updated
 * by whichever CPU switched last. */
uint64_t paging_get_current_root_table(void) {
    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/* Load root_table into CR3 and mirror it in g_current_pml4_phys.  Returns 0, or
 * -1 for a zero root_table.  Naked so the compiler cannot wrap the CR3 write in
 * a frame or spill around it.  The mirror is a single global, last-writer-wins,
 * so under SMP it is not necessarily this CPU's root — read CR3 directly
 * (paging_get_current_root_table) wherever that distinction matters. */
__attribute__((naked)) int paging_switch_root(uint64_t root_table) {
    __asm__ volatile("test %rdi, %rdi\n"
                     "jz 1f\n"
                     "mov %rdi, g_current_pml4_phys(%rip)\n"
                     "mov %rdi, %cr3\n"
                     "xor %eax, %eax\n"
                     "ret\n"
                     "1:\n"
                     "mov $-1, %eax\n"
                     "ret\n");
}

/* Allocates a fresh root frame and seeds it with the shared kernel mappings.
 *
 * Slot 511 is copied by VALUE from the kernel root, so the child and the kernel
 * share the same higher-half PDPT frame — the child does not own it and
 * paging_destroy_address_space must not free it.  Slot 0 is either empty
 * (IDENTITY_PD_COUNT == 0) or a privately allocated PDPT whose first
 * IDENTITY_PD_COUNT entries are copied from the kernel's.  Slot 1 (the user
 * slot) is left empty for paging_map_4k_in_root to populate on demand.
 *
 * *out_root_table receives a PHYSICAL frame address on success.  Returns 0 on
 * success, -1 on a NULL out pointer, before paging_init, on frame exhaustion, or
 * when the assembled root fails paging_verify_user_root_impl (in which case the
 * frames taken here are released again). */
int paging_create_address_space(uint64_t* out_root_table) {
    if (!out_root_table || !g_pml4_phys) {
        return -1;
    }
    uint64_t root = 0;
    uint64_t child_pdpt_low = 0;
    if (alloc_table(&root) != 0) {
        return -1;
    }
    volatile uint64_t* dst = table_ptr(root);
    volatile uint64_t* src = table_ptr(g_pml4_phys);
    volatile uint64_t* src_pdpt_low = 0;
    volatile uint64_t* dst_pdpt_low = 0;
    if (IDENTITY_PD_COUNT > 0) {
        if (!(src[0] & PT_FLAG_PRESENT) || alloc_table(&child_pdpt_low) != 0) {
            pfa_free_pages(root, 1);
            return -1;
        }
        src_pdpt_low = table_ptr(entry_phys(src[0]));
        dst_pdpt_low = table_ptr(child_pdpt_low);
        for (uint32_t i = 0; i < IDENTITY_PD_COUNT; ++i) {
            dst_pdpt_low[i] = src_pdpt_low[i];
        }
    }

    /* A child address space starts with only the shared kernel mappings: the
     * higher-half alias in slot 511, and a private copy of the low
     * identity/direct-physical PDPT in slot 0 only when IDENTITY_PD_COUNT > 0.
     * At the current baseline of 0, slot 0 is empty.  Slot 1 stays private for
     * process-owned mappings. */
    dst[0] = (IDENTITY_PD_COUNT > 0) ? (child_pdpt_low | PT_FLAG_PRESENT | PT_FLAG_WRITE) : 0;
    dst[511] = src[511];
    if (paging_verify_user_root_impl(root, 1) != 0) {
        pfa_free_pages(child_pdpt_low, 1);
        pfa_free_pages(root, 1);
        return -1;
    }
    *out_root_table = root;
    return 0;
}

/* Frees the page-table STRUCTURE owned by a user root: every PT and PD below
 * the user slot (PML4 index 1), that slot's PDPT, a private low PDPT in slot 0
 * when it is not the kernel's own, and finally the root frame itself.
 *
 * Leaf frames stay untouched — the mapped data pages belong to the memory-region
 * and shared-memory owners, which release them separately, and 2 MiB leaf PDEs
 * are skipped for the same reason.  Slot 511 is deliberately not walked: it is
 * an alias of the kernel's higher-half PDPT.
 *
 * A zero root, and the kernel root itself, are ignored.  Each frame freed here
 * is freed exactly once, so calling this twice for the same root trips the
 * allocator's double-free panic.
 *
 * Only the slot-0 PDPT frame is reclaimed; PD and PT frames installed below it
 * by paging_clone_low_slot_in_root are not walked. */
void paging_destroy_address_space(uint64_t root_table) {
    if (!root_table || root_table == g_pml4_phys) {
        return;
    }

    volatile uint64_t* pml4 = table_ptr(root_table);
    volatile uint64_t* kernel = table_ptr(g_pml4_phys);
    if (pml4[USER_PML4_INDEX] & PT_FLAG_PRESENT) {
        uint64_t pdpt_phys = entry_phys(pml4[USER_PML4_INDEX]);
        volatile uint64_t* pdpt = table_ptr(pdpt_phys);
        for (uint32_t pdpt_idx = 0; pdpt_idx < ENTRIES_PER_TABLE; ++pdpt_idx) {
            if (!(pdpt[pdpt_idx] & PT_FLAG_PRESENT)) {
                continue;
            }
            uint64_t pd_phys = entry_phys(pdpt[pdpt_idx]);
            volatile uint64_t* pd = table_ptr(pd_phys);
            for (uint32_t pd_idx = 0; pd_idx < ENTRIES_PER_TABLE; ++pd_idx) {
                if (!(pd[pd_idx] & PT_FLAG_PRESENT)) {
                    continue;
                }
                if (pd[pd_idx] & PT_FLAG_LARGE_PAGE) {
                    /* Do not free mapped leaf frames here. Memory-region/shared
                     * ownership tears those down, while paging teardown owns only
                     * the page-table structures. */
                    continue;
                } else {
                    uint64_t pt_phys = entry_phys(pd[pd_idx]);
                    volatile uint64_t* pt = table_ptr(pt_phys);
                    (void)pt;
                    pfa_free_pages(pt_phys, 1);
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

/* Gives root_table a PRIVATE deep copy of the kernel root's low slot (PML4
 * index 0): a fresh PDPT, a fresh PD per present PDPT entry, and a fresh PT per
 * present 4 KiB PDE.  2 MiB PDEs are copied verbatim, so the leaf frames they
 * describe stay shared with the kernel root; only the table frames are new.
 *
 * The copy is a snapshot — later changes to the kernel's low slot do not
 * propagate — which is what lets paging_strip_low_slot_in_root drop this slot
 * from one root without disturbing the kernel's.
 *
 * dst[0] is overwritten, so any subtree previously installed there is dropped
 * without being freed.  Returns 0 on success, -1 when either root is missing,
 * when the kernel root has no low slot, or on frame exhaustion; the partial
 * copy's PD/PT frames are not all reclaimed on the error paths. */
int paging_clone_low_slot_in_root(uint64_t root_table) {
    if (!root_table || !g_pml4_phys) {
        return -1;
    }
    volatile uint64_t* dst = table_ptr(root_table);
    volatile uint64_t* src = table_ptr(g_pml4_phys);
    if (!(src[0] & PT_FLAG_PRESENT)) {
        return -1;
    }
    uint64_t src_pdpt_phys = entry_phys(src[0]);
    volatile uint64_t* src_pdpt = table_ptr(src_pdpt_phys);
    uint64_t new_pdpt_phys = 0;
    if (alloc_table(&new_pdpt_phys) != 0) {
        return -1;
    }
    volatile uint64_t* new_pdpt = table_ptr(new_pdpt_phys);

    for (uint32_t pdpt_i = 0; pdpt_i < ENTRIES_PER_TABLE; ++pdpt_i) {
        uint64_t pdpt_entry = src_pdpt[pdpt_i];
        if (!(pdpt_entry & PT_FLAG_PRESENT)) {
            continue;
        }

        uint64_t src_pd_phys = entry_phys(pdpt_entry);
        uint64_t new_pd_phys = 0;
        if (alloc_table(&new_pd_phys) != 0) {
            pfa_free_pages(new_pdpt_phys, 1);
            return -1;
        }
        volatile uint64_t* src_pd = table_ptr(src_pd_phys);
        volatile uint64_t* new_pd = table_ptr(new_pd_phys);

        for (uint32_t pd_i = 0; pd_i < ENTRIES_PER_TABLE; ++pd_i) {
            uint64_t pd_entry = src_pd[pd_i];
            if (!(pd_entry & PT_FLAG_PRESENT)) {
                continue;
            }
            if (pd_entry & PT_FLAG_LARGE_PAGE) {
                new_pd[pd_i] = pd_entry;
                continue;
            }

            uint64_t src_pt_phys = entry_phys(pd_entry);
            uint64_t new_pt_phys = 0;
            if (alloc_table(&new_pt_phys) != 0) {
                pfa_free_pages(new_pd_phys, 1);
                pfa_free_pages(new_pdpt_phys, 1);
                return -1;
            }
            volatile uint64_t* src_pt = table_ptr(src_pt_phys);
            volatile uint64_t* new_pt = table_ptr(new_pt_phys);
            for (uint32_t pt_i = 0; pt_i < ENTRIES_PER_TABLE; ++pt_i) {
                new_pt[pt_i] = src_pt[pt_i];
            }
            new_pd[pd_i] = (new_pt_phys & ~0xFFFULL) | (pd_entry & 0xFFFULL);
        }

        new_pdpt[pdpt_i] = (new_pd_phys & ~0xFFFULL) | (pdpt_entry & 0xFFFULL);
    }

    dst[0] = (new_pdpt_phys & ~0xFFFULL) | (src[0] & 0xFFFULL);
    return 0;
}

/* Installs a 4 KiB mapping virt -> phys in root_table.
 *
 * root_table and phys are PHYSICAL frame addresses; virt is a virtual address in
 * root_table's address space, not necessarily the running one — the walk goes
 * through the higher-half alias, so mapping into a foreign root does not require
 * switching CR3.  Both are truncated to 4 KiB granularity.
 *
 * flags are MEM_REGION_FLAG_* bits, not raw PTE bits.  The user bit and the VA
 * must agree: PML4 slot 1 is the user slot and requires MEM_REGION_FLAG_USER,
 * every other slot forbids it.  W^X is enforced for user mappings (WRITE and
 * EXEC together are refused), and NX is set on every mapping without
 * MEM_REGION_FLAG_EXEC.  MEM_REGION_FLAG_USER also propagates PT_FLAG_USER into
 * the intermediate PML4E/PDPTE/PDE, which is required by the hardware.
 *
 * Missing intermediate tables are allocated; a 2 MiB PDE covering virt is
 * exploded into a PT first, preserving its W/NX bits.  An already-present leaf
 * PTE is replaced in place (used for shared-memory overlays over wasm linear
 * pages), so this is a map-or-remap, and the old physical frame is not freed.
 *
 * The TLB entry is invalidated on the calling CPU only.
 *
 * Returns 0 on success, -1 on a zero root, a flags/VA user-bit mismatch, a W^X
 * violation, or table-frame exhaustion. */
int paging_map_4k_in_root(uint64_t root_table, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!root_table) {
        return -1;
    }

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx = (virt >> 21) & 0x1FF;
    uint64_t pt_idx = (virt >> 12) & 0x1FF;

    uint8_t user_slot = is_user_slot_virt(virt);
    if (user_slot && !(flags & MEM_REGION_FLAG_USER)) {
        return -1;
    }
    if (!user_slot && (flags & MEM_REGION_FLAG_USER)) {
        return -1;
    }
    if ((flags & MEM_REGION_FLAG_USER) && (flags & MEM_REGION_FLAG_WRITE) &&
        (flags & MEM_REGION_FLAG_EXEC)) {
        /* Enforce W^X policy for user mappings. */
        return -1;
    }

    uint64_t table_flags = 0;
    if (flags & MEM_REGION_FLAG_USER) {
        table_flags |= PT_FLAG_USER;
    }

    volatile uint64_t* pml4 = table_ptr(root_table);
    uint64_t pdpt_phys = 0;
    if (ensure_table((uint64_t*)&pml4[pml4_idx], &pdpt_phys, table_flags) != 0) {
        return -1;
    }

    volatile uint64_t* pdpt = table_ptr(pdpt_phys);
    uint64_t pd_phys = 0;
    if (ensure_table((uint64_t*)&pdpt[pdpt_idx], &pd_phys, table_flags) != 0) {
        return -1;
    }

    volatile uint64_t* pd = table_ptr(pd_phys);
    if (ensure_pt_for_pd((uint64_t*)&pd[pd_idx], table_flags) != 0) {
        return -1;
    }

    uint64_t pt_phys = entry_phys(pd[pd_idx]);
    volatile uint64_t* pt = table_ptr(pt_phys);

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
    if (pt[pt_idx] & PT_FLAG_PRESENT) {
        /* Allow explicit remap updates (e.g. shared-memory overlays on wasm
         * linear pages) by replacing present 4K PTEs in place. */
        pt[pt_idx] = (phys & ~0xFFFULL) | map_flags;
        invlpg(virt);
        return 0;
    }
    pt[pt_idx] = (phys & ~0xFFFULL) | map_flags;
    invlpg(virt);
    return 0;
}

/* Resolve the physical address backing `virt` in `root_table` (0 = current
 * root) via a read-only page-table walk.  Returns the full physical address
 * including the in-page offset, or 0 if any level is not present.  Handles
 * 1 GiB / 2 MiB large pages.  Masks the phys field explicitly (bits 12..51) so
 * a set NX/high bit on a data PTE cannot leak into the result.  Used by the
 * WARP linmem chokepoint to recover scattered physical pages from a dedicated-
 * VA pointer without maintaining a per-page phys list. */
uint64_t paging_virt_to_phys_in_root(uint64_t root_table, uint64_t virt) {
    if (root_table == 0) {
        /* Under SMP the current root must come from this CPU's live CR3, not
         * the last writer to the shared g_current_pml4_phys mirror.  WARP's
         * dedicated linmem VA window resolves backing phys pages through this
         * helper during ring-3 setup, so using the mirror can walk another
         * CPU's address space and make ring-3 dual-map setup fail. */
        root_table = paging_get_current_root_table();
    }
    if (root_table == 0) {
        return 0;
    }
    const uint64_t PHYS_MASK = 0x000FFFFFFFFFF000ULL; /* bits 12..51 */
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx = (virt >> 21) & 0x1FF;
    uint64_t pt_idx = (virt >> 12) & 0x1FF;

    volatile uint64_t* pml4 = table_ptr(root_table);
    if (!(pml4[pml4_idx] & PT_FLAG_PRESENT)) {
        return 0;
    }
    volatile uint64_t* pdpt = table_ptr(pml4[pml4_idx] & PHYS_MASK);
    uint64_t pdpte = pdpt[pdpt_idx];
    if (!(pdpte & PT_FLAG_PRESENT)) {
        return 0;
    }
    if (pdpte & PT_FLAG_LARGE_PAGE) { /* 1 GiB page */
        return (pdpte & 0x000FFFFFC0000000ULL) | (virt & 0x3FFFFFFFULL);
    }
    volatile uint64_t* pd = table_ptr(pdpte & PHYS_MASK);
    uint64_t pde = pd[pd_idx];
    if (!(pde & PT_FLAG_PRESENT)) {
        return 0;
    }
    if (pde & PT_FLAG_LARGE_PAGE) { /* 2 MiB page */
        return (pde & 0x000FFFFFFFE00000ULL) | (virt & 0x1FFFFFULL);
    }
    volatile uint64_t* pt = table_ptr(pde & PHYS_MASK);
    uint64_t pte = pt[pt_idx];
    if (!(pte & PT_FLAG_PRESENT)) {
        return 0;
    }
    return (pte & PHYS_MASK) | (virt & 0xFFFULL);
}

/* paging_virt_to_phys_in_root against the executing CPU's live CR3. */
uint64_t paging_virt_to_phys(uint64_t virt) {
    return paging_virt_to_phys_in_root(0, virt);
}

/* Removes the low identity slot (PML4 index 0) from a user root and frees that
 * slot's PDPT frame, but only when it is private — a root still pointing at the
 * kernel's own low PDPT just has the entry cleared.  PD and PT frames below the
 * PDPT are not walked, so a slot installed by paging_clone_low_slot_in_root
 * leaves those frames allocated.
 *
 * If root_table happens to be the active root, CR3 is rewritten to flush the
 * stale TLB entries on this CPU before returning.
 *
 * Returns 0 when the root is absent-low-slot AND passes the shared-kernel-layout
 * verification, -1 otherwise, including for a zero root or the kernel root
 * (which must keep its low slot). */
int paging_strip_low_slot_in_root(uint64_t root_table) {
    if (!root_table || root_table == g_pml4_phys) {
        return -1;
    }
    volatile uint64_t* pml4 = table_ptr(root_table);
    if (!(pml4[0] & PT_FLAG_PRESENT)) {
        return paging_verify_user_root_impl(root_table, 0);
    }
    uint64_t pdpt_low_phys = entry_phys(pml4[0]);
    uint64_t kernel_pdpt_low_phys = 0;
    if (g_pml4_phys) {
        volatile uint64_t* kernel_pml4 = table_ptr(g_pml4_phys);
        if (kernel_pml4[0] & PT_FLAG_PRESENT) {
            kernel_pdpt_low_phys = entry_phys(kernel_pml4[0]);
        }
    }
    pml4[0] = 0;
    if (paging_get_current_root_table() == root_table) {
        WRITE_CR3(root_table);
    }
    if (pdpt_low_phys && pdpt_low_phys != kernel_pdpt_low_phys) {
        pfa_free_pages(pdpt_low_phys, 1);
    }
    return paging_verify_user_root_impl(root_table, 0);
}

/* Clears the leaf PTE for virt in root_table and invalidates it on the calling
 * CPU.  The physical frame is NOT freed and the now-possibly-empty PT/PD/PDPT
 * are not reclaimed; frame ownership lives with the caller that mapped it.
 *
 * A 2 MiB PDE covering virt is exploded into a PT first — which ALLOCATES a
 * table frame — so that a single 4 KiB hole can be punched into it.
 *
 * Returns 0 on success, -1 for a zero root or when any level along the walk,
 * including the leaf, is not present. */
int paging_unmap_4k_in_root(uint64_t root_table, uint64_t virt) {
    if (!root_table) {
        return -1;
    }

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx = (virt >> 21) & 0x1FF;
    uint64_t pt_idx = (virt >> 12) & 0x1FF;

    volatile uint64_t* pml4 = table_ptr(root_table);
    if (!(pml4[pml4_idx] & PT_FLAG_PRESENT)) {
        return -1;
    }
    volatile uint64_t* pdpt = table_ptr(entry_phys(pml4[pml4_idx]));
    if (!(pdpt[pdpt_idx] & PT_FLAG_PRESENT)) {
        return -1;
    }
    volatile uint64_t* pd = table_ptr(entry_phys(pdpt[pdpt_idx]));
    if (!(pd[pd_idx] & PT_FLAG_PRESENT)) {
        return -1;
    }
    if (pd[pd_idx] & PT_FLAG_LARGE_PAGE) {
        if (ensure_pt_for_pd((uint64_t*)&pd[pd_idx], 0) != 0) {
            return -1;
        }
    }
    volatile uint64_t* pt = table_ptr(entry_phys(pd[pd_idx]));
    if (!(pt[pt_idx] & PT_FLAG_PRESENT)) {
        return -1;
    }
    pt[pt_idx] = 0;
    invlpg(virt);
    return 0;
}

/* paging_map_4k_in_root against the executing CPU's live CR3, so on a user CR3
 * this maps into that process's address space, not the kernel root's. */
int paging_map_4k(uint64_t virt, uint64_t phys, uint64_t flags) {
    return paging_map_4k_in_root(paging_get_current_root_table(), virt, phys, flags);
}

/* paging_unmap_4k_in_root against the executing CPU's live CR3. */
int paging_unmap_4k(uint64_t virt) {
    return paging_unmap_4k_in_root(paging_get_current_root_table(), virt);
}

/* Checks that root_table still carries exactly the shared-kernel layout every
 * user root is required to have: PML4[511] bit-identical to the kernel root's,
 * no populated PML4 slot other than 0, 1 and 511, the higher-half PDPT populated
 * only in the kernel and MMIO slots (the WARP linmem window is exempt because it
 * commits on demand), each shared higher-half PD present exactly over its first
 * HIGHER_HALF_PDE_COUNT entries, and — when slot 0 exists — its PDPT populated
 * exactly over the first IDENTITY_PD_COUNT entries.
 *
 * Returns 0 when the layout matches and -1 on the first mismatch; a non-zero
 * log_failures emits the offending index and entry through klog. */
int paging_verify_user_root(uint64_t root_table, int log_failures) {
    return paging_verify_user_root_impl(root_table, log_failures ? 1 : 0);
}

/* paging_verify_user_root plus the post-strip requirement that PML4[0] is
 * absent.  Returns 0 only when both hold. */
int paging_verify_user_root_no_low_slot(uint64_t root_table, int log_failures) {
    if (paging_verify_user_root_impl(root_table, log_failures ? 1 : 0) != 0) {
        return -1;
    }
    if (!root_table) {
        return -1;
    }
    volatile uint64_t* root = table_ptr(root_table);
    if (root[0] & PT_FLAG_PRESENT) {
        if (log_failures) {
            klog_printf("[paging] verify fail: low slot still present pml4[0]=%016llx\n",
                        (unsigned long long)root[0]);
        }
        return -1;
    }
    return 0;
}

/* Diagnostic dump of the kernel-visible part of a root: the three interesting
 * PML4 entries and every present higher-half PDPT entry.  Writes to klog only
 * and changes no state; a zero root is ignored. */
void paging_dump_user_root_kernel_mappings(uint64_t root_table) {
    if (!root_table) {
        return;
    }
    volatile uint64_t* root = table_ptr(root_table);
    klog_printf("[paging] dump root=%016llx pml4[0]=%016llx pml4[1]=%016llx pml4[511]=%016llx\n",
                (unsigned long long)root_table, (unsigned long long)root[0],
                (unsigned long long)root[1], (unsigned long long)root[511]);
    if (!(root[511] & PT_FLAG_PRESENT)) {
        klog_write("[paging] dump: pml4[511] not present\n");
        return;
    }
    volatile uint64_t* pdpt_high = table_ptr(entry_phys(root[511]));
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        if (!(pdpt_high[i] & PT_FLAG_PRESENT)) {
            continue;
        }
        klog_printf("[paging] dump: pdpt_high[%u]=%016llx\n", (unsigned int)i,
                    (unsigned long long)pdpt_high[i]);
    }
}
