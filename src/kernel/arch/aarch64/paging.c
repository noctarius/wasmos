/* paging.c - aarch64 stage-1 translation tables behind paging.h.
 *
 * 48-bit VAs, 4 KiB granule, four levels in both halves (TCR_EL1 from
 * entry.S), so the level indices are the x86_64 PML4/PDPT/PD/PT indices and
 * paging.h's slot language applies unchanged:
 *   - a root table is a TTBR0 level-0 table: slot 0 is the low identity slot
 *     (kernel root only, cloned or stripped per address space), slot 1 the
 *     user slot, every other slot empty;
 *   - the kernel half is one TTBR1 level-0 table shared by every address
 *     space and never switched. Its slot 511 holds the direct map of physical
 *     [0, KERNEL_HIGHER_HALF_WINDOW_BYTES) as level-1 blocks, the framebuffer
 *     and device-window spans, and the linear-memory window. A walk of an
 *     upper-half VA uses it whatever root it is given.
 *
 * Tables are reached through the direct map (table_ptr) and their frames come
 * from below the direct-map window. Descriptor fields follow ARM ARM D8.3:
 * MAIR index 0 is Device-nGnRE, 1 Normal write-back (entry.S programs MAIR).
 *
 * TLB maintenance uses the inner-shareable broadcast forms, so an invalidation
 * reaches every CPU, which is stronger than paging.h requires. No ASIDs are
 * allocated yet: paging_switch_root invalidates the whole TLB.
 * TODO: per-root ASIDs, so a root switch keeps other roots' entries. */
#include "paging.h"
#include "klog.h"
#include "memory.h"
#include "physmem.h"
#include <stdint.h>

#define ENTRIES_PER_TABLE 512u
#define PAGE_SIZE_4K 0x1000ULL
#define LEVEL1_BLOCK (1ULL << 30)
#define LEVEL2_BLOCK (1ULL << 21)

#define DESC_VALID (1ULL << 0)
/* Bit 1: table (levels 0-2) or page (level 3); clear means a block. */
#define DESC_TABLE (1ULL << 1)
#define DESC_PAGE (1ULL << 1)
#define DESC_ATTR_DEVICE (0ULL << 2)
#define DESC_ATTR_NORMAL (1ULL << 2)
#define DESC_ATTR_MASK (7ULL << 2)
#define DESC_AP_EL0 (1ULL << 6)    /* AP[1]: EL0 may access */
#define DESC_AP_RDONLY (1ULL << 7) /* AP[2]: read-only at every level */
#define DESC_SH_INNER (3ULL << 8)
#define DESC_AF (1ULL << 10)
#define DESC_NG (1ULL << 11)
#define DESC_PXN (1ULL << 53)
#define DESC_UXN (1ULL << 54)
#define DESC_OA_MASK 0x0000FFFFFFFFF000ULL
/* Lower (bits 11:2) and upper (bits 63:50) attributes of a block or page. */
#define DESC_LEAF_ATTRS 0xFFFC000000000FFCULL

#define USER_L0_INDEX 1u
#define LOW_L0_INDEX 0u
#define KERNEL_L0_INDEX 511u
/* Level-1 entries of TTBR1 slot 511 that make up the direct map. */
#define DIRECT_MAP_L1_COUNT ((uint32_t)(KERNEL_HIGHER_HALF_WINDOW_BYTES / LEVEL1_BLOCK))

static uint64_t g_kernel_root;  /* TTBR0 table of the kernel address space */
static uint64_t g_kernel_hi_l0; /* TTBR1 table, shared */

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];
extern void aarch64_switch_ttbr1(uint64_t ttbr1);

static volatile uint64_t* table_ptr(uint64_t phys) {
    return ptr_cast(uint64_t, (phys | KERNEL_HIGHER_HALF_BASE));
}

static uint64_t entry_phys(uint64_t entry) {
    return entry & DESC_OA_MASK;
}

static int entry_is_table(uint64_t entry) {
    return (entry & (DESC_VALID | DESC_TABLE)) == (DESC_VALID | DESC_TABLE);
}

static int entry_is_block(uint64_t entry) {
    return (entry & (DESC_VALID | DESC_TABLE)) == DESC_VALID;
}

static int virt_is_upper(uint64_t virt) {
    return (virt >> 48) == 0xFFFFu;
}

