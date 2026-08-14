#ifndef WASMOS_SYNC_MUTEX_H
#define WASMOS_SYNC_MUTEX_H

#include <stdint.h>

#include "sched_event.h"

/* Non-recursive blocking kernel mutex layered on sched_event_t. event.lock is
 * both the state lock and the wait-list lock, so a blocking lock hands off to
 * sched_event_wait() with that lock already held and cannot lose a wake.
 * Ownership is per thread, not per process. */
typedef struct {
    sched_event_t event;
    uint32_t owner_tid; /* 0 when unlocked; 0xFFFFFFFF for pre-thread-table callers */
    uint8_t locked;
} ksync_mutex_t;

enum { KSYNC_MUTEX_OK = 0, KSYNC_MUTEX_BUSY = 1 };

/* Put a mutex into the unlocked state with an empty wait list. Required before
 * first use, since the embedded event must be initialised. Calling it on a
 * mutex that is held or has waiters discards both. */
void ksync_mutex_init(ksync_mutex_t* mutex);

/* All three operations below return -1 for a NULL mutex or a rejected state.
 * A rejected state is deliberately distinct from KSYNC_MUTEX_BUSY: relocking a
 * mutex this thread already owns would deadlock, so it is refused rather than
 * blocked, and so is unlocking one this thread does not own. */

/* KSYNC_MUTEX_OK on acquisition, KSYNC_MUTEX_BUSY if another thread holds it. */
int ksync_mutex_try_lock(ksync_mutex_t* mutex);

/* Blocks until acquired, then returns KSYNC_MUTEX_OK. Must not be called with
 * interrupts disabled or from a context that cannot be descheduled. */
int ksync_mutex_lock(ksync_mutex_t* mutex);

/* Releases and wakes at most one waiter. Only the owning thread may unlock. */
int ksync_mutex_unlock(ksync_mutex_t* mutex);

#endif
