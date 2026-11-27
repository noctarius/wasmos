/* test_warp_mem_tracking.cpp - the WARP allocation tracking table
 * (src/kernel/warp/mem_utils_kernel.cpp) under concurrent callers.
 *
 * Host threads stand in for CPUs that are inside WARP at the same time, which an
 * SMP build allows: warp_runtime_enter does not serialize them. The physical
 * frame allocator and the page-table writers are stubbed. The frame allocator
 * hands out distinct ranges and records the zone floor it was asked for, and
 * paging_map_4k yields the thread, standing in for the page-table work that
 * separates claiming a tracking slot from filling it on target. Pointers are
 * never dereferenced: the cases allocate, resolve and free, and assert on what
 * the table answers. */

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <vector>

#include "test_shuffle.h"

extern "C" {
#include "memory.h"
#include "paging.h"
#include "warp_ring3.h"
}

#include "src/utils/MemUtils.hpp"

extern "C" int warp_mem_ring3_map_linmem(uint64_t user_root, uint8_t const* linmem_kernel_ptr);
extern "C" void warp_mem_kmalloc_register(uint64_t phys, uint64_t pages, uint64_t data_offset);
extern "C" void warp_mem_kmalloc_unregister(uint64_t phys);

namespace {

constexpr int kThreads = 4;
constexpr int kIterations = 20000;
constexpr uint64_t kPage = 4096u;

/* Frames come from a bump pointer above both zone floors that wraps inside a
 * 1 GiB window, so no two allocations alive at once share a range and every
 * address stays below the 2 GiB the kernel's direct map covers. */
constexpr uint64_t kPhysBase = 1ULL << 28;
constexpr uint64_t kPhysWindow = 1ULL << 30;
std::atomic<uint64_t> g_next_phys{0};
thread_local uint64_t t_last_floor = 0;
std::atomic<int> g_untracked_logs{0};
std::atomic<int> g_alias_remaps{0};
std::atomic<int> g_frees{0};

/* Called from paging_map_4k_in_root when set: the hook a case uses to act at
 * the moment a publication is writing user page tables. */
void (*volatile g_map_in_root_hook)(uint64_t root) = nullptr;

} // namespace

extern "C" uint64_t pfa_alloc_pages_above(uint64_t pages, uint64_t min_addr) {
    t_last_floor = min_addr;
    return kPhysBase + (g_next_phys.fetch_add(pages * kPage) % kPhysWindow);
}

extern "C" void pfa_free_pages(uint64_t, uint64_t) {
    g_frees.fetch_add(1);
}

extern "C" int paging_map_4k(uint64_t, uint64_t, uint64_t) {
    g_alias_remaps.fetch_add(1);
    sched_yield();
    return 0;
}

extern "C" int paging_map_4k_in_root(uint64_t root, uint64_t, uint64_t, uint64_t) {
    if (g_map_in_root_hook) {
        g_map_in_root_hook(root);
    }
    return 0;
}

extern "C" int paging_unmap_4k_in_root(uint64_t, uint64_t) {
    return 0;
}

/* Every page of a dedicated linear-memory slot reads as committed. */
extern "C" uint64_t paging_virt_to_phys(uint64_t virt) {
    return kPhysBase + (virt & 0x3FFFF000ULL);
}

extern "C" void paging_sync_instruction_cache(const void*, uint64_t) {}

extern "C" void klog_write(const char* s) {
    if (s && std::strstr(s, "no tracked allocation")) {
        g_untracked_logs.fetch_add(1);
    }
}

/* A pointer into the dedicated-slot window resolves to its slot, with a 0x20-byte
 * header and four committed pages, as warp/shim.cpp reports a moved block. */
extern "C" int warp_linmem_kernel_window_query(const uint8_t* p, uint64_t* slot_va_base,
                                               uint64_t* basedata_length,
                                               uint64_t* committed_pages) {
    const uint64_t v = reinterpret_cast<uint64_t>(p);
    if (v < WARP_LINMEM_VA_BASE) {
        return -1;
    }
    const uint64_t base =
        WARP_LINMEM_VA_BASE +
        ((v - WARP_LINMEM_VA_BASE) / WARP_LINMEM_VA_STRIDE) * WARP_LINMEM_VA_STRIDE;
    *slot_va_base = base;
    *basedata_length = v - base - 0x20;
    *committed_pages = 4;
    return 0;
}

