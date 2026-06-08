#ifndef WASMOS_SCHED_EVENT_H
#define WASMOS_SCHED_EVENT_H

#ifdef WASMOS_SCHED_THREADABLE

#include <stdint.h>
#include "spinlock.h"
#include "sched_list.h"

typedef enum {
    SCHED_EVENT_TYPE_IPC     = 0,
    SCHED_EVENT_TYPE_JOIN    = 1,
    SCHED_EVENT_TYPE_PROCESS = 2,
    SCHED_EVENT_TYPE_SELECT  = 3,
    SCHED_EVENT_TYPE_FUTEX   = 4,
    SCHED_EVENT_TYPE_TIMER   = 5,
} sched_event_type_t;

typedef enum {
    SCHED_PEND_NONE    = 0,
    SCHED_PEND_OK      = 1,
    SCHED_PEND_TIMEOUT = 2,
    SCHED_PEND_ABORT   = 3,
} sched_pend_state_t;

typedef struct {
    spinlock_t          lock;
    list_head_t         wait_list; /* thread_t.event_node members */
    uint32_t            cnt;       /* semaphore count (IPC / SELECT) */
    sched_event_type_t  type;
} sched_event_t;

struct thread;

/* Initialise an event struct. */
void sched_event_init(sched_event_t *ev, sched_event_type_t type);

/*
 * Block the calling thread on ev.  Caller must hold ev->lock on entry;
 * the lock is released before yielding and must NOT be held on return.
 * timeout_ms == 0 means no timeout.
 */
void sched_event_wait(sched_event_t *ev, uint32_t timeout_ms);

/*
 * Wake the first waiter on ev with the given pend state and data.
 * Caller must hold ev->lock.  Returns the woken thread or NULL.
 */
struct thread *sched_event_wake_one(sched_event_t *ev,
                                    uint64_t data,
                                    sched_pend_state_t pend);

/*
 * Wake all waiters.  Caller must hold ev->lock.
 * Returns the number of threads woken.
 */
int sched_event_wake_all(sched_event_t *ev,
                         uint64_t data,
                         sched_pend_state_t pend);

/*
 * Abort all waiters (SCHED_PEND_ABORT).  Used when an endpoint or
 * select set is destroyed.  Caller must hold ev->lock.
 */
void sched_event_abort_all(sched_event_t *ev);

#endif /* WASMOS_SCHED_THREADABLE */
#endif /* WASMOS_SCHED_EVENT_H */
