/* test_poll.c — host tests for the REAL poll hub (src/kernel/poll.c).
 *
 * poll.c is the push side of select: every ipc_send_from walks it, and a bug
 * here is either a lost wakeup (a service sleeps forever on traffic that did
 * arrive) or a walk over freed watcher nodes.
 *
 * The file's only outward call is ipc_select_signal, stubbed here to record
 * (set, endpoint) pairs — so what is asserted is exactly which sets a notify
 * reaches, in which order.
 *
 * Locking: poll.c takes no locks. Every caller in ipc.c holds ep->lock across
 * the add/remove/notify/free, which is what makes that safe; this test drives
 * it single-threaded, matching that contract.
 */

#include <stdio.h>

#include "test_shuffle.h"
#include <stdlib.h>
#include <string.h>

#include "poll.h"

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

/* poll.c takes both the hub and every watcher node from malloc, so overriding
 * malloc is what reaches its allocation-failure paths. The override cannot call
 * malloc itself, hence aligned_alloc; free is left to the host libc, which
 * releases aligned_alloc storage normally. */
static int g_malloc_fail;

void* malloc(size_t n) {
    if (g_malloc_fail) {
        return 0;
    }
    return aligned_alloc(16u, (n + 15u) & ~(size_t)15u);
}

#define SIGNAL_MAX 32
static struct {
    struct ipc_select* sel;
    uint32_t ep_id;
} g_signals[SIGNAL_MAX];
static int g_signal_count;

/* Records the notification instead of delivering it. The kernel's version takes the
 * select set's event lock, publishes ready_ep and wakes one waiter; here the
 * (set, endpoint) pair is appended to g_signals in call order, which is what lets a case
 * assert exactly which sets a notify reached and in which order. `sel` is never
 * dereferenced -- poll.c only stores and compares the pointer -- so the stand-in sets
 * need no real ipc_select_t behind them.
 *
 * g_signal_count counts every call, but only the first SIGNAL_MAX pairs are stored:
 * past that the count keeps rising while signalled() stops seeing new entries. */
void ipc_select_signal(struct ipc_select* sel, uint32_t ep_id) {
    if (g_signal_count < SIGNAL_MAX) {
        g_signals[g_signal_count].sel = sel;
        g_signals[g_signal_count].ep_id = ep_id;
    }
    g_signal_count++;
}

/* Stand-ins for select sets: poll.c only ever stores and compares the pointer,
 * so the pointed-to type is irrelevant to it. */
static int g_set_a, g_set_b, g_set_c, g_set_d;
#define SET_A ((struct ipc_select*)&g_set_a)
#define SET_B ((struct ipc_select*)&g_set_b)
#define SET_C ((struct ipc_select*)&g_set_c)
#define SET_D ((struct ipc_select*)&g_set_d)

/* Clears the recorded notifications and re-enables allocation. Called at the start of a
 * case, and again mid-case wherever the counts are to be read fresh after a removal. It
 * owns no poll_struct_t -- each case allocates and frees its own -- and leaves g_checks
 * and g_failures cumulative across the whole run. */
static void reset(void) {
    memset(g_signals, 0, sizeof(g_signals));
    g_signal_count = 0;
    g_malloc_fail = 0;
}

/* How many recorded notifications named `sel` since the last reset(). Counts entries,
 * not distinct notifies, so a set registered twice on one event answers 2 for a single
 * poll_notify; and it saturates once more than SIGNAL_MAX signals have been issued. */
