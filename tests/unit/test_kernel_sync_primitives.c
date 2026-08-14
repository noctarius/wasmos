/* test_kernel_sync_primitives.c — host tests for the REAL ksync mutex and semaphore
 * (src/kernel/sync_mutex.c and src/kernel/sync_semaphore.c, compiled unmodified).
 *
 * Both are layered on sched_event_t, whose real form parks threads on a wait list of
 * thread_t.event_node -- something a host process cannot do. The compile line therefore
 * puts tests/unit/include/sched_event.h ahead of the kernel's: same enums, same
 * signatures, a pthread mutex/condvar in place of the wait list. The bodies are supplied
 * below, together with the spinlock family (this file's compile line does not link
 * stubs_spinlock.c) and thread_current_tid.
 *
 * MODELLING NOTE (blocking). Unlike the other host suites in this tree, a contended
 * acquire here genuinely parks: sched_event_wait releases ev->lock -- as the kernel's
 * does before yielding -- and then blocks the calling pthread until a wake signals it,
 * so ksync_mutex_lock's and ksync_semaphore_acquire's retry loops are driven for real by
 * a second thread. What the substitution does not model:
 *   - Wake order is the condvar's, not the wait list's, so the kernel's FIFO handoff is
 *     not observable here; test_sched_event.c covers it against the real sched_event.c.
 *   - timeout_ms is ignored, so every wait is untimed. Both call sites pass 0.
 *   - Nothing is registered as a waiter on a thread_t: no state transition, no pend
 *     state, no deadline. wake_all and abort_all are inert, and neither primitive calls
 *     them.
 */

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>

#include "test_shuffle.h"

/* <sched.h> on the -I path resolves to the kernel's own sched.h (which has no
 * libc prototype), and sched_yield() is not reliably declared by the other
 * system headers across platforms (it happens to be on macOS but not under
 * glibc -std=c11). Declare the libc symbol explicitly; it is provided at link
 * time. */
extern int sched_yield(void);

#include "sync/mutex.h"
#include "sync/semaphore.h"
#include "thread.h"

/* Scheduler, thread and spinlock stubs. sync_mutex.c and sync_semaphore.c are
 * compiled unmodified against these; this file supplies the spinlock family
 * itself because its compile line does not link stubs_spinlock.c.
 *
 * A host process cannot deschedule a kernel thread, so sched_event_wait
 * releases ev->lock -- as the kernel's version does before yielding -- and
 * parks the calling pthread on a condvar; sched_event_wake_one signals one
 * waiter and returns a fixed dummy thread, which both call sites ignore. A
 * signal raised before the waiter reaches the condvar is not recorded, so a
 * case that means to exercise the blocking path waits for the waiter to arrive
 * (wait_for_host_waiters) before it releases the lock or the count. The two
 * call counters let a case assert that the blocking path was taken rather than
 * the uncontended one. g_current_tid is thread-local, so each pthread carries
 * its own identity for the mutex owner checks. */
static _Thread_local uint32_t g_current_tid = 1u;
static uint32_t g_wait_call_count = 0u;
static uint32_t g_wake_one_call_count = 0u;
static thread_t g_woken_thread = {.tid = 99u};

/* Host spinlock family: a plain test-and-set lock, NULL-tolerant throughout.
 * spinlock_try_lock returns non-zero when the lock was acquired and 0 when it was
 * already held (or NULL) -- the same polarity as the kernel's.
 *
 * Divergence from src/kernel/spinlock.c: spinlock_lock neither saves RFLAGS nor issues
 * cli, and raises no preempt depth, so it is indistinguishable from spinlock_lock_noirq
 * and a lock taken through it is NOT held with interrupts disabled. Nothing in a host
 * process can interrupt a critical section, so that guarantee is untested here rather
 * than violated. Recursive acquisition on one thread deadlocks, as in the kernel. */
void spinlock_init(spinlock_t* lock) {
    if (lock) {
        lock->state = 0u;
    }
}

int spinlock_try_lock(spinlock_t* lock) {
    if (!lock) {
        return 0;
    }
    return __sync_lock_test_and_set(&lock->state, 1u) == 0u;
}

void spinlock_lock(spinlock_t* lock) {
    if (!lock) {
        return;
    }
    while (!spinlock_try_lock(lock)) {
        __sync_synchronize();
    }
}

