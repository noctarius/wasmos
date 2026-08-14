/* Host shadow of src/kernel/include/sched_event.h.
 *
 * The enums and the function signatures match the real header, so kernel
 * sources that block on a sched_event_t (sync_mutex.c, sync_semaphore.c) build
 * unmodified. The struct differs: the real one parks threads on a `wait_list`
 * of thread_t.event_node, which a host process cannot do, so the waiting state
 * here is a pthread mutex/condvar plus a waiter and signal count.
 *
 * The functions are declared, not defined: the including test supplies bodies
 * that map wait/wake onto those host primitives, keeping the real header's
 * locking contract (sched_event_wait is entered with ev->lock held and returns
 * with it released). See tests/unit/test_kernel_sync_primitives.c. */
#ifndef WASMOS_TEST_SCHED_EVENT_H
#define WASMOS_TEST_SCHED_EVENT_H

#include <pthread.h>
#include <stdint.h>

#include "sync/spinlock.h"

/* Which subsystem owns the event. Values mirror the real header exactly, so a
 * kernel source compiled against this shadow tags its events identically. The
 * type is recorded and readable; nothing in the host bodies dispatches on it. */
typedef enum {
    SCHED_EVENT_TYPE_IPC = 0,
    SCHED_EVENT_TYPE_JOIN = 1,
    SCHED_EVENT_TYPE_PROCESS = 2,
    SCHED_EVENT_TYPE_SELECT = 3,
    SCHED_EVENT_TYPE_FUTEX = 4,
    SCHED_EVENT_TYPE_TIMER = 5,
    SCHED_EVENT_TYPE_MUTEX = 6,
    SCHED_EVENT_TYPE_SEMAPHORE = 7,
} sched_event_type_t;

/* Why a waiter came out of a wait: not yet resolved, woken normally, deadline
 * expired, or aborted because the thing waited on was destroyed. Values mirror
 * the real header. On target the waker stores one of these into the woken
 * thread; the host bodies in the including test take it as an argument and
 * discard it, so a case cannot distinguish the three wake reasons. */
typedef enum {
    SCHED_PEND_NONE = 0,
    SCHED_PEND_OK = 1,
    SCHED_PEND_TIMEOUT = 2,
    SCHED_PEND_ABORT = 3,
} sched_pend_state_t;

/* One wait queue plus the lock guarding both it and the state the waiters test.
 * The first four fields are the real struct's; `wait_list` is replaced by the
 * three host_* fields.
 *
 * Layout is NOT compatible with the kernel's sched_event_t, so an object built
 * by a source compiled against this header can only be passed to the host
 * bodies -- never to a real sched_event.c. Nothing links both. */
typedef struct {
    ksync_spinlock_t lock; /* Guards cnt and the waiter state; held across test-then-block. */
    uint32_t cnt;          /* Permit count; used only by ksync_semaphore_t. */
    sched_event_type_t type;
    pthread_mutex_t host_mutex; /* Guards host_waiters/host_signals and the condvar. */
    pthread_cond_t host_cond;   /* Where a parked pthread stands in for a parked thread. */
    uint32_t host_waiters;      /* Waiters currently parked on host_cond. */
    /* Undelivered wakes. A wake raised while host_waiters is 0 is not recorded,
     * so it is lost rather than remembered -- unlike the kernel, where a waiter
     * already enqueued under ev->lock cannot miss one. A case that means to
     * exercise the blocking path therefore has to wait for the waiter to park
     * before releasing the lock or the count. */
    uint32_t host_signals;
} sched_event_t;

struct thread;

/* Signatures below are the real header's, so sync_mutex.c and sync_semaphore.c
 * compile unmodified; the locking contract is the real one too. The bodies are
 * the including test's, and the notes here describe what those bodies can and
 * cannot reproduce. See tests/unit/test_kernel_sync_primitives.c. */

/* Initialise `ev` to type, no permits and no waiters. */
void sched_event_init(sched_event_t* ev, sched_event_type_t type);

/* Block the caller on `ev`. Entered with ev->lock held; the lock is released
 * before parking and is NOT held on return, matching the real contract -- which
 * is what makes the test-then-block sequence in the primitives under test
 * meaningful. timeout_ms == 0 means no timeout, and a host body has no deadline
 * machinery to arm, so a non-zero timeout parks indefinitely: the timed-wait
 * path of a primitive is not covered by a suite using this shadow. */
void sched_event_wait(sched_event_t* ev, uint32_t timeout_ms);

/* Wake the first waiter on `ev`, with ev->lock held by the caller. Returns the
 * woken thread, or NULL when there was none. A host body has no wait list to
 * pick from, so the returned pointer identifies no particular waiter; both call
 * sites in the primitives under test ignore it. `data` and `pend` are carried
 * for signature compatibility and are not observable at the waiter. */
struct thread* sched_event_wake_one(sched_event_t* ev, uint64_t data, sched_pend_state_t pend);

/* Wake every waiter on `ev`, with ev->lock held. Returns the number woken. */
int sched_event_wake_all(sched_event_t* ev, uint64_t data, sched_pend_state_t pend);

/* Release every waiter on `ev` with SCHED_PEND_ABORT, with ev->lock held. On
 * target this runs when the endpoint or select set behind the event is
 * destroyed. */
void sched_event_abort_all(sched_event_t* ev);

/* Wake any thread whose timed-wait deadline has passed. The scheduler calls this
 * each dispatch; a host process has no such dispatch loop, so a suite that wants
 * timeouts to fire has to call it itself -- or, as with the untimed waits above,
 * not cover them. */
void sched_timeout_check(void);

#endif
