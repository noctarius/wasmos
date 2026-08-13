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
 * kalloc_small has no size query, so every block carries a prepended alloc
 * header and warp_krealloc reads the length from it.
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
#include "hashmap.h"
#include "linmem_slots.h"
#include "arch/x86_64/smp.h"
#include "sync/spinlock.h"
}

#ifdef WASMOS_WASM_RUNTIME_WARP
/* Forward declarations: implemented in mem_utils_kernel.cpp.
 * Track large warp_kmalloc allocations so ring-3 phys-range queries work. */
extern "C" void warp_mem_kmalloc_register(uint64_t phys, uint64_t pages, uint64_t data_offset);
extern "C" void warp_mem_kmalloc_unregister(uint64_t phys);
#endif

/* Physical base of a page-backed allocation's kernel-alias pointer.  Routes
 * through the mem_utils tracking table / PT-walk so linmem VA-slot pointers
 * resolve correctly (identical to `virt - kHalfBase` for direct-mapped ones). */
extern "C" uint64_t warp_mem_alias_phys(uint64_t virt);

#include "src/WasmModule/WasmModule.hpp"
#include "src/core/common/Span.hpp"
#include "shim.h"

// ---------------------------------------------------------------------------
// 1. operator new/delete — backed by the kernel slab allocator.
//    __cxa_* ABI stubs live in warp/cxx_abi.cpp to avoid duplicate symbols.
// ---------------------------------------------------------------------------

/* Forward declarations — defined after the two-tier allocator below. */
static void* warp_kmalloc(size_t);
static void warp_kfree(void*);

/* Dedicated-VA linmem slot helpers (defined after the per-pid config table).
 * warp_linmem_move: relocate the just-identified linmem block into a per-app VA
 *   slot (reserve VA, commit+zero scattered phys on demand, base pinned).
 * warp_linmem_grow: commit more scattered zeroed pages into the slot in place.
 * warp_linmem_slot_free_pid: idempotent free (walk-unmap + pfa_free + release
 *   the slot), routed from BOTH warp_kfree(is_pages==2) and the reap hook. */
static void* warp_linmem_move(uint32_t pid, void* old_ptr, size_t old_bytes, size_t size);
static void* warp_linmem_grow(void* ptr, size_t size);
static void warp_linmem_slot_free_pid(uint32_t pid);
/* The VA-slot pool itself (reserve/commit/decommit) lives in the shared
 * linmem_slots primitive; WARP layers its AllocHeader + per-pid config on top. */
/* One-shot hint: pid of the linmem block about to be identified at its first
 * warp_krealloc grow (armed by warp_linmem_reserve_hint before module init). */
static uint32_t g_linmem_reserve_pid = 0;
static uint64_t g_linmem_reserve_bytes = 0;

/* operator new/delete use the two-tier allocator so that C++ objects of
 * any size (e.g., vb::WasmModule itself) are allocated correctly. */
void* operator new(size_t size) {
    return warp_kmalloc(size);
}
void* operator new[](size_t size) {
    return warp_kmalloc(size);
}
void operator delete(void* p) noexcept {
    warp_kfree(p);
}
void operator delete[](void* p) noexcept {
    warp_kfree(p);
}
void operator delete(void* p, size_t) noexcept {
    warp_kfree(p);
}
void operator delete[](void* p, size_t) noexcept {
    warp_kfree(p);
}

// ---------------------------------------------------------------------------
// 2. Kernel allocator — two-tier: slab for small, page allocator for large.
//    AllocHeader.is_pages == 1: page-backed, size = page count.
//    AllocHeader.is_pages == 3: dynamically grown WARP compiler small pool.
//    This allows WARP to allocate the large blocks it needs for compiler
//    scratch, JIT output code, and WASM linear memory.
// ---------------------------------------------------------------------------

