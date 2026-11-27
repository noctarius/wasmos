/* warp/mem_utils_kernel.cpp - Kernel implementation of vb::MemUtils.
 *
 * Replaces WARP's MemUtils.cpp and ExecutableMemory.cpp entirely.
 * All JIT code pages are allocated from the kernel physical frame allocator
 * and accessed via the existing higher-half identity mapping (RWX).
 *
 * Page model:
 *   - Higher-half base: KERNEL_HIGHER_HALF_BASE = 0xFFFFFFFF80000000
 *   - Physical frames up to 512 MB are mapped as 2 MB large pages with
 *     PRESENT | WRITE | no-NX, so they are already RWX.
 *   - virt = phys | KERNEL_HIGHER_HALF_BASE (valid for phys < 512 MB).
 *
 * mprotect is a no-op: all JIT pages remain RWX.  A future W^X hardening
 * pass can use paging_map_4k to split large pages and flip permissions. */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

extern "C" {
#include "physmem.h"
#include "paging.h"
#include "slab.h"
#include "klog.h"
#include "memory.h"
#include "serial.h"
#include "sync/spinlock.h"
#ifdef WASMOS_WASM_RUNTIME_WARP
#include "warp_ring3.h"
#endif
}

#include "src/utils/MemUtils.hpp"
#include "src/utils/ExecutableMemory.hpp"

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

namespace {

constexpr uint64_t kPageSize = 4096ULL;
constexpr uint64_t kHalfBase = 0xFFFFFFFF80000000ULL;
/* Stay within the 512 MB higher-half window the kernel already maps. */
constexpr uint64_t kPhysLimit = 512ULL * 1024ULL * 1024ULL;

inline uint64_t round_up_page(uint64_t n) {
    return (n + kPageSize - 1) & ~(kPageSize - 1);
}

#ifdef WASMOS_WASM_RUNTIME_WARP
extern "C" int warp_linmem_kernel_window_query(const uint8_t* linmem_kernel_ptr,
                                               uint64_t* out_slot_va_base,
                                               uint64_t* out_basedata_length,
                                               uint64_t* out_committed_pages);

static inline bool is_ring3_linmem_kernel_window(uint64_t virt) {
    return virt >= WARP_LINMEM_VA_BASE &&
           virt < WARP_LINMEM_VA_BASE +
                      (uint64_t)(WARP_LINMEM_PDPT_COUNT / 2u) * WARP_LINMEM_VA_STRIDE;
}
#endif

/* Re-map `pages` 4 KiB pages of a direct-mapped kernel alias onto their own frames as
 * RWX, splitting whatever large-page mapping covered them.  `ptr` must be a kernel
 * higher-half address whose frame is virt - kHalfBase; anything below kHalfBase, a null
 * pointer or a zero count is rejected with -1, as is a mapping failure.  Returns 0 on
 * success.  Idempotent: re-mapping an already-4 KiB-mapped range is harmless. */
static int remap_direct_alias_pages(uint8_t* ptr, uint64_t pages) {
    if (!ptr || pages == 0) {
        return -1;
    }
    uint64_t virt = reinterpret_cast<uint64_t>(ptr);
    if (virt < kHalfBase) {
        return -1;
    }
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t page_virt = virt + (i * kPageSize);
        uint64_t page_phys = page_virt - kHalfBase;
        if (paging_map_4k(page_virt,
                          page_phys,
                          MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_EXEC) !=
            0) {
            return -1;
        }
    }
    return 0;
}

/* Tracking table for mmap → phys mapping so munmap can free pages.
 * The type field distinguishes linmem from JIT allocations for ring-3 mapping.
 * data_offset: bytes from phys to the first usable data byte (0 for mmap entries,
 * sizeof(AllocHeader) for warp_kmalloc large entries). */
/* Which physical zone an allocation came from, and therefore how ring-3 mapping should
 * treat it.  JIT allocations are taken from WARP_JIT_PHYS_MIN upward and linmem from
 * WASMOS_BUFFER_PHYS_LIMIT upward precisely so a linmem commit's zero-fill can never
 * land on JIT code. */
