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

static size_t host_page_size() {
    static long ps = sysconf(_SC_PAGESIZE);
    return (ps > 0) ? (size_t)ps : 4096UL;
}

static size_t round_up_page(size_t n) {
    size_t ps = host_page_size();
    return (n + ps - 1) & ~(ps - 1);
}

} // namespace

/* -----------------------------------------------------------------------
 * vb::MemUtils implementation
 * ----------------------------------------------------------------------- */

/* Implementations of the vb::MemUtils interface WARP declares; the contracts are
 * WARP's, this file only supplies host behaviour for them. Conventions that hold
 * across the whole namespace:
 *
 *  - Page size is the host's, queried once and cached; every size argument is
 *    rounded up to it, so an allocation is never smaller than requested but is
 *    usually larger.
 *  - The setPermission* family returns 0 on SUCCESS and -1 on failure, which is
 *    the opposite polarity to most of this tree. A null pointer or zero length
 *    is a no-op that reports success.
 *  - The alloc* family signals failure by throwing std::bad_alloc, never by
 *    returning null; allocPagedMemory is the exception and reports failure as a
 *    null MmapMemory::ptr.
 *  - Nothing here is thread-safe beyond what mmap/mprotect themselves guarantee,
 *    and freePagedMemory must be given the same size the allocation used, since
 *    no side table records it. */
