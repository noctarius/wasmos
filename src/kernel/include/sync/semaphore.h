#ifndef WASMOS_SYNC_SEMAPHORE_H
#define WASMOS_SYNC_SEMAPHORE_H

#include <stdint.h>

#include "sched_event.h"

/* Counting semaphore layered on sched_event_t. The count lives in event.cnt and
 * event.lock guards both it and the wait list, so a blocking acquire hands off
 * to sched_event_wait() with that lock held and cannot miss a release.
 * Every operation returns -1 for a NULL semaphore. */
typedef struct {
    sched_event_t event;
} ksync_semaphore_t;

enum { KSYNC_SEMAPHORE_OK = 0, KSYNC_SEMAPHORE_BUSY = 1 };

void ksync_semaphore_init(ksync_semaphore_t* sem, uint32_t initial_count);

/* KSYNC_SEMAPHORE_OK when a permit was taken, KSYNC_SEMAPHORE_BUSY at count 0. */
int ksync_semaphore_try_acquire(ksync_semaphore_t* sem);

/* Blocks until a permit is available, then returns KSYNC_SEMAPHORE_OK. Must not
 * be called with interrupts disabled or from a context that cannot block. */
int ksync_semaphore_acquire(ksync_semaphore_t* sem);

/* Adds one permit and wakes at most one waiter. The count saturates rather than
 * wrapping: at UINT32_MAX the release is refused with -1, because wrapping to 0
 * would report a full semaphore as empty and strand every waiter. */
int ksync_semaphore_release(ksync_semaphore_t* sem);

/* Current permit count. A snapshot only -- it can change before the caller acts
 * on it. Reports 0 for a NULL semaphore, which is indistinguishable from a
 * genuinely empty one. */
uint32_t ksync_semaphore_count(ksync_semaphore_t* sem);

#endif
