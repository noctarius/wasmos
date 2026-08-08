/* test_futex.c — host tests for the REAL kernel futex (src/kernel/futex.c).
 *
 * The futex is the parking lot every userspace mutex, semaphore and condvar is
 * built on, and it had no test of any kind. A wrong bucket, a missed
 * revalidation of the futex word, or a wake that counts wrong shows up as a
 * userspace hang with no kernel-side evidence at all.
 *
 * The interesting part is the address key: the kernel keys on the PHYSICAL
 * address of the word, so two contexts sharing a page must converge on one
 * entry. The mm stub here places the fake "physical" base so that
 * paddr + KERNEL_HIGHER_HALF_BASE lands on a real host buffer, which is exactly
 * the translation futex_wait performs — the arithmetic under test is genuine,
 * only the memory behind it is ours.
 *
 * MODELLING NOTE (blocking) is the same as test_ipc.c's: process_yield cannot
 * suspend a host thread, so a wait that parks returns at once. Tests that need
 * a thread to STAY parked set g_park, which leaves it linked in the wait list —
 * the state a real blocked thread is in between its yield and its waker.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "futex.h"
#include "ipc.h"
#include "memory.h"
#include "paging.h"
#include "sched.h"
#include "sched_event.h"
#include "thread.h"

static int g_failures;
static int g_checks;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("  [FAIL] %s (%s:%d)\n", (msg), __FILE__, __LINE__);                            \
        }                                                                                          \
    } while (0)

/* ------------------------------------------------------------------ stubs */

static int g_malloc_fail;

void* malloc(size_t n) {
    if (g_malloc_fail) {
        return 0;
    }
    return aligned_alloc(16u, (n + 15u) & ~(size_t)15u);
}

#define POOL_MAX 8
static thread_t g_pool[POOL_MAX];
static uint32_t g_current_tid;
static uint64_t g_now;
static int g_yield_calls;
static int g_park;
static void (*g_yield_hook)(void);

uint64_t timer_ticks(void) {
    return g_now;
}
uint64_t timer_ms_to_ticks(uint32_t ms) {
    return (uint64_t)ms;
}

thread_t* thread_table_at(uint32_t index) {
    return (index < POOL_MAX) ? &g_pool[index] : 0;
}

thread_t* thread_get(uint32_t tid) {
    if (tid == 0 || tid > POOL_MAX) {
        return 0;
    }
    return &g_pool[tid - 1u];
}

uint32_t thread_current_tid(void) {
    return g_current_tid;
}

void thread_set_state(uint32_t tid, thread_state_t state, thread_block_reason_t reason) {
    thread_t* t = thread_get(tid);
    if (t) {
        t->state = state;
        t->block_reason = reason;
    }
}

void sched_wake_thread(thread_t* t) {
    if (t) {
        t->state = THREAD_STATE_READY;
    }
}

void process_yield(process_run_result_t result) {
    (void)result;
    g_yield_calls++;
    thread_t* self = thread_get(g_current_tid);
    if (g_yield_hook) {
        void (*hook)(void) = g_yield_hook;
        g_yield_hook = 0;
        hook();
    }
    if (g_park) {
        return;
    }
    if (self) {
        sched_event_t* ev = self->wait_event;
        if (ev) {
            ksync_spinlock_lock(&ev->lock);
            if (self->wait_event == ev) {
                list_head_del(&self->event_node);
                self->wait_event = 0;
            }
            ksync_spinlock_unlock(&ev->lock);
        }
        self->state = THREAD_STATE_RUNNING;
    }
}

/* ------------------------------------------------------- memory-map stub */

/* Two contexts. CTX_SHARED_A and CTX_SHARED_B are deliberately backed by the
 * SAME fake physical page at different virtual bases — the shmem_grant shape
 * the physical-address key exists to handle. */
#define CTX_A 1u
#define CTX_B 2u
#define CTX_SHARED 3u
#define CTX_NO_REGION 4u

#define WORDS 512
static uint32_t g_mem_a[WORDS];
static uint32_t g_mem_shared[WORDS];

