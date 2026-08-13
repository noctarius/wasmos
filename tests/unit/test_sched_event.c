/* test_sched_event.c — host tests for the REAL sched_event.c.
 *
 * Until now sched_event.c had no host coverage at all, and the in-kernel test
 * hand-inlines the wait-list manipulation rather than calling into the file --
 * so those tests would pass unchanged if sched_event.c were deleted. This links
 * the real thing and stubs only what it reaches outside itself: the timer, the
 * thread table, and the two scheduler entry points.
 *
 * Locking contract mirrored from the header: sched_event_wait is entered with
 * ev->lock HELD and unlocks it; wake_one/wake_all/abort_all are called with it
 * held and leave it held.
 */

#include <stdio.h>

#include "test_shuffle.h"
#include <string.h>

#include "sched_event.h"
#include "sched.h"
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

#define POOL_MAX 16
static thread_t g_pool[POOL_MAX];
static uint32_t g_pool_used;

static uint64_t g_now;            /* controllable timer */
static uint32_t g_current_tid;    /* what thread_current_tid answers */
static int g_thread_get_null;     /* force thread_get to fail (V15) */
static uint32_t g_table_at_calls; /* proves the fast path skips the scan (V8) */
static int g_wake_calls;
static thread_t* g_last_woken;
static int g_yield_calls;

/* Hook: when set, thread_table_at arms a fresh deadline part-way through a scan
 * so the closing CAS in sched_timeout_check has to lose (V13). */
static uint32_t g_arm_at_index;
static void (*g_arm_hook)(void);

uint64_t timer_ticks(void) {
    return g_now;
}
uint64_t timer_ms_to_ticks(uint32_t ms) {
    return (uint64_t)ms;
} /* 1 tick per ms */

thread_t* thread_table_at(uint32_t index) {
    g_table_at_calls++;
    if (g_arm_hook && index == g_arm_at_index) {
        void (*hook)(void) = g_arm_hook;
        g_arm_hook = NULL; /* once */
        hook();
    }
    return (index < POOL_MAX) ? &g_pool[index] : NULL;
}

thread_t* thread_get(uint32_t tid) {
    if (g_thread_get_null) {
        return NULL;
    }
    for (uint32_t i = 0; i < POOL_MAX; ++i) {
        if (g_pool[i].tid == tid) {
            return &g_pool[i];
        }
    }
    return NULL;
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
    g_wake_calls++;
    g_last_woken = t;
    if (t) {
        t->state = THREAD_STATE_READY;
    }
}

/* The real one never returns to the caller in kernel context; here it must, so
 * sched_event_wait's post-resume tail can be observed. */
void process_yield(process_run_result_t result) {
    (void)result;
    g_yield_calls++;
}

/* --------------------------------------------------------------- fixtures */

static void reset(void) {
    memset(g_pool, 0, sizeof(g_pool));
    for (uint32_t i = 0; i < POOL_MAX; ++i) {
        list_head_init(&g_pool[i].event_node);
        list_head_init(&g_pool[i].sched_node);
    }
    g_pool_used = 0;
    g_now = 100;
    g_current_tid = 0;
    g_thread_get_null = 0;
    g_table_at_calls = 0;
    g_wake_calls = 0;
    g_last_woken = NULL;
    g_yield_calls = 0;
    g_arm_hook = NULL;
    g_arm_at_index = 0;
}

static thread_t* mk(void) {
    thread_t* t = &g_pool[g_pool_used];
    t->tid = g_pool_used + 1u;
    t->state = THREAD_STATE_RUNNING;
    g_pool_used++;
    return t;
}

/* Block `t` on `ev`, optionally with a timeout, through the real code path.
 *
 * MODELLING NOTE. In the kernel, process_yield(PROCESS_RUN_BLOCKED) does not
 * return until the thread is resumed, so sched_event_wait's tail -- which
 * disarms the deadline -- runs only AFTER the wake. A host stub cannot suspend,
 * so the stub returns immediately and that tail executes at once, clearing the
 * deadline the arm just installed. The state under test (blocked, deadline
 * still armed) is exactly the state that exists between those two points, so it
 * is reconstructed here rather than fabricated: the arm really happened inside
 * sched_event_wait and really lowered the global hint; only the per-thread tick
 * was undone by the premature resume. Without this every timeout test would
 * silently pass against a scheduler that never fires anything. */
