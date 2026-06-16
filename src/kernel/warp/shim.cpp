/* warp/shim.cpp - Freestanding C++ ABI shim and kernel allocator bindings for WARP.
 *
 * Provides three things:
 *   1. C++ ABI (operator new/delete, __cxa_* stubs) so WARP's C++14 library
 *      links against the bare-metal kernel without libc++.
 *   2. Kernel allocator wrappers for WasmModule::initEnvironment.
 *   3. Per-PID configuration tracking and CPU-local runtime serialization,
 *      mirroring the wasm3/shim.c interface so wasm_driver.c can switch
 *      backends via WASMOS_WASM_RUNTIME without changing call sites.
 *
 * Realloc note: kalloc_small has no size query, so we prepend an alloc header
 * to every block so warp_krealloc can copy the right number of bytes.
 */

#include <cstddef>
#include <cstdint>

extern "C" {
#include "klog.h"
#include "slab.h"
#include "physmem.h"
#include "paging.h"
#include "memory.h"
#include "process.h"
#include "arch/x86_64/smp.h"
}

#include "src/WasmModule/WasmModule.hpp"
#include "src/core/common/Span.hpp"
#include "shim.h"

// ---------------------------------------------------------------------------
// 1. operator new/delete — backed by the kernel slab allocator.
//    __cxa_* ABI stubs live in warp/cxx_abi.cpp to avoid duplicate symbols.
// ---------------------------------------------------------------------------

/* Forward declarations — defined after the two-tier allocator below. */
static void *warp_kmalloc(size_t);
static void  warp_kfree(void *);

/* operator new/delete use the two-tier allocator so that C++ objects of
 * any size (e.g., vb::WasmModule itself) are allocated correctly. */
void *operator new(size_t size)   { return warp_kmalloc(size); }
void *operator new[](size_t size) { return warp_kmalloc(size); }
void  operator delete(void *p) noexcept   { warp_kfree(p); }
void  operator delete[](void *p) noexcept { warp_kfree(p); }
void  operator delete(void *p, size_t) noexcept   { warp_kfree(p); }
void  operator delete[](void *p, size_t) noexcept { warp_kfree(p); }

// ---------------------------------------------------------------------------
// 2. Kernel allocator — two-tier: slab for small, page allocator for large.
//    AllocHeader.is_pages == 0: slab-backed, size = byte count.
//    AllocHeader.is_pages == 1: page-backed, size = page count.
//    This allows WARP to allocate the large blocks it needs for compiler
//    scratch, JIT output code, and WASM linear memory.
// ---------------------------------------------------------------------------

