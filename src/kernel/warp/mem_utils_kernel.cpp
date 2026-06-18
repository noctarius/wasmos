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
#ifdef WASMOS_WARP_RING3
#include "warp_ring3.h"
#endif
}

#include "src/utils/MemUtils.hpp"
#include "src/utils/ExecutableMemory.hpp"

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

namespace {

constexpr uint64_t kPageSize   = 4096ULL;
constexpr uint64_t kHalfBase   = 0xFFFFFFFF80000000ULL;
/* Stay within the 512 MB higher-half window the kernel already maps. */
constexpr uint64_t kPhysLimit  = 512ULL * 1024ULL * 1024ULL;

inline uint64_t round_up_page(uint64_t n)  { return (n + kPageSize - 1) & ~(kPageSize - 1); }

static int remap_direct_alias_pages(uint8_t *ptr, uint64_t pages)
{
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
                          MEM_REGION_FLAG_READ |
                              MEM_REGION_FLAG_WRITE |
                              MEM_REGION_FLAG_EXEC) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Tracking table for mmap → phys mapping so munmap can free pages.
 * The type field distinguishes linmem from JIT allocations for ring-3 mapping.
 * data_offset: bytes from phys to the first usable data byte (0 for mmap entries,
 * sizeof(AllocHeader) for warp_kmalloc large entries). */
enum MmapType : uint8_t { MMAP_OTHER = 0, MMAP_JIT = 1, MMAP_LINMEM = 2 };
struct MmapEntry { uint8_t *virt; uint64_t phys; uint64_t pages; MmapType type; uint64_t data_offset; };
static MmapEntry g_mmap_table[256];

/* Next-allocation type hint: set before calling allocPagedMemory. */
static MmapType g_next_alloc_type = MMAP_OTHER;

static MmapEntry *alloc_entry()
{
    for (auto &e : g_mmap_table) if (!e.virt) return &e;
    return nullptr;
}

static MmapEntry *find_entry(uint8_t *v)
{
    for (auto &e : g_mmap_table) if (e.virt == v) return &e;
    return nullptr;
}

/* Find the entry that contains the given physical address. */
#ifdef WASMOS_WARP_RING3
static MmapEntry *find_entry_by_phys(uint64_t phys)
{
    for (auto &e : g_mmap_table)
        if (e.virt && e.phys <= phys && phys < e.phys + e.pages * kPageSize)
            return &e;
    return nullptr;
}
#endif /* WASMOS_WARP_RING3 */

} // namespace

#ifdef WASMOS_WARP_RING3
/* Return basedataLength = byte offset from memoryBase (warp_kmalloc result or
 * allocPagedMemory result) to the first linmem byte.
 * data_offset accounts for the AllocHeader prepended by warp_kmalloc. */
extern "C" uint64_t
warp_mem_linmem_basedata_length(uint8_t const *linmem_kernel_ptr)
{
    if (!linmem_kernel_ptr) return 0;
    uint64_t linmem_virt = reinterpret_cast<uint64_t>(linmem_kernel_ptr);
    if (linmem_virt < kHalfBase) return 0;
    uint64_t linmem_phys = linmem_virt - kHalfBase;
    MmapEntry *e = find_entry_by_phys(linmem_phys);
    return e ? (linmem_phys - e->phys - e->data_offset) : 0ULL;
}

/* Register a large warp_kmalloc allocation so ring-3 mapping can find its phys range.
 * phys: page-aligned physical base of the allocation (AllocHeader sits here).
 * data_offset: sizeof(AllocHeader) — bytes from phys to first usable byte. */
extern "C" void
warp_mem_kmalloc_register(uint64_t phys, uint64_t pages, uint64_t data_offset)
{
    auto *slot = alloc_entry();
    if (!slot) {
        klog_write("[warp-mem] g_mmap_table full, kmalloc not tracked\n");
        return;
    }
    slot->virt        = reinterpret_cast<uint8_t *>(phys | kHalfBase);
    slot->phys        = phys;
    slot->pages       = pages;
    slot->type        = MMAP_OTHER;
    slot->data_offset = data_offset;
}

/* Remove the tracking entry for a warp_kmalloc large allocation. */
extern "C" void
warp_mem_kmalloc_unregister(uint64_t phys)
{
    for (auto &e : g_mmap_table) {
        if (e.virt && e.phys == phys) {
            e = MmapEntry{};
            return;
        }
    }
}

/* Map JIT binary pages into the ring-3 user CR3 at WARP_R3_JIT_BASE.
 * jit_kernel_ptr = getCompiledBinary().data() (kernel alias of JIT code). */