namespace {

struct AllocHeader {
    size_t size;        /* requested byte count */
    size_t capacity;    /* usable byte capacity */
    size_t pages;       /* page count for page-backed allocations */
    uint32_t is_pages;  /* 1 = contiguous page-alloc, 2 = linmem VA slot, 3 = WARP small pool */
    uint32_t owner_pid; /* is_pages==2 only: owning pid, for slot free/dispatch */
};

static constexpr size_t kLargeThreshold = 112; /* slab max usable */
static constexpr uint64_t kHalfBase = 0xFFFFFFFF80000000ULL;
static constexpr uint64_t kPhysLimit = 512ULL * 1024ULL * 1024ULL;
static constexpr size_t kPageSize = 4096UL;
static constexpr size_t kWarpSmallBlockSize = 256UL;

/* WARP's compiler makes many tiny, independently freed allocations.  They
 * must not consume the kernel-wide fixed slab classes: service metadata also
 * uses those classes, and the net TX queue made the former coupling visible.
 * These blocks grow one physical page at a time and return an empty page to
 * the PFA, so compiler capacity follows available physical memory. */
struct WarpSmallPage {
    WarpSmallPage* next;
    uint64_t phys;
    uint32_t free_blocks;
};
struct WarpSmallFreeBlock {
    WarpSmallFreeBlock* next;
};
static_assert(sizeof(WarpSmallPage) <= kWarpSmallBlockSize, "small-pool metadata fits first block");
static constexpr uint32_t kWarpSmallBlocksPerPage =
    static_cast<uint32_t>(kPageSize / kWarpSmallBlockSize) - 1U;
static WarpSmallPage* g_warp_small_pages = nullptr;
static WarpSmallFreeBlock* g_warp_small_free = nullptr;
static ksync_spinlock_t g_warp_small_lock;

static inline AllocHeader* header_of(void* p) {
    return reinterpret_cast<AllocHeader*>(static_cast<uint8_t*>(p) - sizeof(AllocHeader));
}

static inline uint64_t phys_of_pages_ptr(void* p) {
    return warp_mem_alias_phys(reinterpret_cast<uint64_t>(p));
}

static int warp_map_page_alias(uint64_t phys, uint64_t pages) {
    if (!phys || pages == 0) {
        return -1;
    }
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t page_phys = phys + (i * kPageSize);
        uint64_t page_virt = page_phys + kHalfBase;
        if (paging_map_4k(page_virt, page_phys,
                          MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_EXEC) !=
            0) {
            return -1;
        }
    }
    return 0;
}

static WarpSmallPage* warp_small_page_for(void* raw) {
    return reinterpret_cast<WarpSmallPage*>(reinterpret_cast<uintptr_t>(raw) & ~(kPageSize - 1U));
}

static void* warp_small_alloc(void) {
    ksync_spinlock_lock(&g_warp_small_lock);
    if (g_warp_small_free == nullptr) {
        uint64_t phys = pfa_alloc_pages_above(1U, WASMOS_SHMEM_PHYS_LIMIT);
        if (!phys || warp_map_page_alias(phys, 1U) != 0) {
            if (phys) {
                pfa_free_pages(phys, 1U);
            }
            ksync_spinlock_unlock(&g_warp_small_lock);
            return nullptr;
        }
        auto* page = reinterpret_cast<WarpSmallPage*>(phys | kHalfBase);
        page->next = g_warp_small_pages;
        page->phys = phys;
        page->free_blocks = kWarpSmallBlocksPerPage;
        g_warp_small_pages = page;
        for (uint32_t i = 1U; i <= kWarpSmallBlocksPerPage; ++i) {
            auto* block = reinterpret_cast<WarpSmallFreeBlock*>(reinterpret_cast<uint8_t*>(page) +
                                                                i * kWarpSmallBlockSize);
            block->next = g_warp_small_free;
            g_warp_small_free = block;
        }
    }
    WarpSmallFreeBlock* block = g_warp_small_free;
    g_warp_small_free = block->next;
    WarpSmallPage* page = warp_small_page_for(block);
    --page->free_blocks;
    ksync_spinlock_unlock(&g_warp_small_lock);
    return block;
}