static int virt_is_lower(uint64_t virt) {
    return (virt >> 48) == 0u;
}

static uint32_t l0_index(uint64_t virt) {
    return (uint32_t)((virt >> 39) & 0x1FFu);
}

static uint32_t l1_index(uint64_t virt) {
    return (uint32_t)((virt >> 30) & 0x1FFu);
}

static uint32_t l2_index(uint64_t virt) {
    return (uint32_t)((virt >> 21) & 0x1FFu);
}

static uint32_t l3_index(uint64_t virt) {
    return (uint32_t)((virt >> 12) & 0x1FFu);
}

/* The level-0 table a walk of `virt` starts from: the shared TTBR1 table for
 * an upper-half VA, root_table for a lower-half one, 0 for a non-canonical
 * VA. */
static uint64_t walk_l0(uint64_t root_table, uint64_t virt) {
    if (virt_is_upper(virt)) {
        return g_kernel_hi_l0;
    }
    if (virt_is_lower(virt)) {
        return root_table;
    }
    return 0;
}

static void zero_table(uint64_t phys) {
    volatile uint64_t* t = table_ptr(phys);
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        t[i] = 0;
    }
}

static int alloc_table(uint64_t* out_phys) {
    uint64_t phys = pfa_alloc_pages_below(1, KERNEL_HIGHER_HALF_WINDOW_BYTES);
    if (!phys) {
        return -1;
    }
    zero_table(phys);
    *out_phys = phys;
    return 0;
}

/* Make table and descriptor stores visible to the table walker before a
 * translation that depends on them (ARM ARM D8.2.6). */
static void table_store_barrier(void) {
    __asm__ volatile("dsb ishst\n"
                     "isb" ::
                         : "memory");
}

static void tlb_invalidate_page(uint64_t virt) {
    uint64_t operand = (virt >> 12) & 0xFFFFFFFFFFFULL;
    __asm__ volatile("dsb ishst\n"
                     "tlbi vaae1is, %0\n"
                     "dsb ish\n"
                     "isb" ::"r"(operand)
                     : "memory");
}

static void tlb_invalidate_all(void) {
    __asm__ volatile("dsb ishst\n"
                     "tlbi vmalle1is\n"
                     "dsb ish\n"
                     "isb" ::
                         : "memory");
}

void paging_invalidate(uint64_t virt) {
    tlb_invalidate_page(virt);
}

/* Clean the data cache and invalidate the instruction cache to the point of
 * unification over [addr, addr + len), so instructions written through a data
 * mapping are what the next fetch sees (ARM ARM D7.5.9). Line sizes come from
 * CTR_EL0. */
void paging_sync_instruction_cache(const void* addr, uint64_t len) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t dline = 4ULL << ((ctr >> 16) & 0xFu);
    uint64_t iline = 4ULL << (ctr & 0xFu);
    uint64_t start = (uint64_t)(uintptr_t)addr;
    uint64_t end = start + len;
    for (uint64_t p = start & ~(dline - 1u); p < end; p += dline) {
        __asm__ volatile("dc cvau, %0" ::"r"(p) : "memory");
    }
    __asm__ volatile("dsb ish" ::: "memory");
    for (uint64_t p = start & ~(iline - 1u); p < end; p += iline) {
        __asm__ volatile("ic ivau, %0" ::"r"(p) : "memory");
    }
    __asm__ volatile("dsb ish\n"
                     "isb" ::
                         : "memory");
}

/* Leaf attributes for a MEM_REGION_FLAG_* mask. Kernel mappings are never
 * executable at EL0 and user mappings never at EL1; device memory is never
 * executable. User mappings are not global, so they are tagged by ASID once
 * ASIDs exist. */
static uint64_t leaf_attrs(uint64_t flags) {
    uint64_t attrs = DESC_VALID | DESC_AF;
    if (flags & MEM_REGION_FLAG_DEVICE) {
        attrs |= DESC_ATTR_DEVICE | DESC_PXN | DESC_UXN;
    } else {
        attrs |= DESC_ATTR_NORMAL | DESC_SH_INNER;
    }
    if (!(flags & MEM_REGION_FLAG_WRITE)) {
        attrs |= DESC_AP_RDONLY;
    }
    if (flags & MEM_REGION_FLAG_USER) {
        attrs |= DESC_AP_EL0 | DESC_NG | DESC_PXN;
        if (!(flags & MEM_REGION_FLAG_EXEC)) {
            attrs |= DESC_UXN;
        }
    } else {
        attrs |= DESC_UXN;
        if (!(flags & MEM_REGION_FLAG_EXEC)) {
            attrs |= DESC_PXN;
        }
    }
    return attrs;
}