static int signalled(struct ipc_select* sel) {
    int n = 0;
    for (int i = 0; i < g_signal_count && i < SIGNAL_MAX; ++i) {
        if (g_signals[i].sel == sel) {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ tests */

static void test_alloc_starts_empty(void) {
    reset();
    poll_struct_t* ps = poll_struct_alloc();
    CHECK(ps != 0, "allocation succeeds");
    int empty = 1;
    for (int ev = 0; ev < POLL_EV_MAX; ++ev) {
        if (ps->watchers[ev] != 0) {
            empty = 0;
        }
    }
    CHECK(empty, "a fresh poll struct has no watchers on any event");
    poll_notify(ps, POLL_EV_IN, 7u);
    CHECK(g_signal_count == 0, "notifying an empty struct signals nobody");
    poll_struct_free(ps);
}

static void test_alloc_reports_failure(void) {
    reset();
    g_malloc_fail = 1;
    poll_struct_t* ps = poll_struct_alloc();
    g_malloc_fail = 0;
    CHECK(ps == 0, "allocation failure returns NULL rather than a partially built struct");
}

static void test_notify_reaches_every_watcher_on_that_event(void) {
    reset();
    poll_struct_t* ps = poll_struct_alloc();
    CHECK(poll_struct_add(ps, POLL_EV_IN, SET_A, 0) == 0, "add A");
    CHECK(poll_struct_add(ps, POLL_EV_IN, SET_B, 0) == 0, "add B");
    CHECK(poll_struct_add(ps, POLL_EV_IN, SET_C, 0) == 0, "add C");

    poll_notify(ps, POLL_EV_IN, 42u);
    CHECK(g_signal_count == 3, "all three watchers are signalled");
    CHECK(signalled(SET_A) == 1 && signalled(SET_B) == 1 && signalled(SET_C) == 1,
          "each exactly once");
    int right_ep = 1;
    for (int i = 0; i < g_signal_count; ++i) {
        if (g_signals[i].ep_id != 42u) {
            right_ep = 0;
        }
    }
    CHECK(right_ep, "each is told which endpoint became ready");

    poll_struct_free(ps);
}

static void test_event_types_are_independent(void) {
    reset();
    poll_struct_t* ps = poll_struct_alloc();
    CHECK(poll_struct_add(ps, POLL_EV_IN, SET_A, 0) == 0, "A watches IN");
    CHECK(poll_struct_add(ps, POLL_EV_OUT, SET_B, 0) == 0, "B watches OUT");
    CHECK(poll_struct_add(ps, POLL_EV_CLOSE, SET_C, 0) == 0, "C watches CLOSE");
    CHECK(poll_struct_add(ps, POLL_EV_KERNEL, SET_D, 0) == 0, "D watches KERNEL");

    poll_notify(ps, POLL_EV_IN, 1u);
    CHECK(signalled(SET_A) == 1, "the IN watcher fires");
    CHECK(signalled(SET_B) == 0 && signalled(SET_C) == 0 && signalled(SET_D) == 0,
          "watchers on other events do not");

    reset();
    poll_notify(ps, POLL_EV_KERNEL, 2u);
    CHECK(signalled(SET_D) == 1 && g_signal_count == 1, "and each event reaches only its own list");

    poll_struct_free(ps);
}

static void test_one_set_may_watch_several_events(void) {
    reset();
    poll_struct_t* ps = poll_struct_alloc();
    CHECK(poll_struct_add(ps, POLL_EV_IN, SET_A, 0) == 0, "A watches IN");
    CHECK(poll_struct_add(ps, POLL_EV_CLOSE, SET_A, 0) == 0, "and CLOSE");

    poll_notify(ps, POLL_EV_IN, 1u);
    CHECK(signalled(SET_A) == 1, "the IN notify reaches it once, not twice");
    poll_notify(ps, POLL_EV_CLOSE, 1u);
    CHECK(signalled(SET_A) == 2, "the CLOSE notify reaches it too");

    /* Removal is per-set across ALL events — the property ipc_select_destroy
     * depends on, since it never says which event it registered under. */
    reset();
    poll_struct_remove(ps, SET_A);
    poll_notify(ps, POLL_EV_IN, 1u);
    poll_notify(ps, POLL_EV_CLOSE, 1u);
    CHECK(g_signal_count == 0, "one remove clears the set from every event list");

    poll_struct_free(ps);
}

static void test_add_rejects_bad_arguments(void) {
    reset();
    poll_struct_t* ps = poll_struct_alloc();
    CHECK(poll_struct_add(0, POLL_EV_IN, SET_A, 0) == -1, "a NULL struct is refused");
    CHECK(poll_struct_add(ps, POLL_EV_IN, 0, 0) == -1, "a NULL set is refused");
    CHECK(poll_struct_add(ps, (poll_ev_t)POLL_EV_MAX, SET_A, 0) == -1, "an out-of-range event");
    CHECK(poll_struct_add(ps, (poll_ev_t)-1, SET_A, 0) == -1, "a negative event");
    CHECK(poll_struct_add(ps, (poll_ev_t)9999, SET_A, 0) == -1, "a wildly out-of-range event");

    poll_notify(ps, POLL_EV_IN, 1u);
    CHECK(g_signal_count == 0, "no rejected add left a watcher behind");
    poll_struct_free(ps);
}

static void test_add_reports_allocation_failure(void) {
    reset();
    poll_struct_t* ps = poll_struct_alloc();
    g_malloc_fail = 1;
    int rc = poll_struct_add(ps, POLL_EV_IN, SET_A, 0);
    g_malloc_fail = 0;
    CHECK(rc == -1, "a watcher that could not be allocated is reported as a failure");
    poll_notify(ps, POLL_EV_IN, 1u);
    CHECK(g_signal_count == 0, "and really is not registered");

    CHECK(poll_struct_add(ps, POLL_EV_IN, SET_A, 0) == 0, "the retry succeeds");
    poll_notify(ps, POLL_EV_IN, 1u);
    CHECK(g_signal_count == 1, "and now it is registered");
    poll_struct_free(ps);
}

static void test_notify_tolerates_bad_arguments(void) {
    reset();
    poll_struct_t* ps = poll_struct_alloc();
    (void)poll_struct_add(ps, POLL_EV_IN, SET_A, 0);
    poll_notify(0, POLL_EV_IN, 1u); /* the documented NULL-safe case */
    poll_notify(ps, (poll_ev_t)POLL_EV_MAX, 1u);
    poll_notify(ps, (poll_ev_t)-1, 1u);
    CHECK(g_signal_count == 0, "none of the invalid notifies reach a watcher");
    poll_notify(ps, POLL_EV_IN, 1u);
    CHECK(g_signal_count == 1, "and the valid one still works");
    poll_struct_free(ps);
}

/* Watchers are a singly-linked list with head insertion, so removal has three
 * distinct shapes. ipc_select_destroy relies on all of them. */
static void test_remove_handles_head_middle_and_tail(void) {
    struct ipc_select* order[3] = {SET_A, SET_B, SET_C};
    /* Added A,B,C the list is C,B,A — so removing each in turn covers head,
     * middle and tail. */
    for (int victim = 0; victim < 3; ++victim) {
        reset();
        poll_struct_t* ps = poll_struct_alloc();
        for (int i = 0; i < 3; ++i) {
            (void)poll_struct_add(ps, POLL_EV_IN, order[i], 0);
        }
        poll_struct_remove(ps, order[victim]);
        poll_notify(ps, POLL_EV_IN, 5u);
        CHECK(g_signal_count == 2, "the other two watchers survive the removal");
        CHECK(signalled(order[victim]) == 0, "the removed one is gone");
        for (int i = 0; i < 3; ++i) {
            if (i != victim) {
                CHECK(signalled(order[i]) == 1, "each survivor is signalled exactly once");
            }
        }
        poll_struct_free(ps);
    }
}

static void test_remove_clears_every_entry_for_a_set(void) {
    reset();
    poll_struct_t* ps = poll_struct_alloc();
    /* The same set added repeatedly. ipc_select_add deduplicates, so this is not
     * reachable through it, but poll_struct_add is a general primitive and its
     * removal contract has to hold regardless: one remove must clear every entry,
     * or destroy would leave a dangling watcher pointing at a recycled table
     * slot. */
    for (int i = 0; i < 4; ++i) {
        CHECK(poll_struct_add(ps, POLL_EV_IN, SET_A, (uint32_t)i) == 0, "repeated add");
    }
    (void)poll_struct_add(ps, POLL_EV_IN, SET_B, 0);
    poll_notify(ps, POLL_EV_IN, 1u);
    CHECK(signalled(SET_A) == 4, "a set added four times is signalled four times");

    reset();
    poll_struct_remove(ps, SET_A);
    poll_notify(ps, POLL_EV_IN, 1u);
    CHECK(signalled(SET_A) == 0, "one remove clears every duplicate entry");
    CHECK(signalled(SET_B) == 1, "and leaves the other set alone");
    poll_struct_free(ps);
}

static void test_remove_tolerates_bad_arguments(void) {
    reset();
    poll_struct_t* ps = poll_struct_alloc();
    (void)poll_struct_add(ps, POLL_EV_IN, SET_A, 0);
    poll_struct_remove(0, SET_A);
    poll_struct_remove(ps, 0);
    poll_struct_remove(ps, SET_B); /* never added */
    poll_notify(ps, POLL_EV_IN, 1u);
    CHECK(signalled(SET_A) == 1, "no no-op removal disturbed the registered watcher");
    poll_struct_free(ps);
}

/* Under ASan this is the test that proves free walks every list; without it a
 * leak or a double free would go unnoticed. */
static void test_free_releases_every_watcher(void) {
    reset();
    poll_struct_t* ps = poll_struct_alloc();
    for (int ev = 0; ev < POLL_EV_MAX; ++ev) {
        CHECK(poll_struct_add(ps, (poll_ev_t)ev, SET_A, 0) == 0, "watcher on each event");
        CHECK(poll_struct_add(ps, (poll_ev_t)ev, SET_B, 0) == 0, "two per event");
    }
    poll_struct_free(ps);
    poll_struct_free(0); /* NULL-safe, though only poll_notify says so in poll.h */
    CHECK(1, "freeing a fully populated struct, and a NULL one, is clean");
}

static void test_user_data_does_not_affect_dispatch(void) {
    reset();
    poll_struct_t* ps = poll_struct_alloc();
    /* user_data is stored on the watcher but never read by the notify path:
     * ipc_select_signal is told the endpoint id and nothing else. */
    (void)poll_struct_add(ps, POLL_EV_IN, SET_A, 0xDEADBEEFu);
    (void)poll_struct_add(ps, POLL_EV_IN, SET_B, 0u);
    poll_notify(ps, POLL_EV_IN, 3u);
    CHECK(g_signal_count == 2, "both watchers fire regardless of user_data");
    CHECK(g_signals[0].ep_id == 3u && g_signals[1].ep_id == 3u,
          "the endpoint id, not user_data, is what reaches the set");
    poll_struct_free(ps);
}

/* -------------------------------------------------------------------- main */

int main(void) {
    struct {
        const char* name;
        void (*fn)(void);
    } tests[] = {
        {"P1 a fresh struct is empty", test_alloc_starts_empty},
        {"P2 alloc reports failure", test_alloc_reports_failure},
        {"P3 notify reaches every watcher", test_notify_reaches_every_watcher_on_that_event},
        {"P4 event types are independent", test_event_types_are_independent},
        {"P5 one set may watch several events", test_one_set_may_watch_several_events},
        {"P6 add rejects bad arguments", test_add_rejects_bad_arguments},
        {"P7 add reports allocation failure", test_add_reports_allocation_failure},
        {"P8 notify tolerates bad arguments", test_notify_tolerates_bad_arguments},
        {"P9 remove handles head/middle/tail", test_remove_handles_head_middle_and_tail},
        {"P10 remove clears every entry for a set", test_remove_clears_every_entry_for_a_set},
        {"P11 remove tolerates bad arguments", test_remove_tolerates_bad_arguments},
        {"P12 free releases every watcher", test_free_releases_every_watcher},
        {"P13 user_data does not affect dispatch", test_user_data_does_not_affect_dispatch},
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
    printf("test_poll: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        wasmos_test_report_seed(seed);
    }
    return g_failures == 0 ? 0 : 1;
}