enum MmapType : uint8_t { MMAP_OTHER = 0, MMAP_JIT = 1, MMAP_LINMEM = 2 };
struct MmapEntry {
    uint8_t* virt;
    uint64_t phys;
    uint64_t pages;
    MmapType type;
    uint64_t data_offset;
};
/* Fixed 256-entry tracking table; an allocation that finds no free slot is not tracked
 * and its physical range then cannot be resolved by find_entry_by_phys.
 *
 * g_mmap_lock guards every read and write of the table: several CPUs can be inside
 * WARP at once (see warp_runtime_enter in warp/shim.cpp).  A slot is claimed and
 * filled in one critical section, and readers copy the fields they need out of an
 * entry before releasing the lock -- an entry pointer is not valid past the unlock.
 * The helpers below require the lock held.  It is the noirq flavour: interrupts and
 * preemption stay enabled while it is held, because a legacy linear-memory
 * publication holds it across the whole user page-table walk so the block cannot be
 * freed while its frames are being mapped.  Every taker runs in thread context and
 * holds no interrupt-masking lock.  No path holds it across a blocking wait. */
static MmapEntry g_mmap_table[256];
static ksync_spinlock_t g_mmap_lock;

static MmapEntry* alloc_entry() {
    for (auto& e : g_mmap_table)
        if (!e.virt)
            return &e;
    return nullptr;
}

static MmapEntry* find_entry(uint8_t* v) {
    for (auto& e : g_mmap_table)
        if (e.virt == v)
            return &e;
    return nullptr;
}

static void clear_entry(MmapEntry* e) {
    if (!e) {
        return;
    }
    e->virt = nullptr;
    e->phys = 0;
    e->pages = 0;
    e->type = MMAP_OTHER;
    e->data_offset = 0;
}

/* Find the entry that contains the given physical address.  Requires g_mmap_lock. */
#ifdef WASMOS_WASM_RUNTIME_WARP
static MmapEntry* find_entry_by_phys(uint64_t phys) {
    for (auto& e : g_mmap_table)
        if (e.virt && e.phys <= phys && phys < e.phys + e.pages * kPageSize)
            return &e;
    return nullptr;
}

/* Last linear-memory publication for one user root, so a re-publication can tell an
 * incremental extension (same base, same phys identity, more pages — map only the tail)
 * from a relocation (unmap the old range first).  32 slots, and when they are all taken
 * by other roots the lookup falls back to slot 0 and that root's state is overwritten,
 * which costs a redundant full remap rather than correctness.
 *
 * g_ring3_linmem_lock guards only the choice of a slot for a root, and a free slot is
 * marked with the root inside that section, so two roots cannot claim one free slot.
 * Each slot's own `lock` (noirq flavour, like g_mmap_lock) is held from the state
 * comparison through the page-table writes to the state update, so two publications
 * for the same root cannot interleave their unmap and map walks while publications
 * for different roots proceed in parallel.  A slot lock nests outside g_mmap_lock. */
struct Ring3LinmemMapState {
    ksync_spinlock_t lock;
    uint64_t user_root;
    uint64_t user_va_base;
    uint64_t phys_base;
    uint64_t pages;
    uint64_t basedata_length;
    uint64_t data_offset;
};

static Ring3LinmemMapState g_ring3_linmem_maps[32];
static ksync_spinlock_t g_ring3_linmem_lock;

static Ring3LinmemMapState* claim_ring3_linmem_map(uint64_t user_root) {
    Ring3LinmemMapState* found = nullptr;
    Ring3LinmemMapState* free_slot = nullptr;
    ksync_spinlock_lock(&g_ring3_linmem_lock);
    for (auto& slot : g_ring3_linmem_maps) {
        uint64_t root = __atomic_load_n(&slot.user_root, __ATOMIC_RELAXED);
        if (root == user_root) {
            found = &slot;
            break;
        }
        if (root == 0 && free_slot == nullptr) {
            free_slot = &slot;
        }
    }
    if (!found && free_slot) {
        __atomic_store_n(&free_slot->user_root, user_root, __ATOMIC_RELAXED);
        free_slot->pages = 0;
        found = free_slot;
    }
    ksync_spinlock_unlock(&g_ring3_linmem_lock);
    return found != nullptr ? found : &g_ring3_linmem_maps[0];
}
#endif /* WASMOS_WASM_RUNTIME_WARP */

} // namespace

