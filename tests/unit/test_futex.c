/* test_futex.c — host tests for the REAL kernel futex (src/kernel/futex.c).
 *
 * The futex is the parking lot every userspace mutex, semaphore and condvar is
 * built on. A wrong bucket, a missed revalidation of the futex word, or a wake
 * that counts wrong shows up as a userspace hang with no kernel-side evidence
 * at all.
 *
 * The interesting part is the address key: the kernel keys on the PHYSICAL
 * address of the word, so two contexts sharing a page must converge on one
 * entry. The mm stub here places the fake "physical" base so that
 * paddr + KERNEL_HIGHER_HALF_BASE lands on a real host buffer, which is exactly
 * the translation futex_wait performs — the arithmetic under test is genuine,
 * only the memory behind it is host-allocated.
 *
 * MODELLING NOTE (blocking) is the same as test_ipc.c's: process_yield cannot
 * suspend a host thread, so a wait that parks returns at once. Tests that need
 * a thread to STAY parked set g_park, which leaves it linked in the wait list —
 * the state a real blocked thread is in between its yield and its waker.
 */

#include <stdio.h>

#include "test_shuffle.h"
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

/* Counts every evaluation into g_checks and, on failure, counts g_failures and prints
 * `msg` with the file and line. A failed check does NOT end the case: the remaining
 * assertions still run, against the state the failure left behind. main prints the
 * totals and exits non-zero if any check failed. `msg` names the property being
 * asserted, in the affirmative -- it is printed when that property does not hold. */
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("  [FAIL] %s (%s:%d)\n", (msg), __FILE__, __LINE__);                            \
        }                                                                                          \
    } while (0)

/* ------------------------------------------------------------------ stubs */

/* futex.c takes one allocation per distinct futex word, so overriding malloc is what
 * reaches its allocation-failure path: every call returns NULL while g_malloc_fail is
 * set. The override cannot call malloc itself, hence aligned_alloc with the size rounded
 * up to the alignment; free is left to the host libc, which releases that storage
 * normally. */
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

/* The clock every futex deadline is measured against. g_now moves only when a case moves
 * it, so nothing expires on its own; the kernel's timer_ticks is advanced by the timer
 * IRQ. The 1:1 millisecond-to-tick conversion replaces the kernel's
 * ceil(ms * hz / 1000), so a timeout in ms and a deadline in ticks are the same number
 * throughout this file. */
uint64_t timer_ticks(void) {
    return g_now;
}
uint64_t timer_ms_to_ticks(uint32_t ms) {
    return (uint64_t)ms;
}

/* The table sched_timeout_check sweeps for expired deadlines. Bounded by the fixture
 * pool rather than THREAD_MAX_COUNT, so the sweep sees NULL for every index past it;
 * like the kernel's it takes no lock and returns a slot whatever state that slot is in. */
thread_t* thread_table_at(uint32_t index) {
    return (index < POOL_MAX) ? &g_pool[index] : 0;
}

/* Resolves a tid straight to its pool slot, so tid n is g_pool[n-1] and every tid in
 * 1..POOL_MAX answers. The kernel searches the thread table under g_thread_table_lock
 * and refuses a slot whose state is THREAD_STATE_UNUSED; this applies no state filter,
 * so a slot the fixture has recycled is still reachable by tid. */
thread_t* thread_get(uint32_t tid) {
    if (tid == 0 || tid > POOL_MAX) {
        return 0;
    }
    return &g_pool[tid - 1u];
}

/* The identity a futex_wait parks under. Set directly by the fixture (park() swaps it
 * around a wait); the kernel derives it from cpu_local()->current_thread and answers 0
 * when a CPU is running nothing, which no case here produces. */
uint32_t thread_current_tid(void) {
    return g_current_tid;
}

/* Writes state and block reason unconditionally. The kernel takes the thread-table lock
 * and filters the edge through thread_transition_legal, so an illegal transition --
 * notably anything leaving ZOMBIE -- is silently ignored there; this stub enforces no
 * such rule and would resurrect a dead slot. No case here drives an illegal edge, so
 * what goes untested is the refusal, not the transitions the futex actually performs. */
void thread_set_state(uint32_t tid, thread_state_t state, thread_block_reason_t reason) {
    thread_t* t = thread_get(tid);
    if (t) {
        t->state = state;
        t->block_reason = reason;
    }
}

/* Marks the thread READY and nothing more. The kernel runs the wake/block claim
 * handshake, refuses a wake of a thread that is not BLOCKED, enqueues it on a run queue
 * and may request a reschedule -- none of which exists here. A wake is therefore
 * observed purely as the wait-list detach sched_event.c performs before calling this,
 * which is exactly what is_parked() reads. */
void sched_wake_thread(thread_t* t) {
    if (t) {
        t->state = THREAD_STATE_READY;
    }
}

/* MODELLING NOTE (blocking) -- see the file header. The kernel switches away here and
 * does not return until a waker resumes the thread, so sched_event_wait's post-resume
 * tail (which disarms sched_timeout_tick) runs only after the wake. This returns at
 * once, so that tail executes immediately.
 *
 * Before returning it fires a one-shot g_yield_hook if one is armed. That call happens
 * with the caller parked in the wait list and no futex or event lock held -- precisely
 * where a waker or the timeout scan on another CPU would run -- so a hook drives the
 * real handoff rather than a fabricated one, and it disarms itself so a later yield does
 * not re-run it.
 *
 * Then, unless g_park is set, it simulates the resume: under the event's lock it unlinks
 * the caller from the wait list, clears wait_event and marks the thread RUNNING. That is
 * the detach half of a wake, without the pend_state/pend_data delivery. With g_park set
 * it returns leaving the caller linked and still pointing at the event, which is the
 * state a genuinely blocked thread is in between its yield and its waker. */
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