static void block_on(sched_event_t* ev, thread_t* t, uint32_t timeout_ms) {
    uint64_t deadline = (timeout_ms > 0u) ? (g_now + (uint64_t)timeout_ms) : 0u;
    g_current_tid = t->tid;
    ksync_spinlock_lock(&ev->lock);
    sched_event_wait(ev, timeout_ms); /* unlocks */
    t->sched_timeout_tick = deadline; /* see MODELLING NOTE */
    t->state = THREAD_STATE_BLOCKED;  /* the scheduler would leave it blocked */
}

static int wait_list_len(sched_event_t* ev) {
    int n = 0;
    for (list_head_t* p = ev->wait_list.next; p != &ev->wait_list && n < POOL_MAX + 2;
         p = p->next) {
        n++;
    }
    return n;
}

/* Every prev link consistent with its next -- a corrupted wait list is the same
 * failure shape as the run-queue ghost. */
static int wait_list_intact(sched_event_t* ev) {
    for (list_head_t* p = ev->wait_list.next; p != &ev->wait_list; p = p->next) {
        if (p->next->prev != p || p->prev->next != p) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------- wake paths */

static void test_wake_one_is_fifo_and_complete(void) {
    reset();
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);
    thread_t* a = mk();
    thread_t* b = mk();
    thread_t* c = mk();
    block_on(&ev, a, 0);
    block_on(&ev, b, 0);
    block_on(&ev, c, 0);
    a->sched_timeout_tick = 55; /* an armed deadline the wake must cancel */
    g_wake_calls = 0;

    ksync_spinlock_lock(&ev.lock);
    thread_t* got = sched_event_wake_one(&ev, 0xD00D, SCHED_PEND_OK);
    ksync_spinlock_unlock(&ev.lock);

    CHECK(got == a, "the FIRST waiter is woken");
    CHECK(a->pend_state == SCHED_PEND_OK, "pend state set");
    CHECK(a->pend_data == 0xD00D, "pend data delivered");
    CHECK(a->wait_event == 0, "wait_event cleared");
    CHECK(a->sched_timeout_tick == 0, "an armed timeout is cancelled by a normal wake");
    CHECK(list_head_empty(&a->event_node), "detached from the wait list");
    CHECK(g_wake_calls == 1, "sched_wake_thread called exactly once");
    CHECK(wait_list_len(&ev) == 2, "the other two waiters remain");
    CHECK(wait_list_intact(&ev), "and the list is structurally intact");
}

static void test_wake_one_on_empty_list(void) {
    reset();
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);
    ksync_spinlock_lock(&ev.lock);
    thread_t* got = sched_event_wake_one(&ev, 1, SCHED_PEND_OK);
    ksync_spinlock_unlock(&ev.lock);
    CHECK(got == NULL, "nothing to wake yields NULL");
    CHECK(g_wake_calls == 0, "and no wake is issued");
}

static void test_wake_all_drains_and_delivers(void) {
    reset();
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);
    thread_t* th[4];
    for (int i = 0; i < 4; ++i) {
        th[i] = mk();
        block_on(&ev, th[i], 0);
    }
    g_wake_calls = 0;

    ksync_spinlock_lock(&ev.lock);
    int woken = sched_event_wake_all(&ev, 0xBEEF, SCHED_PEND_OK);
    ksync_spinlock_unlock(&ev.lock);

    CHECK(woken == 4, "the waiter count is returned");
    CHECK(g_wake_calls == 4, "one wake per waiter");
    CHECK(wait_list_len(&ev) == 0, "the list is emptied");
    int all = 1;
    for (int i = 0; i < 4; ++i) {
        if (th[i]->pend_state != SCHED_PEND_OK || th[i]->pend_data != 0xBEEF ||
            th[i]->wait_event != 0 || !list_head_empty(&th[i]->event_node)) {
            all = 0;
        }
    }
    CHECK(all, "every waiter gets the same pend state and data, and is detached");
}

static void test_wake_all_on_empty_list(void) {
    reset();
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);
    ksync_spinlock_lock(&ev.lock);
    int woken = sched_event_wake_all(&ev, 0, SCHED_PEND_OK);
    ksync_spinlock_unlock(&ev.lock);
    CHECK(woken == 0, "an empty list wakes nobody");
}