struct mm_context {
    uint32_t id;
};
static struct mm_context g_ctx_a = {CTX_A};
static struct mm_context g_ctx_b = {CTX_B};
static struct mm_context g_ctx_shared = {CTX_SHARED};
static struct mm_context g_ctx_no_region = {CTX_NO_REGION};

/* futex.c reads the word at paddr + KERNEL_HIGHER_HALF_BASE, so hand back a
 * phys_base that makes that land on our buffer. */
static uint64_t fake_phys_of(const void* p) {
    return (uint64_t)((uintptr_t)p - (uintptr_t)KERNEL_HIGHER_HALF_BASE);
}

mm_context_t* mm_context_get(uint32_t id) {
    switch (id) {
    case CTX_A:
        return (mm_context_t*)&g_ctx_a;
    case CTX_B:
        return (mm_context_t*)&g_ctx_b;
    case CTX_SHARED:
        return (mm_context_t*)&g_ctx_shared;
    case CTX_NO_REGION:
        return (mm_context_t*)&g_ctx_no_region;
    default:
        return 0;
    }
}

int mm_context_region_for_type(mm_context_t* ctx, mem_region_type_t type, mem_region_t* out) {
    struct mm_context* c = (struct mm_context*)ctx;
    if (!c || type != MEM_REGION_WASM_LINEAR || c->id == CTX_NO_REGION) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->type = MEM_REGION_WASM_LINEAR;
    out->size = sizeof(uint32_t) * WORDS;
    /* A and B are private; SHARED aliases B's page at a different base — the
     * point being that only phys_base decides the futex key. */
    out->phys_base = (c->id == CTX_A) ? fake_phys_of(g_mem_a) : fake_phys_of(g_mem_shared);
    out->base = (c->id == CTX_SHARED) ? 0x40000000ull : 0x10000000ull;
    return 0;
}

/* --------------------------------------------------------------- fixtures */

static void reset(void) {
    /* Drop every futex entry BEFORE recycling the thread pool: an entry left
     * holding a wait_list would otherwise point at nodes this memset is about
     * to re-initialise, and the next wake would walk a corrupted list. */
    futex_test_reset();
    memset(g_pool, 0, sizeof(g_pool));
    for (uint32_t i = 0; i < POOL_MAX; ++i) {
        g_pool[i].tid = i + 1u;
        g_pool[i].state = THREAD_STATE_RUNNING;
        list_head_init(&g_pool[i].event_node);
        list_head_init(&g_pool[i].sched_node);
    }
    memset(g_mem_a, 0, sizeof(g_mem_a));
    memset(g_mem_shared, 0, sizeof(g_mem_shared));
    g_current_tid = 1;
    g_now = 100;
    g_yield_calls = 0;
    g_park = 0;
    g_yield_hook = 0;
    g_malloc_fail = 0;
}

/* Park `t` on the futex word at `uaddr` and leave it parked. */
static void park(uint32_t ctx, uint32_t uaddr, uint32_t expected, thread_t* t, uint32_t timeout) {
    uint32_t saved = g_current_tid;
    g_current_tid = t->tid;
    g_park = 1;
    (void)futex_wait(uaddr, expected, timeout, ctx);
    g_park = 0;
    t->state = THREAD_STATE_BLOCKED;
    g_current_tid = saved;
}

static int is_parked(thread_t* t) {
    return t->wait_event != 0 && !list_head_empty(&t->event_node);
}

/* ------------------------------------------------------------------ tests */

static void test_a_changed_word_does_not_park(void) {
    reset();
    g_mem_a[4] = 7u; /* uaddr 16 */
    CHECK(futex_wait(16u, 999u, 0u, CTX_A) == 0,
          "waiting on a word that no longer holds the expected value returns at once");
    CHECK(g_yield_calls == 0, "and never blocks — this is the whole lost-wakeup guard");
    CHECK(!is_parked(thread_get(1)), "no waiter is registered");
}

