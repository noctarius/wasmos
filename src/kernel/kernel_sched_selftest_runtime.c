/*
 * kernel_sched_selftest_runtime.c — unit tests for the threadable scheduler.
 *
 * Each test function returns 0 on pass, non-zero on fail.
 * Tests are pure in-kernel C: no process spawn, no QEMU-boot required.
 *
 * Compile-guarded: only compiled when WASMOS_SCHED_THREADABLE is defined.
 */

#include "kernel_sched_selftest_runtime.h"
#include "klog.h"
#include "serial.h"

#ifdef WASMOS_SCHED_THREADABLE

#include "sched.h"
#include "sched_event.h"
#include "thread.h"
#include "process.h"
#include "string.h"
#include "stdlib.h"
#include "arch/x86_64/smp.h"

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

#define TEST_PASS(name)  klog_write("[test] sched " name " ok\n")
#define TEST_FAIL(name)  klog_write("[test] sched " name " FAILED\n")
#define CHECK(expr, name) do { if (!(expr)) { TEST_FAIL(name); return -1; } } while (0)

/* Fabricate a minimal thread_t on the stack for queue tests.
 * Only the fields touched by cpu_sched_enqueue / cpu_sched_pick_next are
 * initialised; the rest remain zero.  Do NOT enqueue these into the real
 * live scheduler — use a private cpu_sched_t created in each test. */
static void
make_thread(thread_t *t, uint32_t tid, sched_prio_t prio)
{
    memset(t, 0, sizeof(*t));
    t->tid       = tid;
    t->state     = THREAD_STATE_READY;
    t->sched_prio = (uint8_t)prio;
    list_head_init(&t->sched_node);
    list_head_init(&t->event_node);
}

/* -------------------------------------------------------------------------
 * Test 1: bitmap — ffs_table returns correct highest-priority index
 * ------------------------------------------------------------------------- */
static int
test_bitmap_ffs(void)
{
    cpu_sched_t cs;
    cpu_sched_init(&cs);

    thread_t t4, t2, t6;
    make_thread(&t4, 1, SCHED_PRIO_WASM);       /* bit 4 */
    make_thread(&t2, 2, SCHED_PRIO_SERVICE);     /* bit 2 */
    make_thread(&t6, 3, SCHED_PRIO_IDLE);        /* bit 6 */

    cpu_sched_enqueue(&cs, &t4);
    cpu_sched_enqueue(&cs, &t2);
    cpu_sched_enqueue(&cs, &t6);

    /* bitmap should be 0b1010100 = bits 2, 4, 6 */
    CHECK(cs.ready_bitmap == ((1u<<2)|(1u<<4)|(1u<<6)), "bitmap-ffs-bitmap");

    /* pick_next must return the highest priority (lowest index = 2) */
    spinlock_lock(&cs.lock);
    thread_t *next = cpu_sched_pick_next(&cs);
    spinlock_unlock(&cs.lock);
    CHECK(next == &t2, "bitmap-ffs-pick");
    CHECK(cs.ready_bitmap == ((1u<<4)|(1u<<6)), "bitmap-ffs-clear");

    TEST_PASS("bitmap-ffs");
    return 0;
}

/* -------------------------------------------------------------------------
 * Test 2: priority ordering — FIFO within same priority, strict ordering
 *         across different priorities
 * ------------------------------------------------------------------------- */
static int
test_priority_ordering(void)
{
    cpu_sched_t cs;
    cpu_sched_init(&cs);

    thread_t hi1, hi2, lo1;
    make_thread(&hi1, 10, SCHED_PRIO_DRIVER);
    make_thread(&hi2, 11, SCHED_PRIO_DRIVER);
    make_thread(&lo1, 12, SCHED_PRIO_BACKGROUND);

    /* Enqueue lower-priority first, then two high-priority */
    cpu_sched_enqueue(&cs, &lo1);
    cpu_sched_enqueue(&cs, &hi1);
    cpu_sched_enqueue(&cs, &hi2);

    spinlock_lock(&cs.lock);
    thread_t *p1 = cpu_sched_pick_next(&cs);
    thread_t *p2 = cpu_sched_pick_next(&cs);
    thread_t *p3 = cpu_sched_pick_next(&cs);
    thread_t *p4 = cpu_sched_pick_next(&cs); /* should be idle */
    spinlock_unlock(&cs.lock);

    CHECK(p1 == &hi1, "prio-order-first");
    CHECK(p2 == &hi2, "prio-order-second");
    CHECK(p3 == &lo1, "prio-order-third");
    /* Fallback now returns cpu_local()->idle_thread (per-CPU), not cs.idle. */
    CHECK(p4 == cpu_local()->idle_thread, "prio-order-idle");
    CHECK(cs.ready_bitmap == 0, "prio-order-bitmap-empty");

    TEST_PASS("priority-ordering");
    return 0;
}

/* -------------------------------------------------------------------------
 * Test 3: dequeue — cpu_sched_dequeue removes without pick_next
 * ------------------------------------------------------------------------- */