extern "C" int
warp_mem_ring3_map_jit(uint64_t user_root,
                       uint8_t const *jit_kernel_ptr, size_t jit_size)
{
    if (!jit_kernel_ptr || jit_size == 0) return -1;
    uint64_t jit_virt = reinterpret_cast<uint64_t>(jit_kernel_ptr);
    if (jit_virt < kHalfBase) return -1;
    uint64_t phys  = jit_virt - kHalfBase;
    uint64_t pages = (static_cast<uint64_t>(jit_size) + kPageSize - 1) / kPageSize;
    uint64_t flags = MEM_REGION_FLAG_READ | MEM_REGION_FLAG_EXEC | MEM_REGION_FLAG_USER;
    for (uint64_t i = 0; i < pages; ++i) {
        if (paging_map_4k_in_root(user_root,
                                   WARP_R3_JIT_BASE + i * kPageSize,
                                   phys + i * kPageSize,
                                   flags) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Map the full basedata+linmem allocation into the ring-3 user CR3.
 * linmem_kernel_ptr = getLinearMemoryRegion(0, 0) (= linmem base kernel alias).
 * Finds the containing MmapEntry (type=LINMEM) to get the alloc start and
 * page count, then maps from WARP_R3_LINMEM_BASE - basedataLength upwards. */
extern "C" int
warp_mem_ring3_map_linmem(uint64_t user_root, uint8_t const *linmem_kernel_ptr)
{
    if (!linmem_kernel_ptr) return -1;
    uint64_t linmem_virt = reinterpret_cast<uint64_t>(linmem_kernel_ptr);
    if (linmem_virt < kHalfBase) return -1;
    uint64_t linmem_phys = linmem_virt - kHalfBase;

    /* The MmapEntry whose phys range contains linmem_phys is the backing
     * allocation (warp_kmalloc or allocPagedMemory).  data_offset accounts
     * for the AllocHeader prepended by warp_kmalloc (0 for mmap entries). */
    MmapEntry *e = find_entry_by_phys(linmem_phys);
    if (!e) return -1;

    /* actual_basedataLength = bytes from memoryBase to linmem (excludes AllocHeader). */
    uint64_t basedataLength = linmem_phys - e->phys - e->data_offset;
    /* Map from phys (page-aligned) so that:
     *   phys + data_offset + basedataLength → WARP_R3_LINMEM_BASE (user space). */
    uint64_t user_va_base   = WARP_R3_LINMEM_BASE - e->data_offset - basedataLength;
    uint64_t flags = MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER;

    for (uint64_t i = 0; i < e->pages; ++i) {
        if (paging_map_4k_in_root(user_root,
                                   user_va_base + i * kPageSize,
                                   e->phys + i * kPageSize,
                                   flags) != 0) {
            klog_write("[lm-map] paging_map_4k_in_root failed page="); serial_write_hex64(i); klog_write("\n");
            return -1;
        }
    }
    return 0;
}
#endif /* WASMOS_WARP_RING3 */

/* -----------------------------------------------------------------------
 * vb::MemUtils implementation
 * ----------------------------------------------------------------------- */

namespace vb {
namespace MemUtils {

size_t getOSMemoryPageSize() noexcept { return kPageSize; }

size_t roundUpToOSMemoryPageSize(size_t n) noexcept
{
    return static_cast<size_t>(round_up_page(static_cast<uint64_t>(n)));
}

size_t roundDownToOSMemoryPageSize(size_t n) noexcept
{
    return n & ~(kPageSize - 1ULL);
}

MmapMemory allocPagedMemory(size_t size)
{
    MmapMemory m{nullptr, -1};
    uint64_t aligned = round_up_page(static_cast<uint64_t>(size));
    uint64_t pages   = aligned / kPageSize;

    /* JIT allocations use a higher physical zone so that linmem (which starts
     * from WASMOS_SHMEM_PHYS_LIMIT) cannot overlap with JIT pages.  This
     * prevents commitVirtualMemory's zero-fill from clobbering JIT code. */
    MmapType typ = g_next_alloc_type;
#ifdef WASMOS_WARP_RING3
    uint64_t phys_min = (typ == MMAP_JIT)
                        ? WARP_JIT_PHYS_MIN
                        : WASMOS_SHMEM_PHYS_LIMIT;
#else
    uint64_t phys_min = WASMOS_SHMEM_PHYS_LIMIT;
    (void)typ;
#endif
    uint64_t phys = pfa_alloc_pages_above(pages, phys_min);
    if (!phys) return m;

    auto *slot = alloc_entry();
    if (!slot) { pfa_free_pages(phys, pages); return m; }

    uint8_t *virt = reinterpret_cast<uint8_t *>(phys | kHalfBase);
    if (remap_direct_alias_pages(virt, pages) != 0) {
        klog_write("[warp-mem] remap_direct_alias_pages failed\n");
        pfa_free_pages(phys, pages);
        *slot = MmapEntry{};
        return m;
    }
    slot->virt  = virt;
    slot->phys  = phys;
    slot->pages = pages;
    slot->type  = typ;

    m.ptr = virt;
    m.fd  = -1;
    return m;
}

void freePagedMemory(uint8_t *ptr, size_t) noexcept
{
    if (!ptr) return;
    auto *e = find_entry(ptr);
    if (!e) return;
    pfa_free_pages(e->phys, e->pages);
    *e = MmapEntry{};
}

int32_t setPermissionRWX(uint8_t *, size_t) noexcept { return 0; }
int32_t setPermissionRX (uint8_t *, size_t) noexcept { return 0; }
int32_t setPermissionRW (uint8_t *, size_t) noexcept { return 0; }

void memcpyAndClearInstrCache(uint8_t *dest, uint8_t const *src, size_t n) noexcept
{
    __builtin_memcpy(dest, src, n);
    /* x86_64: instruction cache is coherent with data cache on all Intel/AMD
     * CPUs; no explicit flush needed for self-modifying code from the same
     * core.  A serialising instruction ensures visibility. */
    __asm__ volatile("" ::: "memory");
}

void clearInstructionCache(uint8_t *, size_t) noexcept
{
    /* x86_64: cache-coherent, no action needed. */
}

uint8_t *allocAlignedMemory(size_t size, size_t)
{
    size = roundUpToOSMemoryPageSize(size);
    MmapMemory m = allocPagedMemory(size);
    if (!m.ptr) throw std::bad_alloc();
    return m.ptr;
}

uint8_t *reallocAlignedMemory(uint8_t *old, size_t oldSz, size_t newSz, size_t alignment)
{
    newSz = roundUpToOSMemoryPageSize(newSz);
    if (oldSz == newSz) return old;
    uint8_t *n = allocAlignedMemory(newSz, alignment);
    if (old) {
        size_t copy = oldSz < newSz ? oldSz : newSz;
        __builtin_memcpy(n, old, copy);
        freePagedMemory(old, oldSz);
    }
    return n;
}

void freeAlignedMemory(void *ptr) noexcept
{
    if (ptr) freePagedMemory(static_cast<uint8_t *>(ptr), 0);
}

void *allocVirtualMemory(size_t size)
{
    /* Linear memory + basedata: allocate from the linmem zone (WASMOS_SHMEM_PHYS_LIMIT+)
     * which is separate from the JIT zone (WARP_JIT_PHYS_MIN+). */
    g_next_alloc_type = MMAP_LINMEM;
    void *p = allocAlignedMemory(size, kPageSize);
    g_next_alloc_type = MMAP_OTHER;
    return p;
}

void freeVirtualMemory(void *ptr, size_t size) noexcept
{
    freePagedMemory(static_cast<uint8_t *>(ptr), size);
}

void commitVirtualMemory(void *ptr, size_t size)
{
    if (!ptr || size == 0) {
        return;
    }
    uint64_t pages = round_up_page(static_cast<uint64_t>(size)) / kPageSize;
    (void)remap_direct_alias_pages(static_cast<uint8_t *>(ptr), pages);
}

void uncommitVirtualMemory(void *ptr, size_t size)
{
    /* No-op: we don't decommit pages. */
    (void)ptr; (void)size;
}

uint8_t *mapRXMemory(size_t size, int32_t)
{
    /* JIT working memory: allocate from the JIT zone (>= WARP_JIT_PHYS_MIN). */
    g_next_alloc_type = MMAP_JIT;
    uint8_t *p = allocAlignedMemory(size, kPageSize);
    g_next_alloc_type = MMAP_OTHER;
    return p;
}

StackInfo getStackInfo()
{
    /* Not used in the kernel — WARP's start() receives the stack top
     * directly.  Return a zero-filled struct. */
    return StackInfo{};
}

} // namespace MemUtils

/* -----------------------------------------------------------------------
 * vb::ExecutableMemory implementation
 * ----------------------------------------------------------------------- */

ExecutableMemory::ExecutableMemory(uint8_t const *data, size_t size)
    : ExecutableMemory(nullptr, size, -1)
{
    if (size == 0) return;
    /* The compiled binary is JIT code; allocate from the JIT physical zone. */
    g_next_alloc_type = MMAP_JIT;
    MemUtils::MmapMemory m = MemUtils::allocPagedMemory(size);
    g_next_alloc_type = MMAP_OTHER;
    if (!m.ptr) throw std::bad_alloc();
    data_ = m.ptr;
    size_ = size;
    MemUtils::memcpyAndClearInstrCache(data_, data, size);
}

ExecutableMemory::ExecutableMemory(ExecutableMemory &&o) noexcept
    : data_(o.data_), size_(o.size_), fd_(o.fd_)
{
    o.data_ = nullptr; o.size_ = 0; o.fd_ = -1;
}

ExecutableMemory &ExecutableMemory::operator=(ExecutableMemory &&o) & noexcept
{
    swap(*this, std::move(o)); return *this;
}

ExecutableMemory::~ExecutableMemory() noexcept
{
    if (data_) freeExecutableMemory();
}

void ExecutableMemory::init(uint8_t const *data)
{
    if (size_ == 0) return;
    g_next_alloc_type = MMAP_JIT;
    MemUtils::MmapMemory m = MemUtils::allocPagedMemory(size_);
    g_next_alloc_type = MMAP_OTHER;
    if (!m.ptr) throw std::bad_alloc();
    fd_   = m.fd;
    data_ = m.ptr;
    MemUtils::memcpyAndClearInstrCache(data_, data, size_);
}

void ExecutableMemory::freeExecutableMemory() const noexcept
{
    MemUtils::freePagedMemory(data_, size_);
}

} // namespace vb