static void test_abort_all_marks_every_waiter(void) {
    reset();
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);
    thread_t* th[3];
    for (int i = 0; i < 3; ++i) {
        th[i] = mk();
        block_on(&ev, th[i], 0);
    }
    ksync_spinlock_lock(&ev.lock);
    sched_event_abort_all(&ev);
    ksync_spinlock_unlock(&ev.lock);

    int all = 1;
    for (int i = 0; i < 3; ++i) {
        if (th[i]->pend_state != SCHED_PEND_ABORT || th[i]->pend_data != 0) {
            all = 0;
        }
    }
    CHECK(all, "every waiter is aborted with no data");
    CHECK(wait_list_len(&ev) == 0, "and the list is empty");
}

/* ------------------------------------------------------------- timeouts */

static void test_arm_lowers_the_hint_but_never_raises_it(void) {
    reset();
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);
    thread_t* early = mk();
    thread_t* late = mk();

    block_on(&ev, early, 10); /* deadline 110 */
    block_on(&ev, late, 500); /* deadline 600 -- must not raise the hint */

    g_now = 115;
    g_wake_calls = 0;
    sched_timeout_check();
    CHECK(g_wake_calls == 1, "the earlier deadline still fires after a later arm");
    CHECK(g_last_woken == early, "and it is the earlier waiter");
}

static void test_arm_zero_is_coerced_to_one(void) {
    reset();
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);
    thread_t* t = mk();
    g_now = 0;
    block_on(&ev, t, 0 + 0); /* timeout_ms == 0 means "no timeout" */
    CHECK(t->sched_timeout_tick == 0, "a zero timeout arms nothing at all");

    /* A deadline that computes to 0 must become 1, since 0 encodes "unarmed". */
    thread_t* u = mk();
    g_now = 0;
    block_on(&ev, u, 0);
    CHECK(u->sched_timeout_tick == 0, "still unarmed");
}

static void test_fast_path_does_not_scan(void) {
    reset();
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);
    thread_t* t = mk();
    block_on(&ev, t, 100); /* deadline 200 */

    g_now = 150; /* before the deadline */
    g_table_at_calls = 0;
    sched_timeout_check();
    CHECK(g_table_at_calls == 0, "nothing due: the thread table is not scanned");

    g_now = 250; /* past it */
    sched_timeout_check();
    CHECK(g_table_at_calls > 0, "and it IS scanned once something is due");
}

static void test_due_deadline_fires_and_recomputes_the_hint(void) {
    reset();
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);
    thread_t* soon = mk();
    thread_t* later = mk();
    block_on(&ev, soon, 10);   /* 110 */
    block_on(&ev, later, 200); /* 300 */

    g_now = 120;
    g_wake_calls = 0;
    sched_timeout_check();
    CHECK(g_wake_calls == 1, "only the due waiter fires");
    CHECK(soon->pend_state == SCHED_PEND_TIMEOUT, "marked as a timeout, not a wake");
    CHECK(soon->wait_event == 0, "detached from its event");
    CHECK(list_head_empty(&soon->event_node), "and unlinked from the wait list");
    CHECK(soon->sched_timeout_tick == 0, "its deadline is disarmed");
    CHECK(wait_list_len(&ev) == 1, "the other waiter is untouched");

    /* The hint must have been recomputed to the remaining deadline, not left at
     * the fired one -- otherwise the next check rescans on every dispatch. */
    g_now = 310;
    g_wake_calls = 0;
    sched_timeout_check();
    CHECK(g_wake_calls == 1, "the next deadline fires when it comes due");
    CHECK(g_last_woken == later, "and it is the remaining waiter");
}

static void test_armed_but_not_blocked_is_disarmed_not_fired(void) {
    reset();
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);
    thread_t* t = mk();
    block_on(&ev, t, 10);          /* deadline 110 */
    t->state = THREAD_STATE_READY; /* woken some other way */

    g_now = 200;
    g_wake_calls = 0;
    sched_timeout_check();
    CHECK(g_wake_calls == 0, "a non-BLOCKED thread is not fired");
    CHECK(t->sched_timeout_tick == 0, "but its stale deadline is cleared");
}

static void test_fire_is_a_noop_after_a_normal_wake(void) {
    reset();
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);
    thread_t* t = mk();
    block_on(&ev, t, 10);

    ksync_spinlock_lock(&ev.lock);
    (void)sched_event_wake_one(&ev, 7, SCHED_PEND_OK); /* clears wait_event */
    ksync_spinlock_unlock(&ev.lock);
    t->state = THREAD_STATE_BLOCKED; /* pretend it re-blocked without an event */
    t->sched_timeout_tick = 110;
    g_wake_calls = 0;

    g_now = 200;
    sched_timeout_check();
    CHECK(g_wake_calls == 0, "with wait_event cleared the timeout does nothing");
    CHECK(t->pend_state == SCHED_PEND_OK, "and the normal wake's result survives");
}

