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
 * WASMOS_SHMEM_PHYS_LIMIT upward precisely so a linmem commit's zero-fill can never
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
 * and its physical range then cannot be resolved by find_entry_by_phys.  Unsynchronised
 * — safe only under the WARP single-CPU invariant (warp/shim.cpp). */
static MmapEntry g_mmap_table[256];

/* Next-allocation type hint: set before calling allocPagedMemory. */
/* Out-of-band argument to the next allocPagedMemory call, because vb::MemUtils' own
 * signature carries no zone.  Every setter restores it to MMAP_OTHER immediately after
 * the call; a path that forgets would mis-zone the next unrelated allocation. */
static MmapType g_next_alloc_type = MMAP_OTHER;

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

/* Find the entry that contains the given physical address. */
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
 * which costs a redundant full remap rather than correctness. */
struct Ring3LinmemMapState {
    uint64_t user_root;
    uint64_t user_va_base;
    uint64_t phys_base;
    uint64_t pages;
    uint64_t basedata_length;
    uint64_t data_offset;
};

static Ring3LinmemMapState g_ring3_linmem_maps[32];

static Ring3LinmemMapState* find_ring3_linmem_map(uint64_t user_root) {
    Ring3LinmemMapState* free_slot = nullptr;
    for (auto& slot : g_ring3_linmem_maps) {
        if (slot.user_root == user_root) {
            return &slot;
        }
        if (slot.user_root == 0 && free_slot == nullptr) {
            free_slot = &slot;
        }
    }
    return free_slot != nullptr ? free_slot : &g_ring3_linmem_maps[0];
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
    for (auto& e : g_mmap_table) {
        uint64_t base = reinterpret_cast<uint64_t>(e.virt);
        if (e.virt && virt >= base && virt < base + e.pages * kPageSize) {
            return e.phys + (virt - base);
        }
    }
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
    MmapEntry* e = find_entry_by_phys(linmem_phys);
    return e ? (linmem_phys - e->phys - e->data_offset) : 0ULL;
}

/* Register a large warp_kmalloc allocation so ring-3 mapping can find its phys range.
 * phys: page-aligned physical base of the allocation (AllocHeader sits here).
 * data_offset: sizeof(AllocHeader) — bytes from phys to first usable byte. */
extern "C" void warp_mem_kmalloc_register(uint64_t phys, uint64_t pages, uint64_t data_offset) {
    auto* slot = alloc_entry();
    if (!slot) {
        klog_write("[warp-mem] g_mmap_table full, kmalloc not tracked\n");
        return;
    }
    slot->virt = reinterpret_cast<uint8_t*>(phys | kHalfBase);
    slot->phys = phys;
    slot->pages = pages;
    slot->type = MMAP_OTHER;
    slot->data_offset = data_offset;
}

