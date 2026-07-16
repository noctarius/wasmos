#ifndef WASMOS_SYNC_SEMAPHORE_H
#define WASMOS_SYNC_SEMAPHORE_H

#include <stdint.h>

#include "sched_event.h"

typedef struct {
    sched_event_t event;
} ksync_semaphore_t;

enum { KSYNC_SEMAPHORE_OK = 0, KSYNC_SEMAPHORE_BUSY = 1 };

void ksync_semaphore_init(ksync_semaphore_t* sem, uint32_t initial_count);
int ksync_semaphore_try_acquire(ksync_semaphore_t* sem);
int ksync_semaphore_acquire(ksync_semaphore_t* sem);
int ksync_semaphore_release(ksync_semaphore_t* sem);
uint32_t ksync_semaphore_count(ksync_semaphore_t* sem);

#endif
