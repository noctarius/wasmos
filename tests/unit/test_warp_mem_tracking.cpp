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
#include <sched.h>
#include <thread>
#include <vector>

#include "test_shuffle.h"

extern "C" {
#include "memory.h"
#include "warp_ring3.h"
}

#include "src/utils/MemUtils.hpp"

extern "C" int warp_mem_ring3_map_linmem(uint64_t user_root, uint8_t const* linmem_kernel_ptr);

namespace {

constexpr int kThreads = 4;
constexpr int kIterations = 20000;
constexpr uint64_t kPage = 4096u;

/* Frames come from a bump pointer well above both zone floors, so every
 * allocation has its own range and none is ever reused. */
std::atomic<uint64_t> g_next_phys{1ULL << 30};
thread_local uint64_t t_last_floor = 0;
std::atomic<int> g_untracked_logs{0};

} // namespace

extern "C" uint64_t pfa_alloc_pages_above(uint64_t pages, uint64_t min_addr) {
    t_last_floor = min_addr;
    return g_next_phys.fetch_add(pages * kPage);
}

extern "C" void pfa_free_pages(uint64_t, uint64_t) {}

extern "C" int paging_map_4k(uint64_t, uint64_t, uint64_t) {
    sched_yield();
    return 0;
}

extern "C" int paging_map_4k_in_root(uint64_t, uint64_t, uint64_t, uint64_t) {
    return 0;
}

extern "C" int paging_unmap_4k_in_root(uint64_t, uint64_t) {
    return 0;
}

extern "C" uint64_t paging_virt_to_phys(uint64_t) {
    return 0;
}

extern "C" void paging_sync_instruction_cache(const void*, uint64_t) {}

extern "C" void klog_write(const char* s) {
    if (s && std::strstr(s, "no tracked allocation")) {
        g_untracked_logs.fetch_add(1);
    }
}

extern "C" int warp_linmem_kernel_window_query(const uint8_t*, uint64_t*, uint64_t*, uint64_t*) {
    return -1;
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

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_concurrent_allocations_stay_resolvable),
        WASMOS_TEST_CASE(test_concurrent_allocations_keep_their_zone),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    std::printf("test_warp_mem_tracking: ok\n");
    return 0;
}