static void warp_small_free(void* raw) {
    WarpSmallPage* page = warp_small_page_for(raw);
    ksync_spinlock_lock(&g_warp_small_lock);
    auto* block = static_cast<WarpSmallFreeBlock*>(raw);
    block->next = g_warp_small_free;
    g_warp_small_free = block;
    ++page->free_blocks;
    if (page->free_blocks != kWarpSmallBlocksPerPage) {
        ksync_spinlock_unlock(&g_warp_small_lock);
        return;
    }

    WarpSmallFreeBlock** link = &g_warp_small_free;
    while (*link != nullptr) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(*link);
        if ((addr & ~(kPageSize - 1U)) == reinterpret_cast<uintptr_t>(page)) {
            *link = (*link)->next;
        } else {
            link = &(*link)->next;
        }
    }
    WarpSmallPage** page_link = &g_warp_small_pages;
    while (*page_link != nullptr && *page_link != page) {
        page_link = &(*page_link)->next;
    }
    if (*page_link == page) {
        *page_link = page->next;
        pfa_free_pages(page->phys, 1U);
    }
    ksync_spinlock_unlock(&g_warp_small_lock);
}

} // namespace

static void* warp_kmalloc(size_t const size) {
    size_t total = sizeof(AllocHeader) + size;

    if (total <= kLargeThreshold) {
        /* Small path: WARP-private, dynamically backed pool. */
        void* raw = warp_small_alloc();
        if (!raw) {
            klog_printf("[warp-mem] small pool alloc failed size=%llu total=%llu\n",
                        (unsigned long long)size, (unsigned long long)total);
            return nullptr;
        }
        auto* hdr = static_cast<AllocHeader*>(raw);
        hdr->size = size;
        hdr->capacity = kWarpSmallBlockSize - sizeof(AllocHeader);
        hdr->pages = 0;
        hdr->is_pages = 3;
        hdr->owner_pid = 0;
        return hdr + 1;
    } else {
        /* Large path: physical page allocator */
        uint64_t pages = (static_cast<uint64_t>(total) + kPageSize - 1) / kPageSize;
        uint64_t phys = pfa_alloc_pages_above(pages, WASMOS_SHMEM_PHYS_LIMIT);
        if (!phys) {
            return nullptr;
        }
        if (warp_map_page_alias(phys, pages) != 0) {
            pfa_free_pages(phys, pages);
            return nullptr;
        }
#ifdef WASMOS_WASM_RUNTIME_WARP
        warp_mem_kmalloc_register(phys, pages, sizeof(AllocHeader));
#endif
        auto* hdr = reinterpret_cast<AllocHeader*>(phys | kHalfBase);
        hdr->size = size;
        hdr->capacity = pages * kPageSize - sizeof(AllocHeader);
        hdr->pages = pages;
        hdr->is_pages = 1;
        return hdr + 1;
    }
}