void spinlock_unlock(spinlock_t* lock) {
    if (lock) {
        __sync_lock_release(&lock->state);
    }
}

void spinlock_lock_noirq(spinlock_t* lock) {
    spinlock_lock(lock);
}

void spinlock_unlock_noirq(spinlock_t* lock) {
    spinlock_unlock(lock);
}

/* The identity the mutex attributes an acquire to. Thread-local, so each pthread in a
 * case carries its own tid for the owner checks. The kernel reads
 * cpu_local()->current_thread and answers 0 when a CPU is running no thread, which never
 * happens here -- so ksync_mutex_current_owner_tid's 0 -> 0xFFFFFFFF fallback for
 * early-boot callers is not reached by any case. */
uint32_t thread_current_tid(void) {
    return g_current_tid;
}

/* Mirrors the real initialiser (lock, count, type) and additionally brings up the host
 * mutex/condvar and clears the waiter and signal counts. Aborts on a NULL event rather
 * than ignoring it. Not safe against a live event: a pthread already parked on it would
 * be abandoned. */
void sched_event_init(sched_event_t* ev, sched_event_type_t type) {
    assert(ev);
    ksync_spinlock_init(&ev->lock);
    ev->cnt = 0u;
    ev->type = type;
    assert(pthread_mutex_init(&ev->host_mutex, 0) == 0);
    assert(pthread_cond_init(&ev->host_cond, 0) == 0);
    ev->host_waiters = 0u;
    ev->host_signals = 0u;
}

/* Entered with ev->lock HELD and returns with it released -- the real header's contract,
 * which is what lets a caller test its condition and hand off without losing a wake. The
 * caller then blocks on the condvar until a signal is available.
 *
 * timeout_ms is ignored: the wait is untimed, so a caller that passed a deadline would
 * park forever. Nothing is linked into a wait list and no thread state, pend state or
 * deadline is touched. Each entry is counted in g_wait_call_count so a case can assert
 * that the blocking path, not the uncontended one, was taken. */
void sched_event_wait(sched_event_t* ev, uint32_t timeout_ms) {
    (void)timeout_ms;
    assert(ev);
    ksync_spinlock_unlock(&ev->lock);
    __atomic_fetch_add(&g_wait_call_count, 1u, __ATOMIC_RELAXED);
    assert(pthread_mutex_lock(&ev->host_mutex) == 0);
    ev->host_waiters++;
    while (ev->host_signals == 0u) {
        assert(pthread_cond_wait(&ev->host_cond, &ev->host_mutex) == 0);
    }
    ev->host_signals--;
    ev->host_waiters--;
    assert(pthread_mutex_unlock(&ev->host_mutex) == 0);
}

/* Called with ev->lock held and leaves it held, as in the kernel. Releases at most one
 * parked pthread; a signal raised while no waiter is registered is DROPPED rather than
 * queued, which is why the blocking cases hand off through wait_for_host_waiters.
 *
 * `data` and `pend` are discarded: the real one delivers them to the woken thread as
 * pend_data/pend_state. The returned thread_t* is a fixed dummy rather than the waiter
 * that was released -- both call sites ignore the return value -- so nothing here
 * observes which thread a wake reached. Each call is counted in g_wake_one_call_count. */
thread_t* sched_event_wake_one(sched_event_t* ev, uint64_t data, sched_pend_state_t pend) {
    (void)data;
    (void)pend;
    assert(ev);
    __atomic_fetch_add(&g_wake_one_call_count, 1u, __ATOMIC_RELAXED);
    assert(pthread_mutex_lock(&ev->host_mutex) == 0);
    if (ev->host_waiters != 0u) {
        ev->host_signals++;
        assert(pthread_cond_signal(&ev->host_cond) == 0);
    }
    assert(pthread_mutex_unlock(&ev->host_mutex) == 0);
    return &g_woken_thread;
}

/* Inert: neither the mutex nor the semaphore reaches these three, so no host waiter is
 * released, no deadline is scanned, and wake_all always reports zero woken. They exist
 * to satisfy the link against the shadow header. A case that needed a broadcast wake or
 * a timed wait would have to give them real bodies first. */
int sched_event_wake_all(sched_event_t* ev, uint64_t data, sched_pend_state_t pend) {
    (void)ev;
    (void)data;
    (void)pend;
    return 0;
}

void sched_event_abort_all(sched_event_t* ev) {
    (void)ev;
}