/* Whether a block already provides the translation a 4 KiB request asks for:
 * same frame, same memory type, writable when write is asked. Such a request
 * changes nothing worth splitting the block for. */
static int block_satisfies(uint64_t block, uint64_t block_size, uint64_t virt, uint64_t phys,
                           uint64_t flags) {
    uint64_t mapped = entry_phys(block) + (virt & (block_size - 1u));
    if (mapped != (phys & ~0xFFFULL)) {
        return 0;
    }
    uint64_t want_type = (flags & MEM_REGION_FLAG_DEVICE) ? DESC_ATTR_DEVICE : DESC_ATTR_NORMAL;
    if ((block & DESC_ATTR_MASK) != want_type) {
        return 0;
    }
    if ((flags & MEM_REGION_FLAG_WRITE) && (block & DESC_AP_RDONLY)) {
        return 0;
    }
    if ((flags & MEM_REGION_FLAG_USER) != 0) {
        return 0;
    }
    return 1;
}

/* Replace the block at *entry, which maps `block_size` bytes at `block_va`,
 * with a next-level table of equivalent blocks or pages, by break-before-make
 * (ARM ARM D8.16.1): the entry is invalidated and the TLB flushed before the
 * table is installed. Between the two, every access to the range faults, so a
 * block covering the running kernel image is refused: it would fault the CPU
 * performing the split. Returns 0 or -1. */
static int split_block(volatile uint64_t* entry, uint64_t block_va, uint64_t block_size) {
    uint64_t block = *entry;
    uint64_t base = entry_phys(block);
    uint64_t kernel_lo = (uint64_t)(uintptr_t)__kernel_start - KERNEL_HIGHER_HALF_BASE;
    uint64_t kernel_hi = (uint64_t)(uintptr_t)__kernel_end - KERNEL_HIGHER_HALF_BASE;
    if (base < kernel_hi && kernel_lo < base + block_size) {
        klog_printf("[paging] refusing to split the block holding the kernel va=%016llx\n",
                    (unsigned long long)block_va);
        return -1;
    }
    uint64_t next_size = block_size == LEVEL1_BLOCK ? LEVEL2_BLOCK : PAGE_SIZE_4K;
    uint64_t kind = next_size == PAGE_SIZE_4K ? DESC_PAGE : 0;
    uint64_t table = 0;
    if (alloc_table(&table) != 0) {
        return -1;
    }
    volatile uint64_t* t = table_ptr(table);
    uint64_t attrs = block & DESC_LEAF_ATTRS;
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        t[i] = (base + (uint64_t)i * next_size) | attrs | DESC_VALID | kind;
    }
    *entry = 0;
    tlb_invalidate_all();
    *entry = table | DESC_VALID | DESC_TABLE;
    table_store_barrier();
    return 0;
}

/* The next-level table below *entry, allocating it when the entry is invalid.
 * A block is split first. Returns 0 with *out_phys set, or -1. */
static int ensure_table(volatile uint64_t* entry, uint64_t entry_va, uint64_t entry_size,
                        uint64_t* out_phys) {
    if (entry_is_table(*entry)) {
        *out_phys = entry_phys(*entry);
        return 0;
    }
    if (entry_is_block(*entry)) {
        if (split_block(entry, entry_va, entry_size) != 0) {
            return -1;
        }
        *out_phys = entry_phys(*entry);
        return 0;
    }
    uint64_t phys = 0;
    if (alloc_table(&phys) != 0) {
        return -1;
    }
    *entry = phys | DESC_VALID | DESC_TABLE;
    table_store_barrier();
    *out_phys = phys;
    return 0;
}

/* Map one GiB of RAM-or-reserved physical space into the direct map below
 * *l1_entry: 4 KiB pages over every 2 MiB chunk that holds RAM, so a single
 * page can later be unmapped (stack guard pages) or remapped without splitting
 * a live block, and a Normal 2 MiB block over the rest (firmware-reserved
 * memory such as ACPI tables). Returns 0 or -1. */
