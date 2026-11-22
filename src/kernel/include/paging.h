/* paging.h - page-table management: map, unmap, walk, and root switch.
 *
 * The interface is architecture-neutral: mapping flags are MEM_REGION_FLAG_*
 * (memory.h), never raw page-table bits, and a root table is an opaque physical
 * address. The implementation is per architecture (arch/x86_64/paging.c,
 * arch/aarch64/paging.c), and the descriptions below name the x86_64
 * structures -- PML4 slots, CR3, INVLPG -- where the contract depends on them.
 * aarch64 uses the same four-level slot geometry: a root is a TTBR0 table
 * (slot 0 low identity, slot 1 user), and PML4[511] is the TTBR1 table every
 * address space shares, so nothing of it is copied or verified per root. The
 * kernel virtual layout constants come from arch/vm_layout.h. */
#ifndef WASMOS_PAGING_H
#define WASMOS_PAGING_H

#include <stdint.h>
#include "arch/vm_layout.h"

/* Build the kernel root page table and load it into CR3, replacing the firmware's
 * identity map.  Maps the shared higher-half window (see KERNEL_HIGHER_HALF_BASE) and
 * keeps a low identity PDPT in the kernel root only, so early code that still writes
 * through low physical addresses keeps working.  Returns 0 on success, -1 if any page-
 * table frame could not be allocated.  Call once, before any other paging entry point;
 * every table frame comes from pfa_alloc_pages_below and is never freed. */
int paging_init(void);

/* Value of KERNEL_HIGHER_HALF_BASE, for callers that convert physical addresses to the
 * kernel alias without including this header's constants. */
uint64_t paging_get_higher_half_base(void);

/* Physical address of the kernel root PML4 established by paging_init, or 0 before it.
 * This is the root idle/kernel threads run on, not necessarily the live CR3. */
uint64_t paging_get_root_table(void);

/* Physical root-table address read straight out of this CPU's CR3.  Under SMP this is
 * the only trustworthy "current root": the global mirror maintained by
 * paging_switch_root is last-writer-wins across all CPUs. */
uint64_t paging_get_current_root_table(void);

/* Load root_table (a physical PML4 address) into CR3 and update the global mirror.
 * Returns 0 on success and -1 when root_table is 0; no other validation is performed,
 * so the caller owns the guarantee that the root maps the code that follows the switch.
 * Implemented naked so no prologue/spill straddles the CR3 write. */
int paging_switch_root(uint64_t root_table);

/* Allocate a fresh PML4 for a new process and seed it with only the shared kernel
 * mappings: PML4[511] is copied from the kernel root, PML4[0] is left empty at the
 * current IDENTITY_PD_COUNT baseline, and PML4[1] (the user slot) stays private and
 * empty.  On success writes the new root's physical address to *out_root_table and
 * returns 0.  Returns -1 on a NULL out_root_table, before paging_init, on allocation
 * failure, or when the freshly built root fails paging_verify_user_root; the partially
 * built root is freed in that case.  The caller owns the returned root and releases it
 * with paging_destroy_address_space. */
int paging_create_address_space(uint64_t* out_root_table);

/* Free the page-table structures of a user root: the PDPT/PD/PT frames below the user
 * slot PML4[1], a private low PDPT in PML4[0], and the root frame itself.  Leaf data
 * frames are deliberately left alone — region and shared-memory ownership frees those,
 * so calling this before mm_context_destroy leaks the mapped pages rather than double-
 * freeing them.  A 0 root_table and the kernel root are ignored. */
void paging_destroy_address_space(uint64_t root_table);

/* Deep-copy the kernel root's low identity mapping (PML4[0] and every PDPT/PD/PT below
 * it) into root_table, giving that address space a private, writable copy of the
 * identity window.  Large-page PD entries are shared by value; 4 KiB tables are
 * duplicated.  Returns 0 on success, -1 when root_table is 0, when the kernel root has
 * no low slot, or on allocation failure.  Any previous PML4[0] of root_table is
 * overwritten without being freed. */
int paging_clone_low_slot_in_root(uint64_t root_table);