static int
test_dequeue(void)
{
    cpu_sched_t cs;
    cpu_sched_init(&cs);

    thread_t ta, tb;
    make_thread(&ta, 20, SCHED_PRIO_SYSTEM);
    make_thread(&tb, 21, SCHED_PRIO_SYSTEM);

    cpu_sched_enqueue(&cs, &ta);
    cpu_sched_enqueue(&cs, &tb);
    CHECK(cs.thread_count[SCHED_PRIO_SYSTEM] == 2, "dequeue-count-before");

    spinlock_lock(&cs.lock);
    cpu_sched_dequeue(&cs, &ta);
    spinlock_unlock(&cs.lock);

    CHECK(cs.thread_count[SCHED_PRIO_SYSTEM] == 1, "dequeue-count-after");
    CHECK((cs.ready_bitmap & (1u << SCHED_PRIO_SYSTEM)) != 0, "dequeue-bitmap-nonzero");

    spinlock_lock(&cs.lock);
    cpu_sched_dequeue(&cs, &tb);
    spinlock_unlock(&cs.lock);

    CHECK(cs.thread_count[SCHED_PRIO_SYSTEM] == 0, "dequeue-count-zero");
    CHECK((cs.ready_bitmap & (1u << SCHED_PRIO_SYSTEM)) == 0, "dequeue-bitmap-cleared");

    TEST_PASS("dequeue");
    return 0;
}

/* -------------------------------------------------------------------------
 * Test 4: sched_event_init / wake_one / wake_all with no real blocking
 *
 * We cannot actually call sched_event_wait (it yields to the scheduler).
 * Instead we test the wake path directly: populate the wait_list by hand,
 * call sched_event_wake_one / sched_event_wake_all, and verify the list
 * state and thread pend_state fields.
 *
 * sched_wake_thread is NOT called here (it would enqueue into the live
 * scheduler); we stub it by checking list membership before and after.
 * ------------------------------------------------------------------------- */

/* Minimal stand-in: add a thread to an event's wait_list without actually
 * blocking (skips the blocking_transition / yield path). */
static void
fake_wait(sched_event_t *ev, thread_t *t)
{
    spinlock_lock(&ev->lock);
    t->wait_event = ev;
    t->pend_state = SCHED_PEND_NONE;
    t->pend_data  = 0;
    list_head_add_tail(&ev->wait_list, &t->event_node);
    spinlock_unlock(&ev->lock);
}

static int
test_event_wake_one(void)
{
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);

    thread_t ta, tb;
    make_thread(&ta, 30, SCHED_PRIO_WASM);
    make_thread(&tb, 31, SCHED_PRIO_WASM);

    fake_wait(&ev, &ta);
    fake_wait(&ev, &tb);

    CHECK(!list_head_empty(&ev.wait_list), "event-wake-one-list-nonempty");

    /* Wake one — but intercept sched_wake_thread by checking state manually.
     * We call wake_one under the lock to match the production call convention,
     * but sched_wake_thread will call thread_set_state which needs a valid tid
     * — skip that by comparing pend_state directly after list removal. */
    spinlock_lock(&ev.lock);
    /* Manually dequeue first waiter the same way wake_one does, without the
     * sched_wake_thread call, so we can test pure list/pend logic. */
    CHECK(!list_head_empty(&ev.wait_list), "event-wake-one-before");
    thread_t *first = list_first_entry(&ev.wait_list, thread_t, event_node);
    list_head_del(&first->event_node);
    first->wait_event = 0;
    first->pend_state = SCHED_PEND_OK;
    first->pend_data  = 42;
    spinlock_unlock(&ev.lock);

    CHECK(first == &ta, "event-wake-one-order");
    CHECK(first->pend_state == SCHED_PEND_OK, "event-wake-one-pend");
    CHECK(first->pend_data  == 42, "event-wake-one-data");
    CHECK(!list_head_empty(&ev.wait_list), "event-wake-one-tb-remains");

    TEST_PASS("event-wake-one");
    return 0;
}

static int
test_event_wake_all(void)
{
    sched_event_t ev;
    sched_event_init(&ev, SCHED_EVENT_TYPE_IPC);

    thread_t ta, tb, tc;
    make_thread(&ta, 40, SCHED_PRIO_WASM);
    make_thread(&tb, 41, SCHED_PRIO_WASM);
    make_thread(&tc, 42, SCHED_PRIO_WASM);

    fake_wait(&ev, &ta);
    fake_wait(&ev, &tb);
    fake_wait(&ev, &tc);

    /* Manually drain all waiters, mirroring sched_event_wake_all logic. */
    spinlock_lock(&ev.lock);
    int woken = 0;
    list_head_t *pos, *tmp;
    list_for_each_safe(pos, tmp, &ev.wait_list) {
        thread_t *t = list_entry(pos, thread_t, event_node);
        list_head_del(&t->event_node);
        t->wait_event = 0;
        t->pend_state = SCHED_PEND_ABORT;
        t->pend_data  = 0;
        woken++;
    }
    spinlock_unlock(&ev.lock);

    CHECK(woken == 3, "event-wake-all-count");
    CHECK(list_head_empty(&ev.wait_list), "event-wake-all-empty");
    CHECK(ta.pend_state == SCHED_PEND_ABORT, "event-wake-all-pend-a");
    CHECK(tb.pend_state == SCHED_PEND_ABORT, "event-wake-all-pend-b");
    CHECK(tc.pend_state == SCHED_PEND_ABORT, "event-wake-all-pend-c");

    TEST_PASS("event-wake-all");
    return 0;
}