static int build_direct_map_gib(volatile uint64_t* l1_entry, uint64_t gib_base) {
    uint64_t l2 = 0;
    if (alloc_table(&l2) != 0) {
        return -1;
    }
    volatile uint64_t* t2 = table_ptr(l2);
    uint64_t attrs = leaf_attrs(MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_EXEC);
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        uint64_t chunk = gib_base + (uint64_t)i * LEVEL2_BLOCK;
        if (!pfa_range_overlaps_ram(chunk, LEVEL2_BLOCK)) {
            t2[i] = chunk | attrs;
            continue;
        }
        uint64_t l3 = 0;
        if (alloc_table(&l3) != 0) {
            return -1;
        }
        volatile uint64_t* t3 = table_ptr(l3);
        for (uint32_t j = 0; j < ENTRIES_PER_TABLE; ++j) {
            t3[j] = (chunk + (uint64_t)j * PAGE_SIZE_4K) | attrs | DESC_PAGE;
        }
        t2[i] = l3 | DESC_VALID | DESC_TABLE;
    }
    *l1_entry = l2 | DESC_VALID | DESC_TABLE;
    return 0;
}

/* Builds the kernel's own tables and switches both TTBRs to them, replacing
 * entry.S's bootstrap tables:
 *   - a TTBR1 level-0 table whose slot 511 holds the direct map: the device
 *     GiB (physical [0, 1 GiB) on QEMU virt) as one Device-nGnRE block, the
 *     RAM GiBs as pages and 2 MiB blocks (build_direct_map_gib);
 *   - the kernel root, whose low slot is the identity map of the window as
 *     1 GiB blocks, exactly as the bootstrap built it.
 * The TTBR0 switch keeps every translation and block size, so it is a plain
 * register write. The TTBR1 switch changes block sizes under live
 * translations, so it runs through aarch64_switch_ttbr1's identity alias.
 * Every table frame comes from below the direct-map window and is never freed.
 * Returns 0, or -1 when a frame could not be allocated. */
int paging_init(void) {
    uint64_t root = 0;
    uint64_t low_l1 = 0;
    uint64_t hi_l0 = 0;
    uint64_t hi_l1 = 0;
    if (alloc_table(&root) != 0 || alloc_table(&low_l1) != 0 || alloc_table(&hi_l0) != 0 ||
        alloc_table(&hi_l1) != 0) {
        klog_write("[paging] table alloc failed\n");
        return -1;
    }
    volatile uint64_t* low = table_ptr(low_l1);
    volatile uint64_t* high = table_ptr(hi_l1);
    uint64_t device = leaf_attrs(MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_DEVICE);
    uint64_t normal = leaf_attrs(MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_EXEC);
    low[0] = device;
    high[0] = device;
    for (uint32_t i = 1; i < DIRECT_MAP_L1_COUNT; ++i) {
        uint64_t gib = (uint64_t)i * LEVEL1_BLOCK;
        low[i] = gib | normal;
        if (build_direct_map_gib(&high[i], gib) != 0) {
            klog_write("[paging] direct-map table alloc failed\n");
            return -1;
        }
    }
    table_ptr(root)[LOW_L0_INDEX] = low_l1 | DESC_VALID | DESC_TABLE;
    table_ptr(hi_l0)[KERNEL_L0_INDEX] = hi_l1 | DESC_VALID | DESC_TABLE;
    table_store_barrier();
    __asm__ volatile("msr ttbr0_el1, %0\n"
                     "isb" ::"r"(root)
                     : "memory");
    tlb_invalidate_all();
    void (*switch_ttbr1)(uint64_t) =
        (void (*)(uint64_t))((uintptr_t)aarch64_switch_ttbr1 - (uintptr_t)KERNEL_HIGHER_HALF_BASE);
    switch_ttbr1(hi_l0);
    g_kernel_hi_l0 = hi_l0;
    g_kernel_root = root;
    klog_printf("[paging] ttbr0=%016llx ttbr1=%016llx\n[paging] higher-half=%016llx\n",
                (unsigned long long)root,
                (unsigned long long)hi_l0,
                (unsigned long long)KERNEL_HIGHER_HALF_BASE);
    return 0;
}

uint64_t paging_get_higher_half_base(void) {
    return KERNEL_HIGHER_HALF_BASE;
}

uint64_t paging_get_root_table(void) {
    return g_kernel_root;
}