namespace {

struct AllocHeader {
    size_t   size;     /* byte count (is_pages=0) or page count (is_pages=1) */
    uint32_t is_pages; /* 0 = slab, 1 = page-allocator */
};

static constexpr size_t   kLargeThreshold = 112;           /* slab max usable */
static constexpr uint64_t kHalfBase       = 0xFFFFFFFF80000000ULL;
static constexpr uint64_t kPhysLimit      = 512ULL * 1024ULL * 1024ULL;
static constexpr size_t   kPageSize       = 4096UL;

static inline AllocHeader *header_of(void *p)
{
    return reinterpret_cast<AllocHeader *>(static_cast<uint8_t *>(p) - sizeof(AllocHeader));
}

static inline uint64_t phys_of_pages_ptr(void *p)
{
    return reinterpret_cast<uint64_t>(p) - kHalfBase;
}

static int warp_map_page_alias(uint64_t phys, uint64_t pages)
{
    if (!phys || pages == 0) {
        return -1;
    }
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t page_phys = phys + (i * kPageSize);
        uint64_t page_virt = page_phys + kHalfBase;
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

} // namespace

static void *warp_kmalloc(size_t const size)
{
    size_t total = sizeof(AllocHeader) + size;

    if (total <= kLargeThreshold) {
        /* Small path: slab allocator */
        void *raw = kalloc_small(total);
        if (!raw) return nullptr;
        auto *hdr = static_cast<AllocHeader *>(raw);
        hdr->size     = size;
        hdr->is_pages = 0;
        return hdr + 1;
    } else {
        /* Large path: physical page allocator */
        uint64_t pages = (static_cast<uint64_t>(total) + kPageSize - 1) / kPageSize;
        uint64_t phys  = pfa_alloc_pages_below(pages, kPhysLimit);
        if (!phys) return nullptr;
        if (warp_map_page_alias(phys, pages) != 0) {
            pfa_free_pages(phys, pages);
            return nullptr;
        }
        auto *hdr = reinterpret_cast<AllocHeader *>(phys | kHalfBase);
        hdr->size     = pages;   /* store PAGE count */
        hdr->is_pages = 1;
        return hdr + 1;
    }
}

static void *warp_krealloc(void *const ptr, size_t const size)
{
    if (!ptr) return warp_kmalloc(size);
    AllocHeader *old_hdr = header_of(ptr);
    size_t old_bytes;
    if (old_hdr->is_pages) {
        old_bytes = old_hdr->size * kPageSize - sizeof(AllocHeader);
    } else {
        old_bytes = old_hdr->size;
    }

    if (!size) {
        /* Free only */
        if (old_hdr->is_pages) {
            pfa_free_pages(phys_of_pages_ptr(old_hdr), old_hdr->size);
        } else {
            kfree_small(old_hdr);
        }
        return nullptr;
    }

    void *n = warp_kmalloc(size);
    if (!n) return nullptr;
    size_t copy = old_bytes < size ? old_bytes : size;
    __builtin_memcpy(n, ptr, copy);

    if (old_hdr->is_pages) {
        pfa_free_pages(phys_of_pages_ptr(old_hdr), old_hdr->size);
    } else {
        kfree_small(old_hdr);
    }
    return n;
}

static void warp_kfree(void *const ptr)
{
    if (!ptr) return;
    AllocHeader *hdr = header_of(ptr);
    if (hdr->is_pages) {
        pfa_free_pages(phys_of_pages_ptr(hdr), hdr->size);
    } else {
        kfree_small(hdr);
    }
}

// ---------------------------------------------------------------------------
// 3. Kernel ILogger — routes WARP diagnostics through klog_write
// ---------------------------------------------------------------------------

namespace {

class KernelLogger final : public vb::ILogger {
public:
    KernelLogger &operator<<(char const *const msg) override {
        if (msg) klog_write(msg);
        return *this;
    }
    KernelLogger &operator<<(const vb::Span<char const> &msg) override {
        // Span may not be null-terminated; write as many chars as we can.
        // TODO: use a bounded write when klog grows one.
        if (msg.data() && msg.size() > 0) klog_write(msg.data());
        return *this;
    }
    KernelLogger &operator<<(uint32_t const) override { return *this; }
};

static KernelLogger g_kernel_logger;

} // namespace

// ILogger accessor used by warp/link.cpp.
vb::ILogger &warp_kernel_logger() { return g_kernel_logger; }

// ---------------------------------------------------------------------------
// 4. Per-PID configuration table
// ---------------------------------------------------------------------------

namespace {

struct WarpPidConfig {
    uint64_t heap_size;
    uint64_t heap_max;
    uint8_t  configured;
};

static WarpPidConfig g_pid_config[PROCESS_MAX_COUNT];
static uint8_t       g_env_initialized = 0;

} // namespace

// ---------------------------------------------------------------------------
// 5. Public C API
// ---------------------------------------------------------------------------

extern "C" {

void
warp_heap_configure(uint32_t pid, uint64_t initial_size, uint64_t max_size)
{
    if (pid >= PROCESS_MAX_COUNT) return;
    if (!g_env_initialized) {
        vb::WasmModule::initEnvironment(warp_kmalloc, warp_krealloc, warp_kfree);
        g_env_initialized = 1;
    }
    g_pid_config[pid].heap_size  = initial_size;
    g_pid_config[pid].heap_max   = max_size;
    g_pid_config[pid].configured = 1;
}

uint32_t
warp_heap_bind_pid(uint32_t pid)
{
    uint32_t prev = cpu_local()->wasm3_heap_bound_pid;
    cpu_local()->wasm3_heap_bound_pid = pid;
    return prev;
}

void
warp_heap_restore_pid(uint32_t previous_pid)
{
    cpu_local()->wasm3_heap_bound_pid = previous_pid;
}

uint32_t
warp_runtime_enter(uint32_t pid)
{
    return warp_heap_bind_pid(pid);
}

void
warp_runtime_leave(uint32_t previous_pid)
{
    warp_heap_restore_pid(previous_pid);
}

void
warp_heap_release(uint32_t pid)
{
    if (pid >= PROCESS_MAX_COUNT) return;
    g_pid_config[pid] = WarpPidConfig{};
}

uint64_t
warp_heap_committed_bytes(uint32_t pid)
{
    if (pid >= PROCESS_MAX_COUNT || !g_pid_config[pid].configured) return 0;
    return g_pid_config[pid].heap_size;
}

int
warp_heap_probe_growth(size_t size)
{
    void *p = warp_kmalloc(size);
    if (!p) return -1;
    warp_kfree(p);
    return 0;
}

} // extern "C"
