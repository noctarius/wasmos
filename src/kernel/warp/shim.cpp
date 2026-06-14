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

void *operator new(size_t size)            { return kalloc_small(size); }
void *operator new[](size_t size)          { return kalloc_small(size); }
void  operator delete(void *p) noexcept   { kfree_small(p); }
void  operator delete[](void *p) noexcept { kfree_small(p); }
void  operator delete(void *p, size_t) noexcept   { kfree_small(p); }
void  operator delete[](void *p, size_t) noexcept { kfree_small(p); }

// ---------------------------------------------------------------------------
// 2. Kernel allocator — block header tracks size for realloc
// ---------------------------------------------------------------------------

namespace {

struct AllocHeader {
    size_t size;
};

static inline AllocHeader *header_of(void *p) {
    return reinterpret_cast<AllocHeader *>(static_cast<uint8_t *>(p) - sizeof(AllocHeader));
}

} // namespace

static void *warp_kmalloc(size_t const size) {
    size_t total = sizeof(AllocHeader) + size;
    void *raw = kalloc_small(total);
    if (!raw) return nullptr;
    auto *hdr = static_cast<AllocHeader *>(raw);
    hdr->size = size;
    return hdr + 1;
}

static void *warp_krealloc(void *const ptr, size_t const size) {
    if (!ptr) return warp_kmalloc(size);
    if (!size) { kfree_small(header_of(ptr)); return nullptr; }
    size_t old_size = header_of(ptr)->size;
    void *n = warp_kmalloc(size);
    if (!n) return nullptr;
    size_t copy = old_size < size ? old_size : size;
    __builtin_memcpy(n, ptr, copy);
    kfree_small(header_of(ptr));
    return n;
}

static void warp_kfree(void *const ptr) {
    if (ptr) kfree_small(header_of(ptr));
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