/* This CPU's TTBR0 table, without the ASID field. */
uint64_t paging_get_current_root_table(void) {
    uint64_t ttbr0;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    return ttbr0 & DESC_OA_MASK;
}

/* Load root_table into TTBR0 and drop every cached translation. The kernel
 * runs from TTBR1, which a root switch does not touch, so no code or stack
 * straddles the switch. */
int paging_switch_root(uint64_t root_table) {
    if (!root_table) {
        return -1;
    }
    __asm__ volatile("msr ttbr0_el1, %0\n"
                     "isb" ::"r"(root_table)
                     : "memory");
    tlb_invalidate_all();
    return 0;
}

static int verify_user_root(uint64_t root_table, int log_failures) {
    if (!root_table || !g_kernel_root) {
        return -1;
    }
    volatile uint64_t* root = table_ptr(root_table);
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        if (i == LOW_L0_INDEX || i == USER_L0_INDEX) {
            continue;
        }
        if (root[i] & DESC_VALID) {
            if (log_failures) {
                klog_printf("[paging] verify fail: unexpected l0[%u]=%016llx\n",
                            (unsigned int)i,
                            (unsigned long long)root[i]);
            }
            return -1;
        }
    }
    /* The low slot, when present, carries no identity entries in a user root
     * (x86_64's IDENTITY_PD_COUNT baseline of 0). */
    if (root[LOW_L0_INDEX] & DESC_VALID) {
        volatile uint64_t* low = table_ptr(entry_phys(root[LOW_L0_INDEX]));
        for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
            if (low[i] & DESC_VALID) {
                if (log_failures) {
                    klog_printf("[paging] verify fail: low l1[%u]=%016llx\n",
                                (unsigned int)i,
                                (unsigned long long)low[i]);
                }
                return -1;
            }
        }
    }
    return 0;
}

/* A fresh TTBR0 table with the low and user slots empty. The kernel half needs
 * no copy: TTBR1 is shared. */
int paging_create_address_space(uint64_t* out_root_table) {
    if (!out_root_table || !g_kernel_root) {
        return -1;
    }
    uint64_t root = 0;
    if (alloc_table(&root) != 0) {
        return -1;
    }
    if (verify_user_root(root, 1) != 0) {
        pfa_free_pages(root, 1);
        return -1;
    }
    *out_root_table = root;
    return 0;
}

/* Free the table frames below `entry` down to `levels` further levels; leaf
 * frames (pages and blocks) are left to their owners. */
static void free_subtree(uint64_t entry, uint32_t levels) {
    if (!entry_is_table(entry)) {
        return;
    }
    uint64_t table = entry_phys(entry);
    if (levels > 1u) {
        volatile uint64_t* t = table_ptr(table);
        for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
            free_subtree(t[i], levels - 1u);
        }
    }
    pfa_free_pages(table, 1);
}

/* Free the user slot's level-1/2/3 tables, a private low-slot level-1 table,
 * and the root. Leaf frames belong to the region and shared-memory owners. */
void paging_destroy_address_space(uint64_t root_table) {
    if (!root_table || root_table == g_kernel_root) {
        return;
    }
    volatile uint64_t* root = table_ptr(root_table);
    free_subtree(root[USER_L0_INDEX], 3u);
    uint64_t kernel_low = entry_phys(table_ptr(g_kernel_root)[LOW_L0_INDEX]);
    if ((root[LOW_L0_INDEX] & DESC_VALID) && entry_phys(root[LOW_L0_INDEX]) != kernel_low) {
        pfa_free_pages(entry_phys(root[LOW_L0_INDEX]), 1);
    }
    pfa_free_pages(root_table, 1);
}

/* Deep-copy `src` (a level-`level` table) into a fresh table; leaves are
 * copied by value. Returns the new table's frame, or 0 on exhaustion. */
static uint64_t clone_table(uint64_t src, uint32_t level) {
    uint64_t dst = 0;
    if (alloc_table(&dst) != 0) {
        return 0;
    }
    volatile uint64_t* s = table_ptr(src);
    volatile uint64_t* d = table_ptr(dst);
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        uint64_t e = s[i];
        if (level < 3u && entry_is_table(e)) {
            uint64_t child = clone_table(entry_phys(e), level + 1u);
            if (!child) {
                return 0;
            }
            d[i] = child | (e & ~DESC_OA_MASK);
        } else {
            d[i] = e;
        }
    }
    return dst;
}