static void run_threads(void (*body)(int, std::atomic<int>*), std::atomic<int>* failures) {
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back(body, t, failures);
    }
    for (auto& th : threads) {
        th.join();
    }
}

static void linmem_publisher(int index, std::atomic<int>* failures) {
    const uint64_t root = 0x1000u * (uint64_t)(index + 1);
    for (int i = 0; i < kIterations; ++i) {
        auto* base = static_cast<uint8_t*>(vb::MemUtils::allocVirtualMemory(2 * kPage));
        if (!base) {
            failures->fetch_add(1);
            continue;
        }
        if (warp_mem_ring3_map_linmem(root, base + 16) != 0) {
            failures->fetch_add(1);
        }
        vb::MemUtils::freeVirtualMemory(base, 2 * kPage);
    }
}

/* Regression: 2026-11-27-warp-mmap-table-race -- the tracking table was claimed
 * by a scan for the first empty slot and filled afterwards, with no lock, so two
 * CPUs allocating at once could claim the same slot and the second fill erased
 * the first. The lost allocation then had no entry: publishing its linear memory
 * to ring 3 failed with "r3 linmem: no tracked allocation for the base" and the
 * module never started, which is the intermittent WARP SMP CI failure. */
static void test_concurrent_allocations_stay_resolvable(void) {
    std::atomic<int> failures{0};
    g_untracked_logs.store(0);
    run_threads(linmem_publisher, &failures);
    if (failures.load() != 0) {
        std::fprintf(stderr,
                     "concurrent linmem: %d of %d publications failed (%d untracked)\n",
                     failures.load(),
                     kThreads * kIterations,
                     g_untracked_logs.load());
        std::abort();
    }
}

static void zone_allocator(int index, std::atomic<int>* failures) {
    const bool jit = (index & 1) != 0;
    const uint64_t want = jit ? WARP_JIT_PHYS_MIN : WASMOS_BUFFER_PHYS_LIMIT;
    for (int i = 0; i < kIterations; ++i) {
        uint8_t* p = nullptr;
        if (jit) {
            p = vb::MemUtils::mapRXMemory(kPage, -1);
        } else {
            p = static_cast<uint8_t*>(vb::MemUtils::allocVirtualMemory(kPage));
        }
        if (t_last_floor != want) {
            failures->fetch_add(1);
        }
        vb::MemUtils::freePagedMemory(p, kPage);
    }
}

/* Regression: 2026-11-27-warp-alloc-zone-hint-race -- the zone of the next
 * allocation travelled in one global that each caller set, allocated through and
 * reset. A CPU allocating JIT code while another allocated linear memory could
 * read the other's value, placing JIT pages in the linear-memory zone, where a
 * linear-memory commit's zero-fill can overwrite compiled code, or linear memory
 * in the JIT zone. */
static void test_concurrent_allocations_keep_their_zone(void) {
    std::atomic<int> failures{0};
    run_threads(zone_allocator, &failures);
    if (failures.load() != 0) {
        std::fprintf(stderr,
                     "zone hint: %d of %d allocations used another caller's zone\n",
                     failures.load(),
                     kThreads * kIterations);
        std::abort();
    }
}

constexpr int kTableSlots = 256;

/* Review of PR #93 -- an allocation finding the tracking table full split and
 * remapped its direct-map alias before discovering there was no slot, so every
 * allocation past the 256th consumed page-table frames and left 2 MiB pages
 * split, then failed anyway. */