static void test_a_matching_word_parks(void) {
    reset();
    g_mem_a[4] = 7u;
    thread_t* t = thread_get(1);
    park(CTX_A, 16u, 7u, t, 0u);
    CHECK(g_yield_calls == 1, "a matching word blocks the caller");
    CHECK(is_parked(t), "and registers it as a waiter");
    CHECK(futex_wake(16u, 1u, CTX_A) == 1, "the wake finds it");
    CHECK(!is_parked(t), "and unparks it");
}

static void test_wake_releases_exactly_count_waiters_in_fifo_order(void) {
    reset();
    g_mem_a[0] = 1u;
    thread_t* t[4] = {thread_get(1), thread_get(2), thread_get(3), thread_get(4)};
    for (int i = 0; i < 4; ++i) {
        park(CTX_A, 0u, 1u, t[i], 0u);
    }
    CHECK(is_parked(t[0]) && is_parked(t[1]) && is_parked(t[2]) && is_parked(t[3]),
          "four waiters are parked on one word");

    CHECK(futex_wake(0u, 2u, CTX_A) == 2, "waking two reports two");
    CHECK(!is_parked(t[0]) && !is_parked(t[1]), "the two that waited longest are released");
    CHECK(is_parked(t[2]) && is_parked(t[3]), "the rest stay parked — FIFO, not arbitrary");

    CHECK(futex_wake(0u, 10u, CTX_A) == 2, "asking for more than are waiting wakes what there is");
    CHECK(!is_parked(t[2]) && !is_parked(t[3]), "and drains the list");
    CHECK(futex_wake(0u, 1u, CTX_A) == 0, "a wake with nobody waiting reports zero");
}

static void test_wake_with_a_zero_count_wakes_nobody(void) {
    reset();
    g_mem_a[0] = 1u;
    thread_t* t = thread_get(1);
    park(CTX_A, 0u, 1u, t, 0u);
    CHECK(futex_wake(0u, 0u, CTX_A) == 0, "a zero-count wake reports zero");
    CHECK(is_parked(t), "and leaves the waiter alone");
}

static void test_distinct_words_are_independent(void) {
    reset();
    g_mem_a[0] = 1u;
    g_mem_a[1] = 1u;
    thread_t* a = thread_get(1);
    thread_t* b = thread_get(2);
    park(CTX_A, 0u, 1u, a, 0u);
    park(CTX_A, 4u, 1u, b, 0u);

    CHECK(futex_wake(0u, 10u, CTX_A) == 1, "waking one word reaches only its own waiter");
    CHECK(!is_parked(a) && is_parked(b), "the other word's waiter is untouched");
    CHECK(futex_wake(4u, 10u, CTX_A) == 1, "and the other word still works");
}

/* Two words 16 buckets apart collide in the 16-bucket table. They must stay
 * separate entries on the bucket chain, not merge. */
static void test_words_colliding_in_one_bucket_stay_separate(void) {
    reset();
    /* futex_bucket is (paddr >> 12) & 15, so two addresses one page apart land
     * in adjacent buckets and 16 pages apart land in the SAME bucket. Our
     * region is smaller than that, so collide via the low bits instead: any
     * two offsets inside one page share a bucket. */
    g_mem_a[0] = 1u;
    g_mem_a[2] = 1u;
    thread_t* a = thread_get(1);
    thread_t* b = thread_get(2);
    park(CTX_A, 0u, 1u, a, 0u);
    park(CTX_A, 8u, 1u, b, 0u);
    CHECK(is_parked(a) && is_parked(b), "both are parked");
    CHECK(futex_wake(0u, 10u, CTX_A) == 1,
          "a wake on one address in a shared bucket wakes only that address");
    CHECK(is_parked(b), "the bucket neighbour is untouched");
    CHECK(futex_wake(8u, 10u, CTX_A) == 1, "and is still reachable by its own address");
}

/* The reason the key is physical: two contexts mapping the same page at
 * different virtual bases must share one parking lot, or a shared-memory mutex
 * never wakes its peer. */
