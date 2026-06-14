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

/* Simple tracking table for mmap → phys mapping so munmap can free pages. */
struct MmapEntry { uint8_t *virt; uint64_t phys; uint64_t pages; };
static MmapEntry g_mmap_table[64];

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

} // namespace

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

    uint64_t phys = pfa_alloc_pages_below(pages, kPhysLimit);
    if (!phys) return m;

    auto *slot = alloc_entry();
    if (!slot) { pfa_free_pages(phys, pages); return m; }

    uint8_t *virt = reinterpret_cast<uint8_t *>(phys | kHalfBase);
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t page_phys = phys + (i * kPageSize);
        uint64_t page_virt = reinterpret_cast<uint64_t>(virt) + (i * kPageSize);
        if (paging_map_4k(page_virt,
                          page_phys,
                          MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_EXEC) != 0) {
            pfa_free_pages(phys, pages);
            *slot = MmapEntry{};
            return m;
        }
    }
    slot->virt  = virt;
    slot->phys  = phys;
    slot->pages = pages;

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
    return allocAlignedMemory(size, kPageSize);
}

void freeVirtualMemory(void *ptr, size_t size) noexcept
{
    freePagedMemory(static_cast<uint8_t *>(ptr), size);
}

void commitVirtualMemory(void *ptr, size_t size)
{
    /* Pages are always committed in our model (no demand paging). */
    (void)ptr; (void)size;
}

void uncommitVirtualMemory(void *ptr, size_t size)
{
    /* No-op: we don't decommit pages. */
    (void)ptr; (void)size;
}

uint8_t *mapRXMemory(size_t size, int32_t) { return allocAlignedMemory(size, kPageSize); }

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
    MemUtils::MmapMemory m = MemUtils::allocPagedMemory(size);
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
    MemUtils::MmapMemory m = MemUtils::allocPagedMemory(size_);
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
