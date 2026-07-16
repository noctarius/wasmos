#ifndef WASMOS_SYNC_MUTEX_H
#define WASMOS_SYNC_MUTEX_H

#include <stdint.h>

#include "sched_event.h"

typedef struct {
    sched_event_t event;
    uint32_t owner_tid;
    uint8_t locked;
} ksync_mutex_t;

enum { KSYNC_MUTEX_OK = 0, KSYNC_MUTEX_BUSY = 1 };

void ksync_mutex_init(ksync_mutex_t* mutex);
int ksync_mutex_try_lock(ksync_mutex_t* mutex);
int ksync_mutex_lock(ksync_mutex_t* mutex);
int ksync_mutex_unlock(ksync_mutex_t* mutex);

#endif