/* Four contexts. CTX_B and CTX_SHARED are deliberately backed by the SAME fake
 * physical page (g_mem_shared) at different virtual bases — the shmem_grant
 * shape the physical-address key exists to handle. CTX_A gets a page of its own
 * and CTX_NO_REGION has no linear-memory region at all. */
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
 * phys_base that makes that land on the host buffer. */
static uint64_t fake_phys_of(const void* p) {
    return (uint64_t)((uintptr_t)p - (uintptr_t)KERNEL_HIGHER_HALF_BASE);
}

/* Resolves the four fixture context ids, and NULL for anything else -- which is how the
 * unknown-context rejection is driven. The kernel looks a live mm_context_t up by id in
 * a global list under a lock.
 *
 * The returned pointer addresses this file's `struct mm_context`, which shares no layout
 * with the real mm_context_t. That is safe only because its one consumer,
 * mm_context_region_for_type, is stubbed here too: futex.c treats the handle as opaque
 * and never dereferences it. */
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

/* Describes the fixture's linear-memory region: WORDS words, with phys_base chosen so
 * that futex.c's paddr + KERNEL_HIGHER_HALF_BASE lands on the host buffer backing that
 * context. Returns 0 when a region is produced, -1 for a NULL context, a type other than
 * MEM_REGION_WASM_LINEAR, or CTX_NO_REGION -- the same polarity as the kernel's, which
 * scans the context's region list for the first entry of that type.
 *
 * Unlike the kernel's it does not check `out` for NULL; futex.c always passes storage.
 * The region is described fresh on every call, so no case can mutate it. */
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

/* Per-case fixture reset, called first by every case. Drops every futex entry, rebuilds
 * the thread pool (tid i+1, RUNNING, both list nodes canonically detached -- a
 * zero-filled list_head_t reads as LINKED), zeroes both fake pages, and returns the
 * fixture to tid 1 at tick 100 with no hook armed, no park held and allocation
 * succeeding.
 *
 * g_checks and g_failures are cumulative across the run and are not touched. Neither is
 * futex.c's bucket table sizing or futex_init(), which main calls once. */
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

/* A thread counts as parked while it both points at an event and is linked into that
 * event's wait list. Testing only one of the two would misread a thread whose two fields
 * disagree -- which is exactly the corruption several cases here exist to catch. */
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

/* Two distinct words that hash to one bucket must stay separate entries on that
 * bucket's chain, not merge.
 *
 * futex_bucket is (paddr >> 12) & (FUTEX_TABLE_SIZE - 1), i.e. bits 12..15, so
 * addresses one page apart land in adjacent buckets and 16 pages apart land in
 * the SAME bucket. The test region is smaller than 16 pages, so the collision is
 * produced via the low bits instead: any two offsets inside one page share a
 * bucket while keeping distinct physical addresses. */
static void test_words_colliding_in_one_bucket_stay_separate(void) {
    reset();
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

    /* A timeout is IPC_ERR_TIMEOUT, and the value is the point: this return
     * crosses into WASM through the futex_wait hostcall, and a bare -1 collides
     * with IPC_ERR_INVALID -- leaving a guest unable to tell an expired deadline
     * from a bad address, which is the whole reason the error model forbids it. */
    CHECK(futex_wait(0u, 1u, 50u, CTX_A) == IPC_ERR_TIMEOUT,
          "a wait whose deadline expires reports TIMEOUT");
    CHECK(IPC_ERR_TIMEOUT != IPC_ERR_INVALID, "which is distinguishable from a bad address");
    CHECK(t->pend_state == SCHED_PEND_TIMEOUT, "the scheduler marked it as a timeout");
    CHECK(!is_parked(t), "and unlinked it from the futex wait list");
    CHECK(t->sched_timeout_tick == 0, "the deadline is disarmed on the way out");

    /* A wait released by a real wake must NOT be reported as a timeout. */
    reset();
    g_mem_a[0] = 1u;
    thread_t* w = thread_get(1);
    park(CTX_A, 0u, 1u, w, 50u);
    /* sched_event_wait disarms the deadline on resume, and the host stub's
     * immediate return from process_yield counts as one (see the MODELLING NOTE
     * at the top). Re-arm it so the wake below has something to cancel. */
    w->sched_timeout_tick = g_now + 50u;
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
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    const int test_count = (int)(sizeof(tests) / sizeof(tests[0]));
    int order[WASMOS_TEST_MAX_CASES];
    const uint64_t seed = wasmos_test_shuffle(order, test_count);

    for (int i = 0; i < test_count; ++i) {
        int before = g_failures;
        printf("  ... %s\n", tests[order[i]].name);
        fflush(stdout);
        tests[order[i]].fn();
        if (g_failures != before) {
            printf("[fail] %s\n", tests[order[i]].name);
        }
    }
    printf("test_futex: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        wasmos_test_report_seed(seed);
    }
    return g_failures == 0 ? 0 : 1;
}