/* Remove the tracking entry for a warp_kmalloc large allocation. */
extern "C" void warp_mem_kmalloc_unregister(uint64_t phys) {
    for (auto& e : g_mmap_table) {
        if (e.virt && e.phys == phys) {
            clear_entry(&e);
            return;
        }
    }
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
 * resolved through the tracking table.  A re-publication for the same root extends the
 * existing mapping when base, physical identity and offsets are unchanged and only the
 * page count grew; otherwise the previous range is unmapped first.  Returns 0 on
 * success, -1 when the allocation cannot be located, a page has no backing frame, or a
 * mapping failed — partial mappings are not rolled back. */
extern "C" int warp_mem_ring3_map_linmem(uint64_t user_root, uint8_t const* linmem_kernel_ptr) {
    if (!linmem_kernel_ptr)
        return -1;
    uint64_t linmem_virt = reinterpret_cast<uint64_t>(linmem_kernel_ptr);
    uint64_t basedataLength = 0;
    uint64_t user_va_base = 0;
    uint64_t phys_identity = 0;
    uint64_t total_pages = 0;
    uint8_t dedicated_slot = 0;
    uint64_t flags = MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER;
    uint64_t data_offset = 0;

    if (is_ring3_linmem_kernel_window(linmem_virt)) {
        uint64_t slot_va_base = 0;
        if (warp_linmem_kernel_window_query(
                linmem_kernel_ptr, &slot_va_base, &basedataLength, &total_pages) != 0) {
            return -1;
        }
        data_offset = linmem_virt - basedataLength - slot_va_base;
        uint64_t user_linmem_base = WARP_R3_LINMEM_BASE + (linmem_virt & (kPageSize - 1ULL));
        user_va_base = user_linmem_base - data_offset - basedataLength;
        phys_identity = slot_va_base;
        dedicated_slot = 1;
    } else {
        if (linmem_virt < kHalfBase) {
            return -1;
        }
        uint64_t linmem_phys = warp_mem_alias_phys(linmem_virt);
        if (!linmem_phys) {
            return -1;
        }

        /* The MmapEntry whose phys range contains linmem_phys is the backing
         * allocation (warp_kmalloc or allocPagedMemory).  data_offset accounts
         * for the AllocHeader prepended by warp_kmalloc (0 for mmap entries). */
        MmapEntry* e = find_entry_by_phys(linmem_phys);
        if (!e) {
            return -1;
        }

        basedataLength = linmem_phys - e->phys - e->data_offset;
        uint64_t user_linmem_base = WARP_R3_LINMEM_BASE + (linmem_phys & (kPageSize - 1ULL));
        user_va_base = user_linmem_base - e->data_offset - basedataLength;
        phys_identity = e->phys;
        total_pages = e->pages;
        data_offset = e->data_offset;
    }

    Ring3LinmemMapState* state = find_ring3_linmem_map(user_root);
    uint64_t start_page = 0;
    uint8_t incremental =
        state != nullptr && state->user_root == user_root && state->user_va_base == user_va_base &&
        state->phys_base == phys_identity && state->basedata_length == basedataLength &&
        state->data_offset == data_offset && state->pages <= total_pages;

    if (!incremental && state != nullptr && state->user_root == user_root) {
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
            return -1;
        }
        if (paging_map_4k_in_root(user_root, user_va_base + i * kPageSize, phys_page, flags) != 0) {
            return -1;
        }
    }
    if (state != nullptr) {
        state->user_root = user_root;
        state->user_va_base = user_va_base;
        state->phys_base = phys_identity;
        state->pages = total_pages;
        state->basedata_length = basedataLength;
        state->data_offset = data_offset;
    }
    return 0;
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
    MmapMemory m{nullptr, -1};
    uint64_t aligned = round_up_page(static_cast<uint64_t>(size));
    uint64_t pages = aligned / kPageSize;

    /* JIT allocations use a higher physical zone so that linmem (which starts
     * from WASMOS_SHMEM_PHYS_LIMIT) cannot overlap with JIT pages.  This
     * prevents commitVirtualMemory's zero-fill from clobbering JIT code. */
    MmapType typ = g_next_alloc_type;
#ifdef WASMOS_WASM_RUNTIME_WARP
    uint64_t phys_min = (typ == MMAP_JIT) ? WARP_JIT_PHYS_MIN : WASMOS_SHMEM_PHYS_LIMIT;
#else
    uint64_t phys_min = WASMOS_SHMEM_PHYS_LIMIT;
    (void)typ;
#endif
    uint64_t phys = pfa_alloc_pages_above(pages, phys_min);
    if (!phys)
        return m;

    auto* slot = alloc_entry();
    if (!slot) {
        pfa_free_pages(phys, pages);
        return m;
    }

    uint8_t* virt = reinterpret_cast<uint8_t*>(phys | kHalfBase);
    if (remap_direct_alias_pages(virt, pages) != 0) {
        klog_write("[warp-mem] remap_direct_alias_pages failed\n");
        pfa_free_pages(phys, pages);
        clear_entry(slot);
        return m;
    }
    slot->virt = virt;
    slot->phys = phys;
    slot->pages = pages;
    slot->type = typ;

    m.ptr = virt;
    m.fd = -1;
    return m;
}

void freePagedMemory(uint8_t* ptr, size_t) noexcept {
    if (!ptr)
        return;
    auto* e = find_entry(ptr);
    if (!e)
        return;
    pfa_free_pages(e->phys, e->pages);
    clear_entry(e);
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
    /* x86_64: instruction cache is coherent with data cache on all Intel/AMD
     * CPUs; no explicit flush needed for self-modifying code from the same
     * core.  A serialising instruction ensures visibility. */
    __asm__ volatile("" ::: "memory");
}

void clearInstructionCache(uint8_t*, size_t) noexcept {
    /* x86_64: cache-coherent, no action needed. */
}

uint8_t* allocAlignedMemory(size_t size, size_t) {
    size = roundUpToOSMemoryPageSize(size);
    MmapMemory m = allocPagedMemory(size);
    if (!m.ptr)
        throw std::bad_alloc();
    return m.ptr;
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
    /* Linear memory + basedata: allocate from the linmem zone (WASMOS_SHMEM_PHYS_LIMIT+)
     * which is separate from the JIT zone (WARP_JIT_PHYS_MIN+). */
    g_next_alloc_type = MMAP_LINMEM;
    void* p = allocAlignedMemory(size, kPageSize);
    g_next_alloc_type = MMAP_OTHER;
    return p;
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
    /* JIT working memory: allocate from the JIT zone (>= WARP_JIT_PHYS_MIN). */
    g_next_alloc_type = MMAP_JIT;
    uint8_t* p = allocAlignedMemory(size, kPageSize);
    g_next_alloc_type = MMAP_OTHER;
    return p;
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
    g_next_alloc_type = MMAP_JIT;
    MemUtils::MmapMemory m = MemUtils::allocPagedMemory(size);
    g_next_alloc_type = MMAP_OTHER;
    if (!m.ptr)
        throw std::bad_alloc();
    data_ = m.ptr;
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
    g_next_alloc_type = MMAP_JIT;
    MemUtils::MmapMemory m = MemUtils::allocPagedMemory(size_);
    g_next_alloc_type = MMAP_OTHER;
    if (!m.ptr)
        throw std::bad_alloc();
    fd_ = m.fd;
    data_ = m.ptr;
    MemUtils::memcpyAndClearInstrCache(data_, data, size_);
}

void ExecutableMemory::freeExecutableMemory() const noexcept {
    MemUtils::freePagedMemory(data_, size_);
}

} // namespace vb
