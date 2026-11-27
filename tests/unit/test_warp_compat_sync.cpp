/* test_warp_compat_sync.cpp - the synchronization the WARP backend's freestanding
 * C++ support provides: std::mutex from src/kernel/warp/compat/mutex.
 *
 * Host threads stand in for CPUs that are inside WARP at the same time, which an
 * SMP build allows: warp_runtime_enter does not serialize them (FIXME(smp-warp) in
 * src/kernel/warp/shim.cpp). The kernel spinlock is the host stub from
 * stubs_spinlock.c. The header is included directly and the host <mutex> is not,
 * so std::mutex here is the one the kernel build compiles WARP against. */

#include <cstdio>
#include <cstdlib>
#include <pthread.h>

#include "test_shuffle.h"
#include "warp/compat/mutex"

namespace {

constexpr int kThreads = 4;
constexpr long kIterations = 200000;

std::mutex g_mutex;
volatile long g_counter = 0;

void* locked_incrementer(void*) {
    for (long i = 0; i < kIterations; ++i) {
        std::lock_guard<std::mutex> guard(g_mutex);
        long v = g_counter;
        g_counter = v + 1;
    }
    return nullptr;
}

void* unique_lock_incrementer(void*) {
    for (long i = 0; i < kIterations; ++i) {
        std::unique_lock<std::mutex> lock(g_mutex);
        long v = g_counter;
        g_counter = v + 1;
    }
    return nullptr;
}

void run_threads(void* (*body)(void*)) {
    pthread_t threads[kThreads];
    for (auto& t : threads) {
        pthread_create(&t, nullptr, body, nullptr);
    }
    for (auto& t : threads) {
        pthread_join(t, nullptr);
    }
}

void expect_counter(const char* what) {
    const long want = kThreads * kIterations;
    if (g_counter != want) {
        std::fprintf(stderr, "%s: counter %ld, want %ld\n", what, g_counter, want);
        std::abort();
    }
}

} // namespace

/* Regression: 2026-11-27-warp-compat-mutex-noop -- std::mutex::lock and unlock
 * were empty and try_lock always succeeded, so a std::lock_guard or
 * std::unique_lock in WARP excluded nothing. WasmModule guards the reallocation
 * of a module's linear memory with one; two CPUs inside WARP at once could both
 * run a section the lock was meant to make exclusive, and the failure is silent
 * corruption, not an error. */
static void test_lock_guard_excludes_other_cpus(void) {
    g_counter = 0;
    run_threads(locked_incrementer);
    expect_counter("lock_guard");
}

static void test_unique_lock_excludes_other_cpus(void) {
    g_counter = 0;
    run_threads(unique_lock_incrementer);
    expect_counter("unique_lock");
}

static void test_try_lock_fails_while_held(void) {
    std::mutex m;
    m.lock();
    if (m.try_lock()) {
        std::fprintf(stderr, "try_lock: acquired a held mutex\n");
        std::abort();
    }
    m.unlock();
    if (!m.try_lock()) {
        std::fprintf(stderr, "try_lock: failed on a free mutex\n");
        std::abort();
    }
    m.unlock();
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_lock_guard_excludes_other_cpus),
        WASMOS_TEST_CASE(test_unique_lock_excludes_other_cpus),
        WASMOS_TEST_CASE(test_try_lock_fails_while_held),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    std::printf("test_warp_compat_sync: ok\n");
    return 0;
}