namespace vb {
namespace MemUtils {

size_t getOSMemoryPageSize() VB_NOEXCEPT {
    return host_page_size();
}

size_t roundUpToOSMemoryPageSize(size_t n) VB_NOEXCEPT {
    return round_up_page(n);
}

size_t roundDownToOSMemoryPageSize(size_t n) VB_NOEXCEPT {
    size_t ps = host_page_size();
    return n & ~(ps - 1);
}

MmapMemory allocPagedMemory(size_t size) {
    MmapMemory m{nullptr, -1};
    if (!size)
        return m;
    size = round_up_page(size);
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return m;
    m.ptr = static_cast<uint8_t*>(p);
    m.fd = -1;
    return m;
}

void freePagedMemory(uint8_t* ptr, size_t size) VB_NOEXCEPT {
    if (!ptr || !size)
        return;
    munmap(ptr, round_up_page(size));
}

int32_t setPermissionRWX(uint8_t* start, size_t len) VB_NOEXCEPT {
    if (!start || !len)
        return 0;
    /* Both branches are the same plain RWX mprotect. The Apple arm being split
     * out marks where a hardened-runtime host would need MAP_JIT pages plus
     * pthread_jit_write_protect_np instead; warp_aot only compiles and
     * serialises, so it never executes what it writes here. */
#ifdef __APPLE__
    return mprotect(start, round_up_page(len), PROT_READ | PROT_WRITE | PROT_EXEC) == 0 ? 0 : -1;
#else
    return mprotect(start, round_up_page(len), PROT_READ | PROT_WRITE | PROT_EXEC) == 0 ? 0 : -1;
#endif
}

int32_t setPermissionRX(uint8_t* start, size_t len) VB_NOEXCEPT {
    if (!start || !len)
        return 0;
    return mprotect(start, round_up_page(len), PROT_READ | PROT_EXEC) == 0 ? 0 : -1;
}

int32_t setPermissionRW(uint8_t* start, size_t len) VB_NOEXCEPT {
    if (!start || !len)
        return 0;
    return mprotect(start, round_up_page(len), PROT_READ | PROT_WRITE) == 0 ? 0 : -1;
}

/* Copy freshly generated code and make it visible to instruction fetch. On
 * x86-64 the instruction cache is coherent with data writes, so the copy plus a
 * compiler barrier is the whole operation and clearInstructionCache is empty;
 * both would need real cache maintenance on a weakly ordered target. */
void memcpyAndClearInstrCache(uint8_t* dest, uint8_t const* src, size_t n) VB_NOEXCEPT {
    if (!dest || !src || !n)
        return;
    memcpy(dest, src, n);
    /* x86-64: cache is coherent; no explicit flush needed. */
    __asm__ volatile("" ::: "memory");
}

void clearInstructionCache(uint8_t*, size_t) VB_NOEXCEPT {
    /* x86-64: no-op. */
}

uint8_t* allocAlignedMemory(size_t size, size_t /*alignment*/) {
    size = round_up_page(size);
    MmapMemory m = allocPagedMemory(size);
    if (!m.ptr)
        throw std::bad_alloc();
    return m.ptr;
}

/* Grow or shrink a page allocation. Returns `old` unchanged when the rounded
 * sizes match, so the caller cannot assume the pointer moved or that it did not.
 * Otherwise it allocates, copies min(oldSz, newSz) bytes and frees the original,
 * which means shrinking discards the tail. alignment is ignored: every mapping
 * is already page-aligned, which is at least as strict as anything WARP asks
 * for. Throws std::bad_alloc if the new mapping fails, leaving `old` valid. */
uint8_t* reallocAlignedMemory(uint8_t* old, size_t oldSz, size_t newSz, size_t alignment) {
    newSz = round_up_page(newSz);
    if (oldSz == newSz)
        return old;
    uint8_t* n = allocAlignedMemory(newSz, alignment);
    if (old) {
        size_t copy = oldSz < newSz ? oldSz : newSz;
        memcpy(n, old, copy);
        freePagedMemory(old, oldSz);
    }
    return n;
}

/* FIXME: this reads a size header that allocAlignedMemory never writes.
 * allocAlignedMemory returns a bare mmap base, so ptr - sizeof(size_t) lands in
 * the preceding (usually unmapped) page: the load faults, or munmap is handed a
 * garbage length. Either allocAlignedMemory has to prepend the header this
 * assumes, or the sizes have to be tracked in a side table. warp_aot survives
 * only because WARP does not reach this path before the process exits. */
void freeAlignedMemory(void* ptr) VB_NOEXCEPT {
    if (!ptr)
        return;
    size_t* hdr = static_cast<size_t*>(ptr) - 1;
    size_t sz = *hdr;
    munmap(hdr, sz);
}

void* allocVirtualMemory(size_t size) {
    return allocAlignedMemory(size, host_page_size());
}

void freeVirtualMemory(void* ptr, size_t size) VB_NOEXCEPT {
    freePagedMemory(static_cast<uint8_t*>(ptr), size);
}

/* Reserve/release virtual address space. On the host a reservation is a normal
 * anonymous mapping, so commit and uncommit have nothing left to do: the pages
 * are already backed on demand by the kernel. A target that separates reserve
 * from commit would implement these; here they are deliberately empty and
 * ignore their arguments. */
void commitVirtualMemory(void*, size_t) { /* no-op on host */ }
void uncommitVirtualMemory(void*, size_t) { /* no-op on host */ }

uint8_t* mapRXMemory(size_t size, int32_t /*fd*/) {
    if (!size)
        return nullptr;
    size = round_up_page(size);
    /* The fd is ignored: allocPagedMemory here is plain MAP_ANONYMOUS and hands
     * back fd = -1, so there is no shared object to map a second view of. The
     * mapping is RWX from the start because callers write code into it and only
     * afterwards narrow it with setPermissionRX. */
    void* p =
        mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        throw std::bad_alloc();
    return static_cast<uint8_t*>(p);
}

/* Bounds of the current thread's stack, used by WARP's stack-overflow guard.
 * Returns an all-zero struct, which reports no usable stack window rather than a
 * wrong one. warp_aot only compiles and never calls into generated code, so the
 * guard is not consulted there. This file is also linked into the host WARP
 * coroutine test, which does execute generated code, so whatever that guard does
 * with an empty window is what it does there. */
StackInfo getStackInfo() {
    /* Not needed for AOT compilation — return zeroed struct. */
    return StackInfo{};
}

} // namespace MemUtils

/* -----------------------------------------------------------------------
 * vb::ExecutableMemory implementation
 * ----------------------------------------------------------------------- */

/* Both entry points below must leave data_ EXECUTABLE.  allocPagedMemory hands
 * back PROT_READ|PROT_WRITE, so the copied code has to be mprotect'ed before
 * anything jumps into it — otherwise the first call into a JIT-compiled module
 * dies with SIGSEGV on any host that enforces NX.  warp_aot itself only compiles
 * and serialises, so it never executes and never exercised this; the host WARP
 * coroutine test (tests/unit/test_warp_wasm_coroutine.cpp) does.
 *
 * This mirrors the non-Linux branch of WARP's own ExecutableMemory::init
 * (libs/warp/src/utils/ExecutableMemory.cpp).  Its __linux__ branch instead maps
 * a second RX view through the memfd returned by allocPagedMemory, which this
 * shim cannot reproduce: this allocPagedMemory is plain MAP_ANONYMOUS and
 * reports fd = -1, so mapRXMemory would return a fresh, empty mapping rather
 * than an alias of the code just written. */
ExecutableMemory::ExecutableMemory(uint8_t const* data, size_t size)
    : ExecutableMemory(nullptr, size, -1) {
    if (size == 0)
        return;
    MemUtils::MmapMemory m = MemUtils::allocPagedMemory(size);
    if (!m.ptr)
        throw std::bad_alloc();
    data_ = m.ptr;
    size_ = size;
    MemUtils::memcpyAndClearInstrCache(data_, data, size);
    if (MemUtils::setPermissionRX(data_, size_) != 0)
        throw std::bad_alloc();
}

/* Move operations and destructor. The moved-from object is left empty so its
 * destructor unmaps nothing, and the destructor frees only when data_ is
 * non-null. Ownership of the mapping is what moves; the code inside it is not
 * copied. */
ExecutableMemory::ExecutableMemory(ExecutableMemory&& o) VB_NOEXCEPT : data_(o.data_),
                                                                       size_(o.size_),
                                                                       fd_(o.fd_) {
    o.data_ = nullptr;
    o.size_ = 0;
    o.fd_ = -1;
}

ExecutableMemory& ExecutableMemory::operator=(ExecutableMemory&& o) & VB_NOEXCEPT {
    swap(*this, std::move(o));
    return *this;
}

ExecutableMemory::~ExecutableMemory() VB_NOEXCEPT {
    if (data_)
        freeExecutableMemory();
}

void ExecutableMemory::init(uint8_t const* data) {
    if (size_ == 0)
        return;
    MemUtils::MmapMemory m = MemUtils::allocPagedMemory(size_);
    if (!m.ptr)
        throw std::bad_alloc();
    fd_ = m.fd;
    data_ = m.ptr;
    MemUtils::memcpyAndClearInstrCache(data_, data, size_);
    if (MemUtils::setPermissionRX(data_, size_) != 0)
        throw std::bad_alloc();
}

/* Unmap the code mapping. Tolerates a null or zero-sized object and does not
 * clear data_/size_ (it is const), so it must not be called twice on the same
 * live object; the destructor is the only caller. */
void ExecutableMemory::freeExecutableMemory() const VB_NOEXCEPT {
    if (data_ && size_) {
        MemUtils::freePagedMemory(data_, size_);
    }
}

} // namespace vb