static void test_two_contexts_sharing_a_page_share_the_futex(void) {
    reset();
    g_mem_shared[0] = 5u;
    thread_t* t = thread_get(1);
    park(CTX_B, 0u, 5u, t, 0u);
    CHECK(is_parked(t), "a waiter parks from one context");
    CHECK(futex_wake(0u, 1u, CTX_SHARED) == 1,
          "a wake from the other context mapping the same page reaches it");
    CHECK(!is_parked(t), "the waiter really was released");

    /* And a context backed by a different page must NOT reach it. */
    reset();
    g_mem_shared[0] = 5u;
    t = thread_get(1);
    park(CTX_B, 0u, 5u, t, 0u);
    CHECK(futex_wake(0u, 1u, CTX_A) == 0, "an unrelated context's page is a different futex");
    CHECK(is_parked(t), "so the waiter stays parked");
}

static void test_addresses_outside_the_region_are_rejected(void) {
    reset();
    const uint32_t region_bytes = (uint32_t)(sizeof(uint32_t) * WORDS);
    CHECK(futex_wait(region_bytes, 0u, 0u, CTX_A) == IPC_ERR_INVALID,
          "a word starting at the end of the region is refused");
    CHECK(futex_wait(region_bytes - 3u, 0u, 0u, CTX_A) == IPC_ERR_INVALID,
          "so is a word that would straddle the end — the bound covers the whole word");
    CHECK(futex_wait(0xFFFFFFFCu, 0u, 0u, CTX_A) == IPC_ERR_INVALID,
          "and an offset whose word wraps the 32-bit range");
    /* The last fully-contained word IS in range: prove it by giving it a value
     * the wait does not expect, so it returns without parking. */
    g_mem_a[WORDS - 1] = 0xABCDu;
    CHECK(futex_wait(region_bytes - 4u, 0u, 0u, CTX_A) == 0,
          "the last fully-contained word is accepted");
    CHECK(g_yield_calls == 0, "neither the rejects nor the accepted probe blocked");

    CHECK(futex_wake(region_bytes, 1u, CTX_A) == 0, "wake on an out-of-range address is a no-op");
}

static void test_an_unknown_context_is_rejected(void) {
    reset();
    CHECK(futex_wait(0u, 0u, 0u, 0xDEADu) == IPC_ERR_INVALID, "an unknown context cannot wait");
    CHECK(futex_wake(0u, 1u, 0xDEADu) == 0, "and cannot wake");
    CHECK(futex_wait(0u, 0u, 0u, CTX_NO_REGION) == IPC_ERR_INVALID,
          "a context with no linear-memory region cannot wait");
    CHECK(futex_wake(0u, 1u, CTX_NO_REGION) == 0, "and cannot wake");
    CHECK(g_yield_calls == 0, "no rejection blocked");
}

static void test_allocation_failure_is_reported(void) {
    reset();
    g_mem_a[0] = 1u;
    g_malloc_fail = 1;
    int rc = futex_wait(0u, 1u, 0u, CTX_A);
    g_malloc_fail = 0;
    CHECK(rc == IPC_ERR_FULL,
          "a futex entry that cannot be allocated is reported, not silently skipped");
    CHECK(g_yield_calls == 0, "and the caller is not parked on a nonexistent entry");
    CHECK(futex_wake(0u, 1u, CTX_A) == 0, "no entry was left behind");
}

static void test_a_second_wait_reuses_the_same_entry(void) {
    reset();
    g_mem_a[0] = 1u;
    thread_t* a = thread_get(1);
    thread_t* b = thread_get(2);
    park(CTX_A, 0u, 1u, a, 0u);
    /* With the entry now allocated, a second waiter must find it rather than
     * allocate a second one — otherwise one wake could never reach both. */
    g_malloc_fail = 1;
    park(CTX_A, 0u, 1u, b, 0u);
    g_malloc_fail = 0;
    CHECK(is_parked(b), "the second waiter parked without allocating anything");
    CHECK(futex_wake(0u, 2u, CTX_A) == 2, "one wake reaches both, so they share one entry");
}