static void test_a_full_table_fails_before_touching_page_tables(void) {
    const uint64_t base = 0x7000000ULL;
    for (int i = 0; i < kTableSlots; ++i) {
        warp_mem_kmalloc_register(base + (uint64_t)i * kPage, 1, 0);
    }
    g_alias_remaps.store(0);
    bool threw = false;
    try {
        (void)vb::MemUtils::allocVirtualMemory(4 * kPage);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    const int remaps = g_alias_remaps.load();
    for (int i = 0; i < kTableSlots; ++i) {
        warp_mem_kmalloc_unregister(base + (uint64_t)i * kPage);
    }
    if (!threw || remaps != 0) {
        std::fprintf(stderr, "full table: threw=%d, %d direct-map pages remapped\n", threw, remaps);
        std::abort();
    }
}

std::atomic<int> g_free_state{0}; /* 0 idle, 1 requested, 2 done */
uint8_t* volatile g_publish_base = nullptr;
std::atomic<int> g_freed_while_mapping{0};

void free_on_first_map(uint64_t) {
    if (g_free_state.load() != 0) {
        return;
    }
    g_free_state.store(1);
    /* Give the freeing thread every chance to run to completion. */
    for (int i = 0; i < 2000 && g_free_state.load() != 2; ++i) {
        sched_yield();
    }
    if (g_free_state.load() == 2) {
        g_freed_while_mapping.fetch_add(1);
    }
}

void* freeing_thread(void*) {
    while (g_free_state.load() != 1) {
        sched_yield();
    }
    vb::MemUtils::freeVirtualMemory(g_publish_base, 4 * kPage);
    g_free_state.store(2);
    return nullptr;
}

/* Review of PR #93 -- publication copied a legacy block's frames out of the
 * tracking table and released the lock before writing them into the user page
 * tables. A free in that window returned the frames to the allocator while they
 * were being mapped user-writable, handing a later owner's memory to the guest. */
static void test_a_block_is_not_freed_while_it_is_published(void) {
    g_free_state.store(0);
    g_freed_while_mapping.store(0);
    g_publish_base = static_cast<uint8_t*>(vb::MemUtils::allocVirtualMemory(4 * kPage));
    pthread_t t;
    pthread_create(&t, nullptr, freeing_thread, nullptr);
    g_map_in_root_hook = free_on_first_map;
    (void)warp_mem_ring3_map_linmem(0xA000, g_publish_base + 16);
    g_map_in_root_hook = nullptr;
    if (g_free_state.load() == 1) {
        /* The free was held off until publication finished; let it complete. */
        g_free_state.store(1);
    }
    pthread_join(t, nullptr);
    if (g_freed_while_mapping.load() != 0) {
        std::fprintf(stderr, "publish: the block was freed while its frames were being mapped\n");
        std::abort();
    }
}

std::atomic<int> g_roots_inside{0};
std::atomic<int> g_saw_other_root{0};

void wait_for_other_root(uint64_t) {
    static thread_local bool done = false;
    if (done) {
        return;
    }
    done = true;
    g_roots_inside.fetch_add(1);
    for (int i = 0; i < 20000 && g_roots_inside.load() < 2; ++i) {
        sched_yield();
    }
    if (g_roots_inside.load() >= 2) {
        g_saw_other_root.fetch_add(1);
    }
}

void* publish_own_root(void* arg) {
    const uint64_t slot = reinterpret_cast<uint64_t>(arg);
    const auto* base = reinterpret_cast<const uint8_t*>(WARP_LINMEM_VA_BASE +
                                                        slot * WARP_LINMEM_VA_STRIDE + 0x120);
    (void)warp_mem_ring3_map_linmem(0x10000 * slot, base);
    return nullptr;
}

/* Review of PR #93 -- one lock serialised the publication of every root, held
 * across the whole unmap and map walk, so a large republish of one module's
 * slot-backed linear memory stalled every other module's publication on every
 * CPU. Slot-backed publications for different roots must be able to run at the
 * same time. */
static void test_different_roots_publish_concurrently(void) {
    g_roots_inside.store(0);
    g_saw_other_root.store(0);
    g_map_in_root_hook = wait_for_other_root;
    pthread_t a;
    pthread_t b;
    pthread_create(&a, nullptr, publish_own_root, reinterpret_cast<void*>(1));
    pthread_create(&b, nullptr, publish_own_root, reinterpret_cast<void*>(2));
    pthread_join(a, nullptr);
    pthread_join(b, nullptr);
    g_map_in_root_hook = nullptr;
    if (g_saw_other_root.load() != 2) {
        std::fprintf(stderr,
                     "publish: roots were serialised (%d of 2 saw the other)\n",
                     g_saw_other_root.load());
        std::abort();
    }
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_concurrent_allocations_stay_resolvable),
        WASMOS_TEST_CASE(test_concurrent_allocations_keep_their_zone),
        WASMOS_TEST_CASE(test_a_full_table_fails_before_touching_page_tables),
        WASMOS_TEST_CASE(test_a_block_is_not_freed_while_it_is_published),
        WASMOS_TEST_CASE(test_different_roots_publish_concurrently),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    std::printf("test_warp_mem_tracking: ok\n");
    return 0;
}