/* Physical address backing a warp allocation's kernel-alias pointer.  All
 * phys-from-virt derivations for linmem/JIT blocks route through here so the
 * dedicated-VA linmem block (scattered physical pages) resolves without a
 * per-page list.  A VA in the dedicated linmem window is resolved by walking
 * the page tables; a miss there means a tracking gap / uncommitted linmem VA and
 * returns 0 (loud failure at the caller) rather than silently corrupting with a
 * virt-kHalfBase phys.  Direct-mapped pointers (JIT, ad-hoc) fall back to
 * `virt - kHalfBase` (identical to a tracked contiguous entry). */
extern "C" uint64_t warp_mem_alias_phys(uint64_t virt) {
    if (virt >= WARP_LINMEM_VA_BASE &&
        virt <
            WARP_LINMEM_VA_BASE + (uint64_t)(WARP_LINMEM_PDPT_COUNT / 2u) * WARP_LINMEM_VA_STRIDE) {
        uint64_t phys = paging_virt_to_phys(virt);
        if (!phys) {
            klog_write("[warp-mem] linmem VA unmapped in alias_phys (tracking gap)\n");
        }
        return phys;
    }
    /* Every tracked allocation's alias is phys | kHalfBase with phys below the
     * direct map's limit, which equals phys + kHalfBase, so the tracking table
     * would only ever confirm this subtraction. */
    return virt - kHalfBase;
}

#ifdef WASMOS_WASM_RUNTIME_WARP
/* Return basedataLength = byte offset from memoryBase (warp_kmalloc result or
 * allocPagedMemory result) to the first linmem byte.
 * data_offset accounts for the AllocHeader prepended by warp_kmalloc. */
extern "C" uint64_t warp_mem_linmem_basedata_length(uint8_t const* linmem_kernel_ptr) {
    if (!linmem_kernel_ptr)
        return 0;
    uint64_t linmem_virt = reinterpret_cast<uint64_t>(linmem_kernel_ptr);
    if (is_ring3_linmem_kernel_window(linmem_virt)) {
        uint64_t slot_va_base = 0;
        uint64_t basedata_length = 0;
        uint64_t committed_pages = 0;
        if (warp_linmem_kernel_window_query(
                linmem_kernel_ptr, &slot_va_base, &basedata_length, &committed_pages) != 0) {
            return 0;
        }
        return basedata_length;
    }
    if (linmem_virt < kHalfBase && !is_ring3_linmem_kernel_window(linmem_virt)) {
        return 0;
    }
    uint64_t linmem_phys = warp_mem_alias_phys(linmem_virt);
    uint64_t basedata_length = 0;
    ksync_spinlock_lock_noirq(&g_mmap_lock);
    MmapEntry* e = find_entry_by_phys(linmem_phys);
    if (e) {
        basedata_length = linmem_phys - e->phys - e->data_offset;
    }
    ksync_spinlock_unlock_noirq(&g_mmap_lock);
    return basedata_length;
}

/* Register a large warp_kmalloc allocation so ring-3 mapping can find its phys range.
 * phys: page-aligned physical base of the allocation (AllocHeader sits here).
 * data_offset: sizeof(AllocHeader) — bytes from phys to first usable byte. */
extern "C" void warp_mem_kmalloc_register(uint64_t phys, uint64_t pages, uint64_t data_offset) {
    ksync_spinlock_lock_noirq(&g_mmap_lock);
    auto* slot = alloc_entry();
    if (slot) {
        slot->virt = reinterpret_cast<uint8_t*>(phys | kHalfBase);
        slot->phys = phys;
        slot->pages = pages;
        slot->type = MMAP_OTHER;
        slot->data_offset = data_offset;
    }
    ksync_spinlock_unlock_noirq(&g_mmap_lock);
    if (!slot) {
        klog_write("[warp-mem] g_mmap_table full, kmalloc not tracked\n");
    }
}

/* Remove the tracking entry for a warp_kmalloc large allocation. */
extern "C" void warp_mem_kmalloc_unregister(uint64_t phys) {
    ksync_spinlock_lock_noirq(&g_mmap_lock);
    for (auto& e : g_mmap_table) {
        if (e.virt && e.phys == phys) {
            clear_entry(&e);
            break;
        }
    }
    ksync_spinlock_unlock_noirq(&g_mmap_lock);
}

/* Map JIT binary pages into the ring-3 user CR3 at WARP_R3_JIT_BASE.
 * jit_kernel_ptr = getCompiledBinary().data() (kernel alias of JIT code). */