/* Fires the deadline from inside the yield, which is where the scheduler would
 * run it: the thread is genuinely BLOCKED with its deadline armed at that
 * point, so sched_timeout_check does the real work. */
static void hook_expire_deadline(void) {
    g_now += 1000u;
    sched_timeout_check();
}

static void test_a_timed_wait_reports_the_timeout(void) {
    reset();
    g_mem_a[0] = 1u;
    thread_t* t = thread_get(1);
    g_current_tid = t->tid;
    g_yield_hook = hook_expire_deadline;

    /* A bare -1, not a packed error code, is what a timed-out wait returns.
     * FIXME: this crosses a subsystem boundary -- it reaches WASM through the
     * futex_wait hostcall -- so per the project error model it should be a
     * generated code from abi/errors.yaml. Pinned here so a change is
     * deliberate rather than accidental. */
    CHECK(futex_wait(0u, 1u, 50u, CTX_A) == -1, "a wait whose deadline expires returns -1");
    CHECK(t->pend_state == SCHED_PEND_TIMEOUT, "the scheduler marked it as a timeout");
    CHECK(!is_parked(t), "and unlinked it from the futex wait list");
    CHECK(t->sched_timeout_tick == 0, "the deadline is disarmed on the way out");

    /* A wait released by a real wake must NOT be reported as a timeout. */
    reset();
    g_mem_a[0] = 1u;
    thread_t* w = thread_get(1);
    park(CTX_A, 0u, 1u, w, 50u);
    w->sched_timeout_tick = g_now + 50u; /* see the MODELLING NOTE at the top */
    CHECK(futex_wake(0u, 1u, CTX_A) == 1, "an explicit wake releases it");
    CHECK(w->pend_state == SCHED_PEND_OK, "as a delivery, not a timeout");
    CHECK(w->sched_timeout_tick == 0, "and cancels its armed deadline");
}

static void test_a_zero_timeout_means_forever(void) {
    reset();
    g_mem_a[0] = 1u;
    thread_t* t = thread_get(1);
    park(CTX_A, 0u, 1u, t, 0u);
    CHECK(t->sched_timeout_tick == 0, "an untimed wait arms no deadline");
    g_now += 100000u;
    sched_timeout_check();
    CHECK(is_parked(t), "so no amount of time releases it");
    CHECK(futex_wake(0u, 1u, CTX_A) == 1, "only an explicit wake does");
}

/* -------------------------------------------------------------------- main */

int main(void) {
    struct {
        const char* name;
        void (*fn)(void);
    } tests[] = {
        {"F1 a changed word does not park", test_a_changed_word_does_not_park},
        {"F2 a matching word parks", test_a_matching_word_parks},
        {"F3 wake releases exactly count, FIFO",
         test_wake_releases_exactly_count_waiters_in_fifo_order},
        {"F4 a zero-count wake wakes nobody", test_wake_with_a_zero_count_wakes_nobody},
        {"F5 distinct words are independent", test_distinct_words_are_independent},
        {"F6 bucket collisions stay separate", test_words_colliding_in_one_bucket_stay_separate},
        {"F7 two contexts sharing a page share the futex",
         test_two_contexts_sharing_a_page_share_the_futex},
        {"F8 out-of-region addresses are rejected", test_addresses_outside_the_region_are_rejected},
        {"F9 an unknown context is rejected", test_an_unknown_context_is_rejected},
        {"F10 allocation failure is reported", test_allocation_failure_is_reported},
        {"F11 a second wait reuses the entry", test_a_second_wait_reuses_the_same_entry},
        {"F12 a timed wait reports the timeout", test_a_timed_wait_reports_the_timeout},
        {"F13 a zero timeout means forever", test_a_zero_timeout_means_forever},
    };

    futex_init();
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        int before = g_failures;
        printf("  ... %s\n", tests[i].name);
        fflush(stdout);
        tests[i].fn();
        if (g_failures != before) {
            printf("[fail] %s\n", tests[i].name);
        }
    }
    printf("test_futex: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}