void sched_timeout_check(void) {}

/* Per-case fixture reset, called first by every case: the calling thread's identity back
 * to tid 1 and both call counters to zero. It resets no sched_event_t -- each case
 * declares its own mutex or semaphore and initialises it -- and it does not reach worker
 * pthreads, which set their own g_current_tid on entry and are joined before the next
 * case runs. */
static void reset_scheduler_stubs(void) {
    g_current_tid = 1u;
    g_wait_call_count = 0u;
    g_wake_one_call_count = 0u;
}

/* Spin until at least `expected_waiters` pthreads are parked on `ev`, yielding between
 * polls, and abort through assert after 100000 spins rather than hanging the suite.
 *
 * This is the handshake that makes a blocking case deterministic: because a signal
 * raised with no waiter present is dropped, the wake must not be issued until its waiter
 * is actually on the condvar. Returning means at least that many waiters arrived, not
 * exactly that many. */
static void wait_for_host_waiters(sched_event_t* ev, uint32_t expected_waiters) {
    uint32_t spins = 0u;
    assert(ev);
    while (spins++ < 100000u) {
        uint32_t waiters = 0u;
        assert(pthread_mutex_lock(&ev->host_mutex) == 0);
        waiters = ev->host_waiters;
        assert(pthread_mutex_unlock(&ev->host_mutex) == 0);
        if (waiters >= expected_waiters) {
            return;
        }
        sched_yield();
    }
    assert(!"timed out waiting for blocked test thread");
}

static void test_mutex_try_lock_and_unlock(void) {
    ksync_mutex_t mutex;

    reset_scheduler_stubs();
    ksync_mutex_init(&mutex);
    assert(mutex.event.type == SCHED_EVENT_TYPE_MUTEX);
    assert(ksync_mutex_try_lock(&mutex) == KSYNC_MUTEX_OK);
    assert(mutex.locked == 1u);
    assert(mutex.owner_tid == 1u);
    assert(ksync_mutex_try_lock(&mutex) == -1);
    assert(ksync_mutex_unlock(&mutex) == KSYNC_MUTEX_OK);
    assert(mutex.locked == 0u);
    assert(mutex.owner_tid == 0u);
    assert(g_wake_one_call_count == 1u);
    assert(ksync_mutex_unlock(&mutex) == -1);
}

typedef struct {
    ksync_mutex_t* mutex;
    int try_result;
} mutex_try_thread_arg_t;

typedef struct {
    ksync_mutex_t* mutex;
    volatile uint32_t acquired;
    volatile uint32_t released;
} mutex_lock_thread_arg_t;

static void* mutex_try_lock_worker(void* arg) {
    mutex_try_thread_arg_t* worker_arg = (mutex_try_thread_arg_t*)arg;
    g_current_tid = 2u;
    worker_arg->try_result = ksync_mutex_try_lock(worker_arg->mutex);
    return 0;
}

static void* mutex_blocking_lock_worker(void* arg) {
    mutex_lock_thread_arg_t* worker_arg = (mutex_lock_thread_arg_t*)arg;
    g_current_tid = 2u;
    assert(ksync_mutex_lock(worker_arg->mutex) == KSYNC_MUTEX_OK);
    worker_arg->acquired = 1u;
    assert(ksync_mutex_unlock(worker_arg->mutex) == KSYNC_MUTEX_OK);
    worker_arg->released = 1u;
    return 0;
}

static void test_mutex_try_lock_fails_under_contention(void) {
    ksync_mutex_t mutex;
    pthread_t worker;
    mutex_try_thread_arg_t worker_arg = {0};

    reset_scheduler_stubs();
    ksync_mutex_init(&mutex);
    assert(ksync_mutex_try_lock(&mutex) == KSYNC_MUTEX_OK);
    worker_arg.mutex = &mutex;
    assert(pthread_create(&worker, 0, mutex_try_lock_worker, &worker_arg) == 0);
    assert(pthread_join(worker, 0) == 0);
    assert(worker_arg.try_result == KSYNC_MUTEX_BUSY);
    assert(ksync_mutex_unlock(&mutex) == KSYNC_MUTEX_OK);
}