/* Returns 0 when every page was mapped, -1 on a null/empty range, a `jit_kernel_ptr`
 * that is not a kernel higher-half address, or a mapping failure — in which case the
 * pages mapped so far are left in place. */
extern "C" int warp_mem_ring3_map_jit(uint64_t user_root, uint8_t const* jit_kernel_ptr,
                                      size_t jit_size) {
    if (!jit_kernel_ptr || jit_size == 0)
        return -1;
    uint64_t jit_virt = reinterpret_cast<uint64_t>(jit_kernel_ptr);
    if (jit_virt < kHalfBase)
        return -1;
    uint64_t phys = warp_mem_alias_phys(jit_virt);
    uint64_t pages = (static_cast<uint64_t>(jit_size) + kPageSize - 1) / kPageSize;
    uint64_t flags = MEM_REGION_FLAG_READ | MEM_REGION_FLAG_EXEC | MEM_REGION_FLAG_USER;
    for (uint64_t i = 0; i < pages; ++i) {
        if (paging_map_4k_in_root(
                user_root, WARP_R3_JIT_BASE + i * kPageSize, phys + i * kPageSize, flags) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Map the full basedata+linmem allocation into the ring-3 user CR3.
 * linmem_kernel_ptr = getLinearMemoryRegion(0, 0) (= linmem base kernel alias).
 * Finds the containing MmapEntry (type=LINMEM) to get the alloc start and
 * page count, then maps from WARP_R3_LINMEM_BASE - basedataLength upwards. */
/* Handles both linmem layouts: a dedicated-VA slot, whose frames are scattered and are
 * resolved per page by walking the page tables, and a legacy contiguous allocation
 * resolved through the tracking table.  The contiguous layout is a normal state, not a
 * failure: a module whose linear memory first grows after start is published while its
 * block is still contiguous, and is republished from the slot once that growth moves
 * the block (warp_krealloc in warp/shim.cpp).  A re-publication for the same root extends the
 * existing mapping when base, physical identity and offsets are unchanged and only the
 * page count grew; otherwise the previous range is unmapped first.  Returns 0 on
 * success, -1 when the allocation cannot be located, a page has no backing frame, or a
 * mapping failed — partial mappings are not rolled back. */
/* Write one linear-memory layout into `user_root` and record it in `state`.  Requires
 * state->lock; for a contiguous block the caller also holds g_mmap_lock, which keeps
 * the block's frames allocated for the whole walk.  `dedicated_slot` selects how a
 * page's frame is found: by walking the kernel page tables from `phys_identity` (a
 * slot's VA base) or by offset from `phys_identity` (a contiguous block's frame). */
static int ring3_map_linmem_locked(Ring3LinmemMapState* state, uint64_t user_root,
                                   uint64_t user_va_base, uint64_t phys_identity,
                                   uint64_t total_pages, uint64_t basedataLength,
                                   uint64_t data_offset, uint8_t dedicated_slot) {
    const uint64_t flags = MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER;
    const uint8_t same_root = __atomic_load_n(&state->user_root, __ATOMIC_RELAXED) == user_root;
    uint64_t start_page = 0;
    uint8_t incremental = same_root && state->user_va_base == user_va_base &&
                          state->phys_base == phys_identity &&
                          state->basedata_length == basedataLength &&
                          state->data_offset == data_offset && state->pages <= total_pages;

    if (!incremental && same_root) {
        for (uint64_t i = 0; i < state->pages; ++i) {
            (void)paging_unmap_4k_in_root(user_root, state->user_va_base + i * kPageSize);
        }
    } else if (incremental) {
        start_page = state->pages;
    }

    for (uint64_t i = start_page; i < total_pages; ++i) {
        uint64_t phys_page = 0;
        if (!incremental) {
            (void)paging_unmap_4k_in_root(user_root, user_va_base + i * kPageSize);
        }
        if (dedicated_slot) {
            phys_page = paging_virt_to_phys(phys_identity + i * kPageSize) & ~(kPageSize - 1ULL);
        } else {
            phys_page = phys_identity + i * kPageSize;
        }
        if (!phys_page) {
            /* A committed page with no frame: the committed count and the page
             * tables disagree, which is the invariant this walk depends on. */
            klog_write("[warp] r3 linmem: committed page has no frame\n");
            return -1;
        }
        if (paging_map_4k_in_root(user_root, user_va_base + i * kPageSize, phys_page, flags) != 0) {
            /* Installing a PTE needs page-table frames from the allocator, so
             * this is the one failure here that is a SHORTAGE rather than a
             * broken invariant -- and the one that can come and go with load. */
            klog_write("[warp] r3 linmem: user PTE install failed\n");
            return -1;
        }
    }
    __atomic_store_n(&state->user_root, user_root, __ATOMIC_RELAXED);
    state->user_va_base = user_va_base;
    state->phys_base = phys_identity;
    state->pages = total_pages;
    state->basedata_length = basedataLength;
    state->data_offset = data_offset;
    return 0;
}

extern "C" int warp_mem_ring3_map_linmem(uint64_t user_root, uint8_t const* linmem_kernel_ptr) {
    if (!linmem_kernel_ptr) {
        klog_write("[warp] r3 linmem: null kernel base\n");
        return -1;
    }
    uint64_t linmem_virt = reinterpret_cast<uint64_t>(linmem_kernel_ptr);

    if (is_ring3_linmem_kernel_window(linmem_virt)) {
        uint64_t slot_va_base = 0;
        uint64_t basedataLength = 0;
        uint64_t total_pages = 0;
        if (warp_linmem_kernel_window_query(
                linmem_kernel_ptr, &slot_va_base, &basedataLength, &total_pages) != 0) {
            klog_write("[warp] r3 linmem: slot window query failed\n");
            return -1;
        }
        uint64_t data_offset = linmem_virt - basedataLength - slot_va_base;
        uint64_t user_linmem_base = WARP_R3_LINMEM_BASE + (linmem_virt & (kPageSize - 1ULL));
        uint64_t user_va_base = user_linmem_base - data_offset - basedataLength;
        Ring3LinmemMapState* state = claim_ring3_linmem_map(user_root);
        ksync_spinlock_lock_noirq(&state->lock);
        int rc = ring3_map_linmem_locked(state,
                                         user_root,
                                         user_va_base,
                                         slot_va_base,
                                         total_pages,
                                         basedataLength,
                                         data_offset,
                                         1);
        ksync_spinlock_unlock_noirq(&state->lock);
        return rc;
    }

    if (linmem_virt < kHalfBase) {
        klog_write("[warp] r3 linmem: base is not a kernel alias\n");
        return -1;
    }
    uint64_t linmem_phys = warp_mem_alias_phys(linmem_virt);
    if (!linmem_phys) {
        klog_write("[warp] r3 linmem: base has no physical frame\n");
        return -1;
    }

    /* The MmapEntry whose phys range contains linmem_phys is the backing allocation
     * (warp_kmalloc or allocPagedMemory); data_offset accounts for the AllocHeader
     * warp_kmalloc prepends (0 for mmap entries).  g_mmap_lock stays held until the
     * frames are mapped, so the block cannot be freed and its frames reused while
     * they are being made user-writable. */
    Ring3LinmemMapState* state = claim_ring3_linmem_map(user_root);
    ksync_spinlock_lock_noirq(&state->lock);
    ksync_spinlock_lock_noirq(&g_mmap_lock);
    MmapEntry* e = find_entry_by_phys(linmem_phys);
    int rc = -1;
    if (e) {
        uint64_t basedataLength = linmem_phys - e->phys - e->data_offset;
        uint64_t user_linmem_base = WARP_R3_LINMEM_BASE + (linmem_phys & (kPageSize - 1ULL));
        uint64_t user_va_base = user_linmem_base - e->data_offset - basedataLength;
        rc = ring3_map_linmem_locked(
            state, user_root, user_va_base, e->phys, e->pages, basedataLength, e->data_offset, 0);
    }
    ksync_spinlock_unlock_noirq(&g_mmap_lock);
    ksync_spinlock_unlock_noirq(&state->lock);
    if (!e) {
        klog_write("[warp] r3 linmem: no tracked allocation for the base\n");
    }
    return rc;
}
#endif /* WASMOS_WASM_RUNTIME_WARP */

/* Kernel implementation of the vb::MemUtils interface declared by libs/warp; the
 * contracts are upstream's, and the notes below record only where this implementation
 * deviates.
 *
 * setPermissionRWX / setPermissionRX / setPermissionRW are no-ops returning 0: every
 * page this allocator hands out is already RWX and stays RWX, so a caller cannot rely
 * on W^X here.  uncommitVirtualMemory is a no-op: nothing is ever decommitted, so
 * committed memory is only reclaimed when the whole allocation is freed.
 * clearInstructionCache is a no-op because x86_64 keeps the instruction cache coherent
 * with the data cache.  getStackInfo returns a zero-filled struct — the kernel hands
 * WARP its stack bounds directly instead.  allocAlignedMemory ignores the requested
 * alignment and returns page-aligned memory, which satisfies every alignment WARP asks
 * for; it and reallocAlignedMemory throw std::bad_alloc on exhaustion, which the
 * driver's exception checkpoint turns into a longjmp rather than an unwind. */

/* -----------------------------------------------------------------------
 * vb::MemUtils implementation
 * ----------------------------------------------------------------------- */

namespace {

/* Allocate `size` bytes, rounded up to whole pages, from the physical zone `type`
 * selects, and track the allocation.  JIT allocations come from WARP_JIT_PHYS_MIN
 * upward and everything else from WASMOS_BUFFER_PHYS_LIMIT upward, so a linear-memory
 * commit's zero-fill cannot land on JIT code.  The zone is an argument because
 * vb::MemUtils' own signatures carry none.  The tracking slot is claimed and filled in
 * one g_mmap_lock section BEFORE the direct-map alias is split into 4 KiB pages, so a
 * full table fails without splitting large pages or spending page-table frames; a
 * failed split releases the slot again.  Returns the kernel alias, or nullptr when no frames, no
 * mapping or no tracking slot is available; nothing stays allocated on failure. */
uint8_t* alloc_paged_in_zone(size_t size, MmapType type) {
    uint64_t pages = round_up_page(static_cast<uint64_t>(size)) / kPageSize;
#ifdef WASMOS_WASM_RUNTIME_WARP
    uint64_t phys_min = (type == MMAP_JIT) ? WARP_JIT_PHYS_MIN : WASMOS_BUFFER_PHYS_LIMIT;
#else
    uint64_t phys_min = WASMOS_BUFFER_PHYS_LIMIT;
#endif
    uint64_t phys = pfa_alloc_pages_above(pages, phys_min);
    if (!phys)
        return nullptr;

    uint8_t* virt = reinterpret_cast<uint8_t*>(phys | kHalfBase);
    ksync_spinlock_lock_noirq(&g_mmap_lock);
    auto* slot = alloc_entry();
    if (slot) {
        slot->virt = virt;
        slot->phys = phys;
        slot->pages = pages;
        slot->type = type;
        slot->data_offset = 0;
    }
    ksync_spinlock_unlock_noirq(&g_mmap_lock);
    if (!slot) {
        pfa_free_pages(phys, pages);
        return nullptr;
    }

    if (remap_direct_alias_pages(virt, pages) != 0) {
        klog_write("[warp-mem] remap_direct_alias_pages failed\n");
        ksync_spinlock_lock_noirq(&g_mmap_lock);
        clear_entry(slot);
        ksync_spinlock_unlock_noirq(&g_mmap_lock);
        pfa_free_pages(phys, pages);
        return nullptr;
    }
    return virt;
}

/* alloc_paged_in_zone for callers whose contract is to throw on exhaustion. */
uint8_t* alloc_paged_in_zone_or_throw(size_t size, MmapType type) {
    uint8_t* p = alloc_paged_in_zone(size, type);
    if (!p)
        throw std::bad_alloc();
    return p;
}

} // namespace

namespace vb {
namespace MemUtils {

size_t getOSMemoryPageSize() noexcept {
    return kPageSize;
}

size_t roundUpToOSMemoryPageSize(size_t n) noexcept {
    return static_cast<size_t>(round_up_page(static_cast<uint64_t>(n)));
}

size_t roundDownToOSMemoryPageSize(size_t n) noexcept {
    return n & ~(kPageSize - 1ULL);
}

MmapMemory allocPagedMemory(size_t size) {
    return MmapMemory{alloc_paged_in_zone(size, MMAP_OTHER), -1};
}

void freePagedMemory(uint8_t* ptr, size_t) noexcept {
    if (!ptr)
        return;
    uint64_t phys = 0;
    uint64_t pages = 0;
    ksync_spinlock_lock_noirq(&g_mmap_lock);
    auto* e = find_entry(ptr);
    if (e) {
        phys = e->phys;
        pages = e->pages;
        clear_entry(e);
    }
    ksync_spinlock_unlock_noirq(&g_mmap_lock);
    if (pages != 0)
        pfa_free_pages(phys, pages);
}

int32_t setPermissionRWX(uint8_t*, size_t) noexcept {
    return 0;
}
int32_t setPermissionRX(uint8_t*, size_t) noexcept {
    return 0;
}
int32_t setPermissionRW(uint8_t*, size_t) noexcept {
    return 0;
}

void memcpyAndClearInstrCache(uint8_t* dest, uint8_t const* src, size_t n) noexcept {
    __builtin_memcpy(dest, src, n);
    paging_sync_instruction_cache(dest, n);
}

void clearInstructionCache(uint8_t* addr, size_t n) noexcept {
    paging_sync_instruction_cache(addr, n);
}

uint8_t* allocAlignedMemory(size_t size, size_t) {
    return alloc_paged_in_zone_or_throw(size, MMAP_OTHER);
}

uint8_t* reallocAlignedMemory(uint8_t* old, size_t oldSz, size_t newSz, size_t alignment) {
    newSz = roundUpToOSMemoryPageSize(newSz);
    if (oldSz == newSz)
        return old;
    uint8_t* n = allocAlignedMemory(newSz, alignment);
    if (old) {
        size_t copy = oldSz < newSz ? oldSz : newSz;
        __builtin_memcpy(n, old, copy);
        freePagedMemory(old, oldSz);
    }
    return n;
}

void freeAlignedMemory(void* ptr) noexcept {
    if (ptr)
        freePagedMemory(static_cast<uint8_t*>(ptr), 0);
}

void* allocVirtualMemory(size_t size) {
    /* Linear memory + basedata, from the linmem zone. */
    return alloc_paged_in_zone_or_throw(size, MMAP_LINMEM);
}

void freeVirtualMemory(void* ptr, size_t size) noexcept {
    freePagedMemory(static_cast<uint8_t*>(ptr), size);
}

void commitVirtualMemory(void* ptr, size_t size) {
    if (!ptr || size == 0) {
        return;
    }
    uint64_t pages = round_up_page(static_cast<uint64_t>(size)) / kPageSize;
    (void)remap_direct_alias_pages(static_cast<uint8_t*>(ptr), pages);
}

void uncommitVirtualMemory(void* ptr, size_t size) {
    /* No-op: pages are never decommitted. */
    (void)ptr;
    (void)size;
}

uint8_t* mapRXMemory(size_t size, int32_t) {
    /* JIT working memory, from the JIT zone. */
    return alloc_paged_in_zone_or_throw(size, MMAP_JIT);
}

StackInfo getStackInfo() {
    /* Not used in the kernel — WARP's start() receives the stack top
     * directly.  Return a zero-filled struct. */
    return StackInfo{};
}

} // namespace MemUtils

/* -----------------------------------------------------------------------
 * vb::ExecutableMemory implementation
 * ----------------------------------------------------------------------- */

ExecutableMemory::ExecutableMemory(uint8_t const* data, size_t size)
    : ExecutableMemory(nullptr, size, -1) {
    if (size == 0)
        return;
    /* The compiled binary is JIT code; allocate from the JIT physical zone. */
    data_ = alloc_paged_in_zone_or_throw(size, MMAP_JIT);
    size_ = size;
    MemUtils::memcpyAndClearInstrCache(data_, data, size);
}

ExecutableMemory::ExecutableMemory(ExecutableMemory&& o) noexcept
    : data_(o.data_), size_(o.size_), fd_(o.fd_) {
    o.data_ = nullptr;
    o.size_ = 0;
    o.fd_ = -1;
}

ExecutableMemory& ExecutableMemory::operator=(ExecutableMemory&& o) & noexcept {
    swap(*this, std::move(o));
    return *this;
}

ExecutableMemory::~ExecutableMemory() noexcept {
    if (data_)
        freeExecutableMemory();
}

void ExecutableMemory::init(uint8_t const* data) {
    if (size_ == 0)
        return;
    data_ = alloc_paged_in_zone_or_throw(size_, MMAP_JIT);
    fd_ = -1;
    MemUtils::memcpyAndClearInstrCache(data_, data, size_);
}

void ExecutableMemory::freeExecutableMemory() const noexcept {
    MemUtils::freePagedMemory(data_, size_);
}

} // namespace vb