/* Map one 4 KiB page: virt -> phys, in the address space rooted at the physical address
 * root_table, allocating missing PDPT/PD/PT levels and exploding a covering 2 MiB large
 * page into 4 KiB entries when needed.  `flags` is a MEM_REGION_FLAG_* mask (memory.h),
 * not raw PTE bits: WRITE sets the writable bit, EXEC clears NX, USER sets the user bit.
 * Both addresses are truncated to 4 KiB granularity.  Refuses (returns -1) when the USER
 * flag disagrees with the slot — user VAs (PML4[1]) require it and every other VA
 * forbids it, which is what keeps the kernel higher-half alias out of ring-3 reach — and
 * when a user mapping asks for WRITE and EXEC together (W^X).  Remapping an already
 * present PTE is allowed and replaces it in place.  Invalidates the TLB entry on the
 * calling CPU only; other CPUs running the same root need a separate shootdown.
 * Returns 0 on success, -1 on a 0 root_table, a rejected flag combination, or a failed
 * table allocation. */
int paging_map_4k_in_root(uint64_t root_table, uint64_t virt, uint64_t phys, uint64_t flags);

/* Clear the PTE for one 4 KiB page in root_table and invalidate it on this CPU.  A
 * covering 2 MiB large page is first exploded into 4 KiB entries so only the requested
 * page is removed.  Does not free the physical frame that was mapped.  Returns 0 on
 * success, -1 when root_table is 0, when any level is absent, or when the large-page
 * split fails. */
int paging_unmap_4k_in_root(uint64_t root_table, uint64_t virt);

/* Remove the low identity mapping (PML4[0]) from a user root, the last step before that
 * address space runs ring-3 code with no identity window.  Reloads CR3 when root_table
 * is the live root, and frees the low PDPT frame unless it is the one the kernel root
 * uses.  Returns the result of the user-root verification (0 = still well-formed), or
 * -1 for a 0 root_table or the kernel root, which is never stripped.  Frees only the
 * PDPT frame — the PD/PT frames below it are leaked if the low slot was a private
 * clone. */
int paging_strip_low_slot_in_root(uint64_t root_table);

/* paging_map_4k_in_root / paging_unmap_4k_in_root against this CPU's live CR3. */
int paging_map_4k(uint64_t virt, uint64_t phys, uint64_t flags);
int paging_unmap_4k(uint64_t virt);

/* Invalidate the TLB entry covering `virt` on the calling CPU (INVLPG).  Needed after
 * writing a PTE by hand; the map/unmap helpers already do it for the page they touch. */
void paging_invalidate(uint64_t virt);

/* Make `len` bytes of instructions just written at `addr` through a data
 * mapping visible to instruction fetch, before any CPU executes them. Callers
 * are code loaders (WARP JIT and AOT). A no-op on x86_64, where instruction
 * fetch is coherent with data writes; an architecture with split, non-coherent
 * caches cleans the data cache and invalidates the instruction cache here. */
void paging_sync_instruction_cache(const void* addr, uint64_t len);

/* Read-only page-table walk: physical address backing `virt` (root_table 0 =
 * current root), including in-page offset, or 0 if unmapped.  Handles large
 * pages.  Used to recover scattered phys from a dedicated-VA linmem pointer. */
uint64_t paging_virt_to_phys_in_root(uint64_t root_table, uint64_t virt);

/* paging_virt_to_phys_in_root against this CPU's live CR3.  0 means "not mapped", which
 * is indistinguishable from a legitimate mapping of physical frame 0. */
uint64_t paging_virt_to_phys(uint64_t virt);

/* Check that a user root still has the shape paging_create_address_space produced:
 * PML4[511] identical to the kernel root's, no present PML4 slot other than 0, 1 and
 * 511, exactly the kernel and MMIO slots present in the shared higher-half PDPT (the
 * WARP linmem window is skipped, since it commits on demand), and no low PDPT entry
 * beyond IDENTITY_PD_COUNT.  Returns 0 when the root is well-formed and -1 otherwise;
 * a non-zero log_failures prints the first offending entry.  Read-only. */
int paging_verify_user_root(uint64_t root_table, int log_failures);

/* paging_verify_user_root plus the requirement that PML4[0] is absent — the state a
 * root must be in before it runs ring-3 code.  Returns 0 when both hold, -1 otherwise. */
int paging_verify_user_root_no_low_slot(uint64_t root_table, int log_failures);

/* Print PML4[0], PML4[1], PML4[511] and every present entry of the shared higher-half
 * PDPT of root_table through klog.  Diagnostic only; a 0 root_table is ignored. */
void paging_dump_user_root_kernel_mappings(uint64_t root_table);

#endif