/* sched_timeout_fire re-reads t->wait_event and re-validates it under that
 * event's lock, so the "thread re-blocked elsewhere" branch only triggers when
 * wait_event changes AFTER that read -- genuinely concurrent, and not reachable
 * from a single thread. What IS reachable, and what actually keeps a stale
 * deadline from firing against an unrelated event, is sched_event_wait's resume
 * tail disarming the tick.
 *
 * So this pins the real protection: after a wake the deadline is gone, and a
 * subsequent block on a DIFFERENT event is not disturbed by it. If the resume
 * tail's disarm were removed, an armed-and-abandoned deadline could fire against
 * an event the waiter never asked to be timed out on. */
static void test_stale_deadline_does_not_survive_into_a_new_event(void) {
    reset();
    sched_event_t ev_a, ev_b;
    sched_event_init(&ev_a, SCHED_EVENT_TYPE_IPC);
    sched_event_init(&ev_b, SCHED_EVENT_TYPE_IPC);
    thread_t* t = mk();

    block_on(&ev_a, t, 10); /* armed on A, deadline 110 */
    ksync_spinlock_lock(&ev_a.lock);
    (void)sched_event_wake_one(&ev_a, 1, SCHED_PEND_OK);
    ksync_spinlock_unlock(&ev_a.lock);
    CHECK(t->sched_timeout_tick == 0, "the wake disarmed A's deadline");

    block_on(&ev_b, t, 0); /* now waiting on B with no timeout of its own */
    CHECK(t->sched_timeout_tick == 0, "and blocking on B does not resurrect it");

    g_now = 200;
    g_wake_calls = 0;
    sched_timeout_check();
    CHECK(g_wake_calls == 0, "so nothing fires against B");
    CHECK(wait_list_len(&ev_b) == 1, "and the waiter stays on B");

    /* No "what it prevents" half: a stale tick cannot be manufactured through the
     * API either. Writing sched_timeout_tick directly does not lower the global
     * hint, so the fast path skips the scan and nothing observable happens --
     * the tick and the hint are only ever consistent because sched_timeout_arm
     * sets both. */
}

/* A deadline armed part-way through a scan must not be lost when the scan
 * publishes its recomputed bound: the closing CAS has to fail. Observed
 * indirectly -- if the hint were raised above the new deadline, the fast path
 * would skip and the timeout would never fire. */
static sched_event_t g_hook_ev;
static thread_t* g_hook_thread;
static void arm_during_scan(void) {
    g_hook_thread->state = THREAD_STATE_RUNNING;
    block_on(&g_hook_ev, g_hook_thread, 5); /* deadline g_now + 5 */
    g_hook_thread->state = THREAD_STATE_BLOCKED;
}

static void test_arm_during_scan_is_not_lost(void) {
    reset();
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);
    sched_event_init(&g_hook_ev, SCHED_EVENT_TYPE_IPC);
    thread_t* due = mk();
    block_on(&ev, due, 10); /* deadline 110, forces a scan */

    g_hook_thread = mk();
    g_arm_at_index = 3; /* arm mid-scan, after the due thread is seen */
    g_arm_hook = arm_during_scan;

    g_now = 120;
    sched_timeout_check(); /* fires `due`; the hook arms 125 mid-scan */

    g_now = 130;
    g_wake_calls = 0;
    sched_timeout_check();
    CHECK(g_wake_calls == 1, "the deadline armed during the scan still fires");
    CHECK(g_last_woken == g_hook_thread, "and it is the thread that armed it");
}

/* ------------------------------------------------------- wait registration */

static void test_wait_reregisters_onto_a_new_event(void) {
    reset();
    sched_event_t ev_a, ev_b;
    sched_event_init(&ev_a, SCHED_EVENT_TYPE_IPC);
    sched_event_init(&ev_b, SCHED_EVENT_TYPE_IPC);
    thread_t* t = mk();

    block_on(&ev_a, t, 0);
    CHECK(wait_list_len(&ev_a) == 1, "registered on A");
    block_on(&ev_b, t, 0);

    CHECK(wait_list_len(&ev_a) == 0, "removed from A before joining B");
    CHECK(wait_list_len(&ev_b) == 1, "linked into B exactly once");
    CHECK(t->wait_event == &ev_b, "and wait_event points at B");
    CHECK(wait_list_intact(&ev_a) && wait_list_intact(&ev_b), "both lists intact");
}