int paging_clone_low_slot_in_root(uint64_t root_table) {
    if (!root_table || !g_kernel_root) {
        return -1;
    }
    uint64_t src = table_ptr(g_kernel_root)[LOW_L0_INDEX];
    if (!entry_is_table(src)) {
        return -1;
    }
    uint64_t copy = clone_table(entry_phys(src), 1u);
    if (!copy) {
        return -1;
    }
    table_ptr(root_table)[LOW_L0_INDEX] = copy | DESC_VALID | DESC_TABLE;
    table_store_barrier();
    return 0;
}

int paging_strip_low_slot_in_root(uint64_t root_table) {
    if (!root_table || root_table == g_kernel_root) {
        return -1;
    }
    volatile uint64_t* root = table_ptr(root_table);
    uint64_t low = root[LOW_L0_INDEX];
    if (!(low & DESC_VALID)) {
        return verify_user_root(root_table, 0);
    }
    root[LOW_L0_INDEX] = 0;
    if (paging_get_current_root_table() == root_table) {
        tlb_invalidate_all();
    }
    uint64_t kernel_low = entry_phys(table_ptr(g_kernel_root)[LOW_L0_INDEX]);
    if (entry_phys(low) != kernel_low) {
        pfa_free_pages(entry_phys(low), 1);
    }
    return verify_user_root(root_table, 0);
}

int paging_map_4k_in_root(uint64_t root_table, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!root_table) {
        return -1;
    }
    uint64_t l0 = walk_l0(root_table, virt);
    if (!l0) {
        return -1;
    }
    uint8_t user_slot = (uint8_t)(virt_is_lower(virt) && l0_index(virt) == USER_L0_INDEX);
    if (user_slot != ((flags & MEM_REGION_FLAG_USER) != 0)) {
        return -1;
    }
    if ((flags & MEM_REGION_FLAG_USER) && (flags & MEM_REGION_FLAG_WRITE) &&
        (flags & MEM_REGION_FLAG_EXEC)) {
        return -1;
    }

    volatile uint64_t* t0 = table_ptr(l0);
    uint64_t l1 = 0;
    if (ensure_table(&t0[l0_index(virt)], 0, 0, &l1) != 0) {
        return -1;
    }
    volatile uint64_t* t1 = table_ptr(l1);
    volatile uint64_t* e1 = &t1[l1_index(virt)];
    if (entry_is_block(*e1) && block_satisfies(*e1, LEVEL1_BLOCK, virt, phys, flags)) {
        return 0;
    }
    uint64_t l2 = 0;
    if (ensure_table(e1, virt & ~(LEVEL1_BLOCK - 1u), LEVEL1_BLOCK, &l2) != 0) {
        return -1;
    }
    volatile uint64_t* t2 = table_ptr(l2);
    volatile uint64_t* e2 = &t2[l2_index(virt)];
    if (entry_is_block(*e2) && block_satisfies(*e2, LEVEL2_BLOCK, virt, phys, flags)) {
        return 0;
    }
    uint64_t l3 = 0;
    if (ensure_table(e2, virt & ~(LEVEL2_BLOCK - 1u), LEVEL2_BLOCK, &l3) != 0) {
        return -1;
    }
    volatile uint64_t* pte = &table_ptr(l3)[l3_index(virt)];
    uint64_t desc = (phys & DESC_OA_MASK) | leaf_attrs(flags) | DESC_PAGE;
    if (*pte & DESC_VALID) {
        /* Remap of a live page: break before make. */
        *pte = 0;
        tlb_invalidate_page(virt);
    }
    *pte = desc;
    table_store_barrier();
    return 0;
}