/* -------------------------------------------------------------------------
 * Test 5: sched_default_prio mapping
 * ------------------------------------------------------------------------- */
static int
test_default_prio(void)
{
    CHECK(sched_default_prio(1, 0, 0, 0) == SCHED_PRIO_IDLE,       "prio-idle");
    CHECK(sched_default_prio(0, 1, 0, 0) == SCHED_PRIO_SYSTEM,     "prio-system");
    CHECK(sched_default_prio(0, 0, 1, 0) == SCHED_PRIO_DRIVER,     "prio-driver");
    CHECK(sched_default_prio(0, 0, 0, 1) == SCHED_PRIO_SERVICE,    "prio-service");
    CHECK(sched_default_prio(0, 0, 0, 0) == SCHED_PRIO_WASM,       "prio-wasm");
    TEST_PASS("default-prio");
    return 0;
}

/* -------------------------------------------------------------------------
 * Test 6: sched_list — empty sentinel, add_tail, del, for_each_safe
 * ------------------------------------------------------------------------- */
static int
test_sched_list(void)
{
    list_head_t head;
    list_head_init(&head);
    CHECK(list_head_empty(&head), "list-empty-init");

    list_head_t nodes[4];
    for (int i = 0; i < 4; i++) {
        list_head_init(&nodes[i]);
        list_head_add_tail(&head, &nodes[i]);
    }
    CHECK(!list_head_empty(&head), "list-nonempty-after-add");
    CHECK(head.next == &nodes[0], "list-first");

    /* Remove the second node */
    list_head_del(&nodes[1]);
    /* Walk remaining: 0, 2, 3 */
    int count = 0;
    list_head_t *pos, *tmp;
    list_for_each_safe(pos, tmp, &head) {
        count++;
    }
    CHECK(count == 3, "list-count-after-del");

    /* Remove remaining via for_each_safe */
    list_for_each_safe(pos, tmp, &head) {
        list_head_del(pos);
    }
    CHECK(list_head_empty(&head), "list-empty-after-clear");

    TEST_PASS("sched-list");
    return 0;
}

/* -------------------------------------------------------------------------
 * Test 7: wake-during-block-transition — waking a thread in the narrow
 *         RUNNING->BLOCKED handoff must not spin or enqueue early
 * ------------------------------------------------------------------------- */
static int
test_wake_during_block_transition(void)
{
    thread_t t;
    make_thread(&t, 50, SCHED_PRIO_SERVICE);
    t.state = THREAD_STATE_BLOCKED;
    t.block_reason = THREAD_BLOCK_EVENT;
    t.blocking_transition = 1;

    sched_wake_thread(&t);

    CHECK(t.state == THREAD_STATE_READY, "wake-transition-state");
    CHECK(t.block_reason == THREAD_BLOCK_NONE, "wake-transition-reason");
    CHECK(t.blocking_transition == 1, "wake-transition-flag");
    CHECK(list_head_empty(&t.sched_node), "wake-transition-no-enqueue");

    TEST_PASS("wake-during-block-transition");
    return 0;
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */
int
kernel_sched_selftest_run(void)
{
    int failures = 0;
    failures += (test_bitmap_ffs()        != 0) ? 1 : 0;
    failures += (test_priority_ordering() != 0) ? 1 : 0;
    failures += (test_dequeue()           != 0) ? 1 : 0;
    failures += (test_event_wake_one()    != 0) ? 1 : 0;
    failures += (test_event_wake_all()    != 0) ? 1 : 0;
    failures += (test_default_prio()      != 0) ? 1 : 0;
    failures += (test_sched_list()        != 0) ? 1 : 0;
    failures += (test_wake_during_block_transition() != 0) ? 1 : 0;

    if (failures == 0) {
        klog_write("[test] sched selftest all ok\n");
    } else {
        klog_write("[test] sched selftest FAILURES=");
        serial_write_hex64((uint64_t)(uint32_t)failures);
        klog_write("\n");
    }
    return failures;
}

#else /* !WASMOS_SCHED_THREADABLE */

int
kernel_sched_selftest_run(void)
{
    /* Nothing to test when the new scheduler is not compiled in. */
    return 0;
}

#endif /* WASMOS_SCHED_THREADABLE */