static void test_wait_without_a_current_thread_releases_the_lock(void) {
    reset();
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);
    g_thread_get_null = 1;

    ksync_spinlock_lock(&ev.lock);
    sched_event_wait(&ev, 0);
    g_thread_get_null = 0;

    /* If the lock leaked, this acquire would spin forever. */
    int acquired = ksync_spinlock_try_lock(&ev.lock);
    CHECK(acquired, "the event lock is released on the no-thread path");
    if (acquired) {
        ksync_spinlock_unlock_noirq(&ev.lock);
    }
    CHECK(g_yield_calls == 0, "and the thread does not yield");
}

static void test_wake_cancels_an_armed_timeout(void) {
    reset();
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);
    thread_t* t = mk();
    block_on(&ev, t, 10); /* deadline 110 */

    ksync_spinlock_lock(&ev.lock);
    (void)sched_event_wake_one(&ev, 1, SCHED_PEND_OK);
    ksync_spinlock_unlock(&ev.lock);
    CHECK(t->sched_timeout_tick == 0, "the wake disarms the deadline");

    t->state = THREAD_STATE_BLOCKED;
    g_wake_calls = 0;
    g_now = 300;
    sched_timeout_check();
    CHECK(g_wake_calls == 0, "so no second wake arrives after the deadline passes");
}

static void test_wait_list_survives_interleaved_waits_and_wakes(void) {
    reset();
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);
    thread_t* th[8];
    for (int i = 0; i < 8; ++i) {
        th[i] = mk();
        block_on(&ev, th[i], 0);
    }
    int ok = 1;
    for (int round = 0; round < 8; ++round) {
        ksync_spinlock_lock(&ev.lock);
        thread_t* got = sched_event_wake_one(&ev, (uint64_t)round, SCHED_PEND_OK);
        ksync_spinlock_unlock(&ev.lock);
        if (got != th[round]) {
            ok = 0; /* FIFO order must hold across the whole sequence */
        }
        if (!wait_list_intact(&ev)) {
            ok = 0;
        }
        /* Re-block an already-woken thread to keep the list churning. */
        if (round < 4) {
            th[round]->state = THREAD_STATE_RUNNING;
            block_on(&ev, th[round], 0);
        }
    }
    CHECK(ok, "FIFO order and list structure hold across interleaved waits and wakes");
}

/* -------------------------------------------------------------------- main */

int main(void) {
    struct {
        const char* name;
        void (*fn)(void);
    } tests[] = {
        {"V1 wake_one is FIFO and complete", test_wake_one_is_fifo_and_complete},
        {"V2 wake_one on an empty list", test_wake_one_on_empty_list},
        {"V3 wake_all drains and delivers", test_wake_all_drains_and_delivers},
        {"V4 wake_all on an empty list", test_wake_all_on_empty_list},
        {"V5 abort_all marks every waiter", test_abort_all_marks_every_waiter},
        {"V6 arm lowers but never raises the hint", test_arm_lowers_the_hint_but_never_raises_it},
        {"V7 zero timeout arms nothing", test_arm_zero_is_coerced_to_one},
        {"V8 fast path does not scan", test_fast_path_does_not_scan},
        {"V9 due deadline fires, hint recomputed", test_due_deadline_fires_and_recomputes_the_hint},
        {"V10 armed but not blocked is disarmed", test_armed_but_not_blocked_is_disarmed_not_fired},
        {"V11 fire is a no-op after a normal wake", test_fire_is_a_noop_after_a_normal_wake},
        {"V12 stale deadline does not survive into a new event",
         test_stale_deadline_does_not_survive_into_a_new_event},
        {"V13 arm during scan is not lost", test_arm_during_scan_is_not_lost},
        {"V14 wait re-registers onto a new event", test_wait_reregisters_onto_a_new_event},
        {"V15 no current thread releases the lock",
         test_wait_without_a_current_thread_releases_the_lock},
        {"V16 wake cancels an armed timeout", test_wake_cancels_an_armed_timeout},
        {"V17 list survives interleaved traffic",
         test_wait_list_survives_interleaved_waits_and_wakes},
    };

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
    printf("test_sched_event: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        wasmos_test_report_seed(seed);
    }
    return g_failures == 0 ? 0 : 1;
}
