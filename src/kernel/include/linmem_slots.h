/* linmem_slots.h - Reserved-VA, lazy-commit linear-memory slot allocator.
 *
 * Backs a WASM module's linear memory in a dedicated kernel higher-half VA slot
 * (the paging.h WARP_LINMEM_* window): the VA is reserved once, and scattered
 * physical pages are committed on demand as the block grows, so the base is
 * pinned for the module's lifetime and never relocates.  The window lives under
 * the shared PML4[511], so a slot VA is dereferenceable from any CR3 without a
 * switch, and the mapping is SUPERVISOR-only (unreachable from ring 3).
 *
 * Shared by the WARP and wasm3 kernel shims so both backends use one
 * non-relocating linear-memory mechanism.  Callers layer their own per-block
 * header and per-process bookkeeping on top; this primitive only owns the slot
 * pool and the map/zero/unmap of scattered pages. */
#ifndef WASMOS_LINMEM_SLOTS_H
#define WASMOS_LINMEM_SLOTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINMEM_SLOT_NONE 0xFFFFFFFFu

/* Number of slots in the pool. */
uint32_t linmem_slot_count(void);

/* Reserve a free slot index, or -1 if the pool is exhausted.  Slots are
 * recycled by linmem_slot_release, so this bounds concurrent WASM apps, not
 * total spawned. */
int linmem_slot_alloc(void);

/* Return a reserved slot to the pool.  Does NOT unmap pages; call
 * linmem_slot_decommit first if any were committed. */
void linmem_slot_release(uint32_t slot);

/* Kernel VA base of a slot index. */
uint64_t linmem_slot_va(uint32_t slot);

/* Commit (map + zero) scattered physical pages into slot-VA pages [from,to).
 * SUPERVISOR-only mapping.  Returns 0 on success, or -1 if a frame could not be
 * allocated or mapped (pages already committed earlier in this call stay
 * mapped; the caller treats a partial failure as fatal for the slot). */
int linmem_slot_commit(uint64_t va_base, uint64_t from_page, uint64_t to_page);

/* Walk-unmap and free `pages` committed pages starting at va_base. */
void linmem_slot_decommit(uint64_t va_base, uint64_t pages);

/* True when va lies inside the slot VA window (any slot, allocated or not). */
int linmem_slot_contains(uint64_t va);

/* Tag a slot with the pid whose linear memory it backs, for fault reporting.
 * Pass 0 to clear. */
void linmem_slot_set_owner(uint32_t slot, uint32_t pid);

/* Classify a faulting address against the slot window, for the #PF reporter.
 * Returns 0 and fills the outputs when `va` lies inside the window, else -1.
 * reserved=0 means the slot was released while this VA was still in use;
 * reserved=1 with present=0 means the page was never committed or was
 * decommitted; present=1 means the mapping exists (stale TLB). */
int linmem_slot_fault_info(uint64_t va, uint32_t* out_slot, uint32_t* out_owner_pid,
                           uint64_t* out_slot_offset, int* out_reserved, int* out_present);

#ifdef __cplusplus
}
#endif

#endif /* WASMOS_LINMEM_SLOTS_H */