static void test_mutex_lock_waits_until_released_by_other_thread(void) {
    ksync_mutex_t mutex;
    pthread_t worker;
    mutex_lock_thread_arg_t worker_arg = {0};

    reset_scheduler_stubs();
    ksync_mutex_init(&mutex);
    assert(ksync_mutex_try_lock(&mutex) == KSYNC_MUTEX_OK);
    worker_arg.mutex = &mutex;
    assert(pthread_create(&worker, 0, mutex_blocking_lock_worker, &worker_arg) == 0);
    wait_for_host_waiters(&mutex.event, 1u);
    assert(ksync_mutex_unlock(&mutex) == KSYNC_MUTEX_OK);
    assert(pthread_join(worker, 0) == 0);
    assert(worker_arg.acquired == 1u);
    assert(worker_arg.released == 1u);
    assert(__atomic_load_n(&g_wait_call_count, __ATOMIC_RELAXED) >= 1u);
}

static void test_mutex_unlock_rejects_non_owner(void) {
    ksync_mutex_t mutex;

    reset_scheduler_stubs();
    ksync_mutex_init(&mutex);
    assert(ksync_mutex_try_lock(&mutex) == KSYNC_MUTEX_OK);
    g_current_tid = 3u;
    assert(ksync_mutex_unlock(&mutex) == -1);
    assert(mutex.locked == 1u);
    assert(mutex.owner_tid == 1u);
}

typedef struct {
    ksync_semaphore_t* sem;
    volatile uint32_t acquired;
} semaphore_thread_arg_t;

static void* semaphore_acquire_worker(void* arg) {
    semaphore_thread_arg_t* worker_arg = (semaphore_thread_arg_t*)arg;
    g_current_tid = 3u;
    assert(ksync_semaphore_acquire(worker_arg->sem) == KSYNC_SEMAPHORE_OK);
    worker_arg->acquired = 1u;
    return 0;
}

static void test_semaphore_try_acquire_and_release(void) {
    ksync_semaphore_t sem;

    reset_scheduler_stubs();
    ksync_semaphore_init(&sem, 2u);
    assert(sem.event.type == SCHED_EVENT_TYPE_SEMAPHORE);
    assert(ksync_semaphore_count(&sem) == 2u);
    assert(ksync_semaphore_try_acquire(&sem) == KSYNC_SEMAPHORE_OK);
    assert(ksync_semaphore_try_acquire(&sem) == KSYNC_SEMAPHORE_OK);
    assert(ksync_semaphore_try_acquire(&sem) == KSYNC_SEMAPHORE_BUSY);
    assert(ksync_semaphore_release(&sem) == KSYNC_SEMAPHORE_OK);
    assert(g_wake_one_call_count == 1u);
    assert(ksync_semaphore_count(&sem) == 1u);
}

static void test_semaphore_acquire_blocks_until_release(void) {
    ksync_semaphore_t sem;
    pthread_t worker;
    semaphore_thread_arg_t worker_arg = {0};

    reset_scheduler_stubs();
    ksync_semaphore_init(&sem, 0u);
    worker_arg.sem = &sem;
    assert(pthread_create(&worker, 0, semaphore_acquire_worker, &worker_arg) == 0);
    wait_for_host_waiters(&sem.event, 1u);
    assert(ksync_semaphore_release(&sem) == KSYNC_SEMAPHORE_OK);
    assert(pthread_join(worker, 0) == 0);
    assert(worker_arg.acquired == 1u);
    assert(__atomic_load_n(&g_wait_call_count, __ATOMIC_RELAXED) >= 1u);
    assert(ksync_semaphore_count(&sem) == 0u);
}

static void test_semaphore_release_rejects_overflow(void) {
    ksync_semaphore_t sem;

    reset_scheduler_stubs();
    ksync_semaphore_init(&sem, 0xFFFFFFFFu);
    assert(ksync_semaphore_release(&sem) == -1);
    assert(g_wake_one_call_count == 0u);
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_mutex_try_lock_and_unlock),
        WASMOS_TEST_CASE(test_mutex_try_lock_fails_under_contention),
        WASMOS_TEST_CASE(test_mutex_lock_waits_until_released_by_other_thread),
        WASMOS_TEST_CASE(test_mutex_unlock_rejects_non_owner),
        WASMOS_TEST_CASE(test_semaphore_try_acquire_and_release),
        WASMOS_TEST_CASE(test_semaphore_acquire_blocks_until_release),
        WASMOS_TEST_CASE(test_semaphore_release_rejects_overflow),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_kernel_sync_primitives: ok\n");
    return 0;
}
