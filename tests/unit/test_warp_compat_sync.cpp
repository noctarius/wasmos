/* test_warp_compat_sync.cpp - the synchronization the WARP backend's freestanding
 * C++ support provides: std::mutex from src/kernel/warp/compat/mutex, and the
 * function-local static guards from src/kernel/warp/cxa_guard.h.
 *
 * Host threads stand in for CPUs that are inside WARP at the same time, which an
 * SMP build allows: warp_runtime_enter (src/kernel/warp/shim.cpp) does not
 * serialize them. The kernel spinlock is the host stub from
 * stubs_spinlock.c. The header is included directly and the host <mutex> is not,
 * so std::mutex here is the one the kernel build compiles WARP against. */

#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <sched.h>

#include "test_shuffle.h"
#include "warp/compat/mutex"
#include "warp/cxa_guard.h"

/* The guard's per-thread bookkeeping for throw delivery is the exception
 * checkpoint's (src/kernel/warp/exception_checkpoint.c), covered by
 * test_warp_exception_checkpoint; here it records nothing. */
extern "C" void warp_exception_note_guard(unsigned long long*) {}
extern "C" void warp_exception_drop_guard(unsigned long long*) {}

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

constexpr int kGuardRounds = 2000;

/* One function-local static as the compiler lowers it: the guard word, the
 * object, and how many times its initializer ran. */
struct GuardedStatic {
    unsigned long long guard;
    volatile int value;
    volatile int initializations;
};

GuardedStatic g_statics[kGuardRounds];
volatile int g_start_round = -1;
volatile int g_arrived = 0;
volatile int g_incomplete_reads = 0;

/* The compiler's sequence for `static int v = init();`: the inline first-byte
 * test, then acquire, the initializer and release for the caller that wins. */
int use_static(GuardedStatic* s) {
    if (__atomic_load_n(reinterpret_cast<unsigned char*>(&s->guard), __ATOMIC_ACQUIRE) == 0) {
        if (warp_cxa_guard_acquire(&s->guard)) {
            __atomic_fetch_add(&s->initializations, 1, __ATOMIC_RELAXED);
            for (int i = 0; i < 200; ++i) {
                sched_yield();
            }
            s->value = 42;
            warp_cxa_guard_release(&s->guard);
        }
    }
    return s->value;
}

void* static_user(void*) {
    for (int round = 0; round < kGuardRounds; ++round) {
        __atomic_fetch_add(&g_arrived, 1, __ATOMIC_ACQ_REL);
        while (__atomic_load_n(&g_start_round, __ATOMIC_ACQUIRE) < round) {
        }
        if (use_static(&g_statics[round]) != 42) {
            __atomic_fetch_add(&g_incomplete_reads, 1, __ATOMIC_RELAXED);
        }
    }
    return nullptr;
}

} // namespace

/* Regression: 2026-11-27-warp-cxa-guard-race -- __cxa_guard_acquire tested and
 * set the guard with plain loads and stores and never waited, so two CPUs
 * reaching the same function-local static at once could both run its
 * initializer, and a CPU arriving while another was initializing was told to
 * initialize again instead of waiting for the value. WARP's function-local
 * statics could be constructed twice or read half-built. */
static void test_a_static_is_initialized_once_and_seen_complete(void) {
    g_start_round = -1;
    g_arrived = 0;
    g_incomplete_reads = 0;
    for (auto& s : g_statics) {
        s.guard = 0;
        s.value = 0;
        s.initializations = 0;
    }
    pthread_t threads[kThreads];
    for (auto& t : threads) {
        pthread_create(&t, nullptr, static_user, nullptr);
    }
    for (int round = 0; round < kGuardRounds; ++round) {
        while (__atomic_load_n(&g_arrived, __ATOMIC_ACQUIRE) < kThreads * (round + 1)) {
        }
        __atomic_store_n(&g_start_round, round, __ATOMIC_RELEASE);
    }
    for (auto& t : threads) {
        pthread_join(t, nullptr);
    }
    int repeated = 0;
    for (auto& s : g_statics) {
        if (s.initializations != 1) {
            ++repeated;
        }
    }
    if (repeated != 0 || g_incomplete_reads != 0) {
        std::fprintf(stderr,
                     "guard: %d of %d statics not initialized exactly once, %d reads "
                     "before initialization completed\n",
                     repeated,
                     kGuardRounds,
                     g_incomplete_reads);
        std::abort();
    }
}

static void test_an_aborted_initialization_is_retried(void) {
    unsigned long long guard = 0;
    if (!warp_cxa_guard_acquire(&guard)) {
        std::fprintf(stderr, "guard: first acquire refused\n");
        std::abort();
    }
    warp_cxa_guard_abort(&guard);
    if (!warp_cxa_guard_acquire(&guard)) {
        std::fprintf(stderr, "guard: acquire after abort refused\n");
        std::abort();
    }
    warp_cxa_guard_release(&guard);
    if (warp_cxa_guard_acquire(&guard)) {
        std::fprintf(stderr, "guard: acquire after release granted\n");
        std::abort();
    }
}

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
        WASMOS_TEST_CASE(test_a_static_is_initialized_once_and_seen_complete),
        WASMOS_TEST_CASE(test_an_aborted_initialization_is_retried),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    std::printf("test_warp_compat_sync: ok\n");
    return 0;
}