static void* warp_krealloc(void* const ptr, size_t const size) {
    if (!ptr)
        return warp_kmalloc(size);
    AllocHeader* old_hdr = header_of(ptr);
    size_t old_bytes = old_hdr->size;

    /* Dedicated-VA linmem slot (is_pages==2): dispatch FIRST — its VA base is
     * NOT in the direct map, so phys_of_pages_ptr() on it would be garbage. */
    if (old_hdr->is_pages == 2) {
        if (!size) {
            warp_linmem_slot_free_pid(old_hdr->owner_pid); /* idempotent */
            return nullptr;
        }
        if (size <= old_hdr->capacity) {
            /* Commit-on-demand: unlike a contiguous block, [committed, size) is
             * not yet backed — warp_linmem_grow maps+zeroes the new pages and
             * returns the SAME base (no relocation). */
            return warp_linmem_grow(ptr, size);
        }
        return nullptr; /* exceeds the reserved VA slot */
    }

    if (!size) {
        /* Free only (contiguous page block or slab). */
        if (old_hdr->is_pages == 3) {
            warp_small_free(old_hdr);
        } else if (old_hdr->is_pages == 1) {
#ifdef WASMOS_WASM_RUNTIME_WARP
            warp_mem_kmalloc_unregister(phys_of_pages_ptr(old_hdr));
#endif
            pfa_free_pages(phys_of_pages_ptr(old_hdr), old_hdr->pages);
        }
        return nullptr;
    }

    if (size <= old_hdr->capacity) {
        old_hdr->size = size;
        return ptr;
    }

    /* Grow.  The first growth of a page-backed block identifies the WARP
     * linmem/job-memory block (the only page-backed block that grows).  If a
     * reservation is pending, MOVE it into a dedicated per-app VA slot: reserve
     * the VA once, commit scattered physical pages on demand, base pinned for
     * the app's lifetime (no relocation → shmem/DMA maps stay valid). */
    /* Claim the hint only from the CPU actually running the armed pid, and take
     * it with a CAS so two CPUs cannot both move a block for one arming.  A
     * mismatch leaves the hint armed for its rightful owner instead of stamping
     * this block with a foreign pid (whose later free would then release a slot
     * still in use). */
    uint32_t bound_pid = cpu_local()->wasm3_heap_bound_pid;
    uint32_t claim = bound_pid;
    if (old_hdr->is_pages == 1 && g_linmem_reserve_bytes && bound_pid != 0 &&
        __atomic_compare_exchange_n(&g_linmem_reserve_pid, &claim, 0u, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        uint32_t pid = bound_pid;
#if WASMOS_TRACE
        klog_printf("[trace-linmem] hint claim pid=%u size=%llx reserve=%llx oldpages=%llx\n",
                    (unsigned)pid, (unsigned long long)size,
                    (unsigned long long)g_linmem_reserve_bytes, (unsigned long long)old_hdr->pages);
#endif
        g_linmem_reserve_bytes = 0;
        void* moved = warp_linmem_move(pid, ptr, old_bytes, size);
        if (moved) {
            return moved; /* old contiguous block freed inside warp_linmem_move */
        }
        /* Slot exhausted / commit failed: fall through to the legacy contiguous
         * realloc so the app still runs (without the pinned-base benefit). */
    }

    size_t target = size;
    if (old_hdr->is_pages && old_hdr->capacity < (SIZE_MAX / 2)) {
        size_t grown = old_hdr->capacity * 2;
        if (grown > target) {
            target = grown;
        }
    }

    void* n = warp_kmalloc(target);
    if (!n) {
        return nullptr;
    }
    header_of(n)->size = size;
    size_t copy = old_bytes < size ? old_bytes : size;
    __builtin_memcpy(n, ptr, copy);

    if (old_hdr->is_pages == 1) {
#ifdef WASMOS_WASM_RUNTIME_WARP
        warp_mem_kmalloc_unregister(phys_of_pages_ptr(old_hdr));
#endif
        pfa_free_pages(phys_of_pages_ptr(old_hdr), old_hdr->pages);
    } else if (old_hdr->is_pages == 3) {
        warp_small_free(old_hdr);
    }
    return n;
}

static void warp_kfree(void* const ptr) {
    if (!ptr)
        return;
    AllocHeader* hdr = header_of(ptr);
    if (hdr->is_pages == 2) {
        /* Dedicated-VA linmem slot — dispatch BEFORE phys_of_pages_ptr (its VA
         * base is not in the direct map).  Idempotent with the reap-path free. */
        warp_linmem_slot_free_pid(hdr->owner_pid);
        return;
    }
    if (hdr->is_pages == 3) {
        warp_small_free(hdr);
    } else if (hdr->is_pages == 1) {
#ifdef WASMOS_WASM_RUNTIME_WARP
        warp_mem_kmalloc_unregister(phys_of_pages_ptr(hdr));
#endif
        pfa_free_pages(phys_of_pages_ptr(hdr), hdr->pages);
    }
}

// ---------------------------------------------------------------------------
// 3. Kernel ILogger — routes WARP diagnostics through klog_write
// ---------------------------------------------------------------------------

namespace {

class KernelLogger final : public vb::ILogger {
  public:
    KernelLogger& operator<<(char const* const msg) override {
        if (msg)
            klog_write(msg);
        return *this;
    }
    KernelLogger& operator<<(const vb::Span<char const>& msg) override {
        if (msg.data() && msg.size() > 0) {
            char buf[128];
            size_t n = msg.size() < 127 ? msg.size() : 127;
            __builtin_memcpy(buf, msg.data(), n);
            buf[n] = '\0';
            klog_write(buf);
        }
        return *this;
    }
    KernelLogger& operator<<(uint32_t const) override {
        return *this;
    }
};

static KernelLogger g_kernel_logger;

} // namespace

// ILogger accessor used by warp/link.cpp.
vb::ILogger& warp_kernel_logger() {
    return g_kernel_logger;
}

// ---------------------------------------------------------------------------
// 4. Per-PID configuration table
// ---------------------------------------------------------------------------

namespace {

struct WarpPidConfig {
    uint64_t heap_size;
    uint64_t heap_max;
    uint64_t linmem_reserved;        /* map_auto scan ceiling (bytes) */
    uint32_t linmem_slot;            /* dedicated-VA slot index, or LINMEM_SLOT_NONE */
    uint64_t linmem_va_base;         /* VA slot base (AllocHeader lives here) */
    uint64_t linmem_committed_pages; /* pages committed into the slot so far */
    uint8_t configured;
};

/* LINMEM_SLOT_NONE comes from linmem_slots.h (shared with the slot primitive). */

/* Per-pid heap configuration, keyed by pid in a growable hashmap (no fixed
 * process-count bound).  Created on configure and removed on exit via
 * warp_heap_release (driven from warp_release_pid in warp/link.cpp).  Lazily
 * initialized on first configure since shim.cpp has no init entry point. */
static hashmap_t g_pid_config_map;
static uint8_t g_env_initialized = 0;

static WarpPidConfig* pid_config_get(uint32_t pid) {
    if (g_pid_config_map.bucket_count == 0)
        return nullptr;
    return static_cast<WarpPidConfig*>(hashmap_get(&g_pid_config_map, pid));
}

} // namespace

/* ---- Dedicated-VA linmem slots (Step 2b) --------------------------------- *
 * Each app's WARP linmem block lives in a VA slot from the shared linmem_slots
 * pool (paging.h WARP_LINMEM_* window): the VA is reserved once, scattered
 * physical pages are committed on demand as the block grows, so the base is
 * pinned for the app's lifetime (no relocation).  The functions below are the
 * WARP glue — AllocHeader tagging and per-pid config — over that primitive. */

/* Move the just-identified linmem block into a dedicated per-app VA slot.
 * Returns the new memoryBase (va_base + AllocHeader), or nullptr on failure
 * (caller falls back to the legacy contiguous realloc). */
static void* warp_linmem_move(uint32_t pid, void* old_ptr, size_t old_bytes, size_t size) {
    WarpPidConfig* cfg = pid_config_get(pid);
    if (!cfg) {
        return nullptr;
    }
    int slot = linmem_slot_alloc();
    if (slot < 0) {
        return nullptr;
    }
    uint64_t va_base = linmem_slot_va((uint32_t)slot);
    uint64_t need_pages = (sizeof(AllocHeader) + (uint64_t)size + kPageSize - 1) / kPageSize;
    if (linmem_slot_commit(va_base, 0, need_pages) != 0) {
        linmem_slot_release((uint32_t)slot);
        return nullptr;
    }
    AllocHeader* nh = reinterpret_cast<AllocHeader*>(static_cast<uintptr_t>(va_base));
    nh->size = size;
    /* Logical capacity = the whole reserved slot (minus header); grows commit
     * on demand up to it without ever relocating. */
    nh->capacity = WARP_LINMEM_VA_STRIDE - sizeof(AllocHeader);
    nh->pages = need_pages; /* committed pages so far */
    nh->is_pages = 2;
    nh->owner_pid = pid;
    void* nmem = reinterpret_cast<void*>(static_cast<uintptr_t>(va_base + sizeof(AllocHeader)));
    __builtin_memcpy(nmem, old_ptr, old_bytes < size ? old_bytes : size);

    cfg->linmem_slot = (uint32_t)slot;
    cfg->linmem_va_base = va_base;
    cfg->linmem_committed_pages = need_pages;
    linmem_slot_set_owner((uint32_t)slot, pid);
    /* map_auto scan ceiling = the app's DECLARED size, NOT the 2 GiB slot.  The
     * slot's capacity is 2 GiB (commit-on-demand growth), but the shmem-window
     * scan must stay within what the app declared/can commit — a 2 GiB ceiling
     * would trip map_auto's 2 MiB low_guard and push a window past the app's
     * module max (fault).  This mirrors the ceiling that worked pre-VA-slot. */
    cfg->linmem_reserved = cfg->heap_size;
#if WASMOS_TRACE
    klog_printf("[trace-linmem] move pid=%u va=%llx committed_pages=%llx reserved=%llx\n",
                (unsigned)pid, (unsigned long long)va_base, (unsigned long long)need_pages,
                (unsigned long long)cfg->heap_size);
#endif

    /* Free the old contiguous direct-map block. */
    AllocHeader* old_hdr = header_of(old_ptr);
#ifdef WASMOS_WASM_RUNTIME_WARP
    warp_mem_kmalloc_unregister(phys_of_pages_ptr(old_hdr));
#endif
    pfa_free_pages(phys_of_pages_ptr(old_hdr), old_hdr->pages);
    return nmem;
}

/* Commit-on-demand growth of a VA-slot block; returns the SAME base. */
static void* warp_linmem_grow(void* ptr, size_t size) {
    AllocHeader* hdr = header_of(ptr);
    uint64_t va_base = reinterpret_cast<uint64_t>(hdr);
    uint64_t need_pages = (sizeof(AllocHeader) + (uint64_t)size + kPageSize - 1) / kPageSize;
    if (need_pages > hdr->pages) {
        if (linmem_slot_commit(va_base, hdr->pages, need_pages) != 0) {
            return nullptr;
        }
        hdr->pages = need_pages;
        WarpPidConfig* cfg = pid_config_get(hdr->owner_pid);
        if (cfg) {
            cfg->linmem_committed_pages = need_pages;
        }
    }
    hdr->size = size;
    return ptr;
}

/* Idempotent free: walk-unmap + pfa_free the committed pages and release the
 * slot.  Routed from BOTH warp_kfree(is_pages==2) and the reap hook
 * (warp_heap_release); the linmem_slot==NONE guard makes the second a no-op,
 * mirroring the ZOMBIE->REAPING idempotency. */
static void warp_linmem_slot_free_pid(uint32_t pid) {
    WarpPidConfig* cfg = pid_config_get(pid);
    if (!cfg || cfg->linmem_slot == LINMEM_SLOT_NONE) {
        return;
    }
    linmem_slot_decommit(cfg->linmem_va_base, cfg->linmem_committed_pages);
    linmem_slot_release(cfg->linmem_slot);
    cfg->linmem_slot = LINMEM_SLOT_NONE;
    cfg->linmem_va_base = 0;
    cfg->linmem_committed_pages = 0;
}

// ---------------------------------------------------------------------------
// 5. Public C API
// ---------------------------------------------------------------------------

extern "C" {

void warp_heap_configure(uint32_t pid, uint64_t initial_size, uint64_t max_size) {
    if (pid == 0)
        return;
    if (!g_env_initialized) {
        vb::WasmModule::initEnvironment(warp_kmalloc, warp_krealloc, warp_kfree);
        g_env_initialized = 1;
    }
    if (g_pid_config_map.bucket_count == 0) {
        hashmap_init(&g_pid_config_map, sizeof(WarpPidConfig), 64);
    }
    WarpPidConfig* cfg = static_cast<WarpPidConfig*>(hashmap_put(&g_pid_config_map, pid));
    if (!cfg)
        return;
    cfg->heap_size = initial_size;
    cfg->heap_max = max_size;
    cfg->linmem_reserved = 0;
    cfg->linmem_slot = LINMEM_SLOT_NONE; /* no VA slot until first grow */
    cfg->linmem_va_base = 0;
    cfg->linmem_committed_pages = 0;
    cfg->configured = 1;
}

uint32_t warp_heap_bind_pid(uint32_t pid) {
    uint32_t prev = cpu_local()->wasm3_heap_bound_pid;
    cpu_local()->wasm3_heap_bound_pid = pid;
    return prev;
}

void warp_heap_restore_pid(uint32_t previous_pid) {
    cpu_local()->wasm3_heap_bound_pid = previous_pid;
}

uint32_t warp_runtime_enter(uint32_t pid) {
    /* FIXME(smp-warp): WARP assumes "only one CPU inside WARP at any time"
     * (see warp/sjlj_unwind.cpp), but this enter/leave pair only rebinds the
     * per-CPU heap PID — it takes NO lock. As long as all WARP modules run on
     * CPU 0 this is fine, but once apps/services execute on APs (via work
     * stealing or cross-CPU placement) the serialization invariant is violated
     * and WARP's shared state (sjlj context stack, codegen/runtime globals)
     * corrupts, producing flaky cross-CPU IPC lost-wakeups/livelocks at boot.
     * A correct fix must serialize only the JIT/native execution region while
     * RELEASING across blocking IPC waits (a naive lock around the whole
     * enter/leave span brackets the blocking waits and deadlocks). This gates
     * the smp-distribution work in process_spawn_idle_ap(). */
    return warp_heap_bind_pid(pid);
}

void warp_runtime_leave(uint32_t previous_pid) {
    warp_heap_restore_pid(previous_pid);
}

void warp_heap_release(uint32_t pid) {
    if (pid == 0)
        return;
    /* Reap-path free of the dedicated-VA linmem slot (idempotent; a no-op if the
     * sync teardown already freed it).  Must run BEFORE the cfg is removed — it
     * reads va_base/committed_pages from the cfg to walk-unmap + pfa_free. */
    warp_linmem_slot_free_pid(pid);
    (void)hashmap_remove(&g_pid_config_map, pid);
}

uint64_t warp_heap_committed_bytes(uint32_t pid) {
    WarpPidConfig* cfg = pid_config_get(pid);
    if (!cfg || !cfg->configured)
        return 0;
    return cfg->heap_size;
}

/* Reserved linmem-block capacity (bytes) for this pid — the map_auto scan
 * ceiling.  0 until the block moves into its dedicated VA slot. */
uint64_t warp_linmem_reserved_bytes(uint32_t pid) {
    WarpPidConfig* cfg = pid_config_get(pid);
    if (!cfg || !cfg->configured)
        return 0;
    return cfg->linmem_reserved;
}

/* One-shot: arm the pid of the linmem block about to be identified at its first
 * warp_krealloc grow.  Called just before the module's first linmem-allocating
 * probe; consumed by warp_krealloc's move-to-slot path. */
void warp_linmem_reserve_hint(uint32_t pid, uint64_t reserve_bytes) {
    g_linmem_reserve_pid = pid;
    g_linmem_reserve_bytes = reserve_bytes;
}

int warp_linmem_kernel_window_query(const uint8_t* linmem_kernel_ptr, uint64_t* out_slot_va_base,
                                    uint64_t* out_basedata_length, uint64_t* out_committed_pages) {
    if (!linmem_kernel_ptr || !out_slot_va_base || !out_basedata_length || !out_committed_pages) {
        return -1;
    }
    uint64_t linmem_virt = reinterpret_cast<uint64_t>(linmem_kernel_ptr);
    if (linmem_virt < WARP_LINMEM_VA_BASE) {
        return -1;
    }
    uint64_t slot = (linmem_virt - WARP_LINMEM_VA_BASE) / WARP_LINMEM_VA_STRIDE;
    if (slot >= linmem_slot_count()) {
        return -1;
    }
    uint64_t slot_va_base = WARP_LINMEM_VA_BASE + slot * WARP_LINMEM_VA_STRIDE;
    AllocHeader* hdr = reinterpret_cast<AllocHeader*>(static_cast<uintptr_t>(slot_va_base));
    if (hdr->is_pages != 2) {
        return -1;
    }
    uint64_t memory_base = slot_va_base + sizeof(AllocHeader);
    if (linmem_virt < memory_base) {
        return -1;
    }
    *out_slot_va_base = slot_va_base;
    *out_basedata_length = linmem_virt - memory_base;
    *out_committed_pages = hdr->pages;
    return 0;
}

int warp_heap_probe_growth(size_t size) {
    void* p = warp_kmalloc(size);
    if (!p)
        return -1;
    warp_kfree(p);
    return 0;
}

} // extern "C"
