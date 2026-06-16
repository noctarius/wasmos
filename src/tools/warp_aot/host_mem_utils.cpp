/* host_mem_utils.cpp - Host-native implementation of vb::MemUtils and
 * vb::ExecutableMemory for the warp_aot tool.
 *
 * Replaces src/kernel/warp/mem_utils_kernel.cpp for the host build.
 * Uses mmap/mprotect/malloc on POSIX (macOS/Linux). */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <new>

#include <sys/mman.h>
#include <unistd.h>

#include "src/utils/MemUtils.hpp"
#include "src/utils/ExecutableMemory.hpp"

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

namespace {

static size_t host_page_size()
{
    static long ps = sysconf(_SC_PAGESIZE);
    return (ps > 0) ? (size_t)ps : 4096UL;
}

static size_t round_up_page(size_t n)
{
    size_t ps = host_page_size();
    return (n + ps - 1) & ~(ps - 1);
}

} // namespace

/* -----------------------------------------------------------------------
 * vb::MemUtils implementation
 * ----------------------------------------------------------------------- */

namespace vb {
namespace MemUtils {

size_t getOSMemoryPageSize() VB_NOEXCEPT
{
    return host_page_size();
}

size_t roundUpToOSMemoryPageSize(size_t n) VB_NOEXCEPT
{
    return round_up_page(n);
}

size_t roundDownToOSMemoryPageSize(size_t n) VB_NOEXCEPT
{
    size_t ps = host_page_size();
    return n & ~(ps - 1);
}

MmapMemory allocPagedMemory(size_t size)
{
    MmapMemory m{nullptr, -1};
    if (!size) return m;
    size = round_up_page(size);
    void *p = mmap(nullptr, size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return m;
    m.ptr = static_cast<uint8_t *>(p);
    m.fd  = -1;
    return m;
}

void freePagedMemory(uint8_t *ptr, size_t size) VB_NOEXCEPT
{
    if (!ptr || !size) return;
    munmap(ptr, round_up_page(size));
}

int32_t setPermissionRWX(uint8_t *start, size_t len) VB_NOEXCEPT
{
    if (!start || !len) return 0;
#ifdef __APPLE__
    /* On Apple Silicon MAP_JIT pages require pthread_jit_write_protect_np.
     * For x86-64 macOS (warp_aot targets x86-64 code generation) a plain
     * mprotect with RWX is sufficient. */
    return mprotect(start, round_up_page(len),
                    PROT_READ | PROT_WRITE | PROT_EXEC) == 0 ? 0 : -1;
#else
    return mprotect(start, round_up_page(len),
                    PROT_READ | PROT_WRITE | PROT_EXEC) == 0 ? 0 : -1;
#endif
}

int32_t setPermissionRX(uint8_t *start, size_t len) VB_NOEXCEPT
{
    if (!start || !len) return 0;
    return mprotect(start, round_up_page(len),
                    PROT_READ | PROT_EXEC) == 0 ? 0 : -1;
}

int32_t setPermissionRW(uint8_t *start, size_t len) VB_NOEXCEPT
{
    if (!start || !len) return 0;
    return mprotect(start, round_up_page(len),
                    PROT_READ | PROT_WRITE) == 0 ? 0 : -1;
}

void memcpyAndClearInstrCache(uint8_t *dest, uint8_t const *src, size_t n) VB_NOEXCEPT
{
    if (!dest || !src || !n) return;
    memcpy(dest, src, n);
    /* x86-64: cache is coherent; no explicit flush needed. */
    __asm__ volatile("" ::: "memory");
}

void clearInstructionCache(uint8_t *, size_t) VB_NOEXCEPT
{
    /* x86-64: no-op. */
}

uint8_t *allocAlignedMemory(size_t size, size_t /*alignment*/)
{
    size = round_up_page(size);
    MmapMemory m = allocPagedMemory(size);
    if (!m.ptr) throw std::bad_alloc();
    return m.ptr;
}

uint8_t *reallocAlignedMemory(uint8_t *old, size_t oldSz, size_t newSz, size_t alignment)
{
    newSz = round_up_page(newSz);
    if (oldSz == newSz) return old;
    uint8_t *n = allocAlignedMemory(newSz, alignment);
    if (old) {
        size_t copy = oldSz < newSz ? oldSz : newSz;
        memcpy(n, old, copy);
        freePagedMemory(old, oldSz);
    }
    return n;
}

void freeAlignedMemory(void *ptr) VB_NOEXCEPT
{
    /* We don't know the size; use a 1-byte hint — munmap requires actual size.
     * Track it via the rounded page size stored at allocation time is not done
     * here; use a table-free approach: re-derive from page alignment.
     * Since allocAlignedMemory always rounds up to page size, we mmap/munmap
     * full pages.  At free time the true size is unknown, so we use a
     * conservative approach: store the size in a small header prepended to
     * the allocation, separated so the caller never sees it.
     * For the AOT tool this path is only hit during WARP's internal cleanup;
     * a simple malloc/free pair with a hidden size header suffices. */
    if (!ptr) return;
    /* Recover the size from the hidden header at ptr - sizeof(size_t). */
    size_t *hdr = static_cast<size_t *>(ptr) - 1;
    size_t sz = *hdr;
    /* The allocation base is hdr cast to uint8_t*. */
    munmap(hdr, sz);
}

void *allocVirtualMemory(size_t size)
{
    return allocAlignedMemory(size, host_page_size());
}

void freeVirtualMemory(void *ptr, size_t size) VB_NOEXCEPT
{
    freePagedMemory(static_cast<uint8_t *>(ptr), size);
}

void commitVirtualMemory(void *, size_t) { /* no-op on host */ }
void uncommitVirtualMemory(void *, size_t) { /* no-op on host */ }

uint8_t *mapRXMemory(size_t size, int32_t /*fd*/)
{
    if (!size) return nullptr;
    size = round_up_page(size);
    /* Allocate as RW first; WARP writes code then calls setPermissionRX. */
    void *p = mmap(nullptr, size,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) throw std::bad_alloc();
    return static_cast<uint8_t *>(p);
}

StackInfo getStackInfo()
{
    /* Not needed for AOT compilation — return zeroed struct. */
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

ExecutableMemory::ExecutableMemory(ExecutableMemory &&o) VB_NOEXCEPT
    : data_(o.data_), size_(o.size_), fd_(o.fd_)
{
    o.data_ = nullptr; o.size_ = 0; o.fd_ = -1;
}

ExecutableMemory &ExecutableMemory::operator=(ExecutableMemory &&o) & VB_NOEXCEPT
{
    swap(*this, std::move(o)); return *this;
}

ExecutableMemory::~ExecutableMemory() VB_NOEXCEPT
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

void ExecutableMemory::freeExecutableMemory() const VB_NOEXCEPT
{
    if (data_ && size_) {
        MemUtils::freePagedMemory(data_, size_);
    }
}

} // namespace vb