int paging_unmap_4k_in_root(uint64_t root_table, uint64_t virt) {
    if (!root_table) {
        return -1;
    }
    uint64_t l0 = walk_l0(root_table, virt);
    if (!l0) {
        return -1;
    }
    volatile uint64_t* e0 = &table_ptr(l0)[l0_index(virt)];
    if (!entry_is_table(*e0)) {
        return -1;
    }
    volatile uint64_t* e1 = &table_ptr(entry_phys(*e0))[l1_index(virt)];
    if (entry_is_block(*e1) && split_block(e1, virt & ~(LEVEL1_BLOCK - 1u), LEVEL1_BLOCK) != 0) {
        return -1;
    }
    if (!entry_is_table(*e1)) {
        return -1;
    }
    volatile uint64_t* e2 = &table_ptr(entry_phys(*e1))[l2_index(virt)];
    if (entry_is_block(*e2) && split_block(e2, virt & ~(LEVEL2_BLOCK - 1u), LEVEL2_BLOCK) != 0) {
        return -1;
    }
    if (!entry_is_table(*e2)) {
        return -1;
    }
    volatile uint64_t* pte = &table_ptr(entry_phys(*e2))[l3_index(virt)];
    if (!(*pte & DESC_VALID)) {
        return -1;
    }
    *pte = 0;
    tlb_invalidate_page(virt);
    return 0;
}

int paging_map_4k(uint64_t virt, uint64_t phys, uint64_t flags) {
    return paging_map_4k_in_root(paging_get_current_root_table(), virt, phys, flags);
}

int paging_unmap_4k(uint64_t virt) {
    return paging_unmap_4k_in_root(paging_get_current_root_table(), virt);
}

uint64_t paging_virt_to_phys_in_root(uint64_t root_table, uint64_t virt) {
    if (root_table == 0) {
        root_table = paging_get_current_root_table();
    }
    uint64_t l0 = walk_l0(root_table, virt);
    if (!l0) {
        return 0;
    }
    uint64_t e = table_ptr(l0)[l0_index(virt)];
    if (!entry_is_table(e)) {
        return 0;
    }
    e = table_ptr(entry_phys(e))[l1_index(virt)];
    if (entry_is_block(e)) {
        return (e & 0x0000FFFFC0000000ULL) | (virt & (LEVEL1_BLOCK - 1u));
    }
    if (!entry_is_table(e)) {
        return 0;
    }
    e = table_ptr(entry_phys(e))[l2_index(virt)];
    if (entry_is_block(e)) {
        return (e & 0x0000FFFFFFE00000ULL) | (virt & (LEVEL2_BLOCK - 1u));
    }
    if (!entry_is_table(e)) {
        return 0;
    }
    e = table_ptr(entry_phys(e))[l3_index(virt)];
    if (!(e & DESC_VALID)) {
        return 0;
    }
    return entry_phys(e) | (virt & 0xFFFULL);
}

uint64_t paging_virt_to_phys(uint64_t virt) {
    return paging_virt_to_phys_in_root(0, virt);
}

int paging_verify_user_root(uint64_t root_table, int log_failures) {
    return verify_user_root(root_table, log_failures ? 1 : 0);
}

int paging_verify_user_root_no_low_slot(uint64_t root_table, int log_failures) {
    if (verify_user_root(root_table, log_failures ? 1 : 0) != 0) {
        return -1;
    }
    volatile uint64_t* root = table_ptr(root_table);
    if (root[LOW_L0_INDEX] & DESC_VALID) {
        if (log_failures) {
            klog_printf("[paging] verify fail: low slot still present l0[0]=%016llx\n",
                        (unsigned long long)root[LOW_L0_INDEX]);
        }
        return -1;
    }
    return 0;
}

/* Diagnostic: the root's low and user slots, and every present level-1 entry
 * of the shared kernel half. */
void paging_dump_user_root_kernel_mappings(uint64_t root_table) {
    if (!root_table || !g_kernel_hi_l0) {
        return;
    }
    volatile uint64_t* root = table_ptr(root_table);
    uint64_t hi = table_ptr(g_kernel_hi_l0)[KERNEL_L0_INDEX];
    klog_printf("[paging] dump root=%016llx l0[0]=%016llx l0[1]=%016llx ttbr1 l0[511]=%016llx\n",
                (unsigned long long)root_table,
                (unsigned long long)root[LOW_L0_INDEX],
                (unsigned long long)root[USER_L0_INDEX],
                (unsigned long long)hi);
    if (!entry_is_table(hi)) {
        return;
    }
    volatile uint64_t* l1 = table_ptr(entry_phys(hi));
    for (uint32_t i = 0; i < ENTRIES_PER_TABLE; ++i) {
        if (l1[i] & DESC_VALID) {
            klog_printf(
                "[paging] dump: hi l1[%u]=%016llx\n", (unsigned int)i, (unsigned long long)l1[i]);
        }
    }
}
