#ifndef WASMOS_SCHED_H
#define WASMOS_SCHED_H

#ifdef WASMOS_SCHED_THREADABLE

#include <stdint.h>
#include "spinlock.h"
#include "sched_list.h"

#define SCHED_PRIO_MAX  7   /* number of priority levels */
#define SCHED_PRIO_BITS 7   /* bitmask width; one bit per level */

typedef enum {
    SCHED_PRIO_REALTIME   = 0, /* IRQ handler workers, timer callbacks */
    SCHED_PRIO_DRIVER     = 1, /* native drivers */
    SCHED_PRIO_SERVICE    = 2, /* system services (device-manager, fs-manager) */
    SCHED_PRIO_SYSTEM     = 3, /* kernel services (process-manager, chardev) */
    SCHED_PRIO_WASM       = 4, /* default WASM processes */
    SCHED_PRIO_BACKGROUND = 5, /* batch / background jobs */
    SCHED_PRIO_IDLE       = 6, /* idle process only */
} sched_prio_t;

struct thread;

typedef struct {
    spinlock_t   lock;
    uint8_t      ready_bitmap;               /* bit i set ↔ ready_list[i] non-empty */
    list_head_t  ready_list[SCHED_PRIO_MAX]; /* one FIFO per priority */
    uint32_t     thread_count[SCHED_PRIO_MAX];
    struct thread *running;                  /* currently executing thread */
    struct thread *idle;                     /* this CPU's idle thread */
    uint32_t     nr_threads;                 /* total threads tracked */
} cpu_sched_t;

/* Initialise the per-CPU scheduler state (called once per CPU at boot). */
void cpu_sched_init(cpu_sched_t *cs);

/* Enqueue a READY thread.  Sends a reschedule hint if the new thread
 * has higher priority than the currently running one. */
void cpu_sched_enqueue(cpu_sched_t *cs, struct thread *t);

/* Remove a thread from whichever priority bucket it is in.
 * Caller must hold cs->lock. */
void cpu_sched_dequeue(cpu_sched_t *cs, struct thread *t);

/* Return the highest-priority ready thread, or cs->idle if none.
 * Caller must hold cs->lock. */
struct thread *cpu_sched_pick_next(cpu_sched_t *cs);

/* Mark the current CPU as needing a reschedule. */
void sched_set_need_resched(void);

/* Wake a blocked thread: wait for its blocking_transition to clear,
 * set state READY, and enqueue it. */
void sched_wake_thread(struct thread *t);

/* Assign a default scheduler priority based on process flags. */
sched_prio_t sched_default_prio(int is_idle,
                                int is_kernel_worker,
                                int is_driver,
                                int is_native_service);

/*
 * Initialise the threadable-scheduler fields of a freshly-spawned thread.
 * Must be called for every thread before its first enqueue.
 */
void sched_thread_init(struct thread *t, sched_prio_t prio);

/*
 * Try to steal a ready thread from another CPU's queue.  Uses trylock to
 * avoid deadlock; returns NULL if no work is available or all remote queues
 * are busy.  my_cpu_id is the calling CPU's index into g_cpus[].
 */
struct thread *cpu_sched_try_steal(uint32_t my_cpu_id);

/*
 * Return the index of the CPU with the lightest ready-queue load.
 * Used at spawn time to distribute new processes across CPUs.
 */
uint32_t cpu_sched_pick_target_cpu(void);

/*
 * Enqueue a freshly spawned thread on the least-loaded CPU and set
 * last_cpu accordingly.  Use only for the initial spawn enqueue; all
 * subsequent re-queues go through sched_enqueue_thread / sched_wake_thread.
 */
void sched_spawn_thread(struct thread *t);

#endif /* WASMOS_SCHED_THREADABLE */
#endif /* WASMOS_SCHED_H */
