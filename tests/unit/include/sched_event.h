#ifndef WASMOS_TEST_SCHED_EVENT_H
#define WASMOS_TEST_SCHED_EVENT_H

#include <pthread.h>
#include <stdint.h>

#include "sync/spinlock.h"

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

typedef enum {
    SCHED_PEND_NONE = 0,
    SCHED_PEND_OK = 1,
    SCHED_PEND_TIMEOUT = 2,
    SCHED_PEND_ABORT = 3,
} sched_pend_state_t;

typedef struct {
    ksync_spinlock_t lock;
    uint32_t cnt;
    sched_event_type_t type;
    pthread_mutex_t host_mutex;
    pthread_cond_t host_cond;
    uint32_t host_waiters;
    uint32_t host_signals;
} sched_event_t;

struct thread;

void sched_event_init(sched_event_t* ev, sched_event_type_t type);
void sched_event_wait(sched_event_t* ev, uint32_t timeout_ms);
struct thread* sched_event_wake_one(sched_event_t* ev, uint64_t data, sched_pend_state_t pend);
int sched_event_wake_all(sched_event_t* ev, uint64_t data, sched_pend_state_t pend);
void sched_event_abort_all(sched_event_t* ev);
void sched_timeout_check(void);

#endif
