/* thread.h - Kernel thread management within a process.
 *
 * A process owns one or more thread_t records.  Each thread has its own kernel
 * stack and process_context_t but shares the process address space.  The main
 * thread (spawned by process_manager) is the one that runs the WASM entry point.
 * Worker threads are created via the thread_create syscall from ring-3. */
#ifndef WASMOS_THREAD_H
#define WASMOS_THREAD_H

#include <stdint.h>
#include "process.h"

#define THREAD_MAX_COUNT 128  /* global limit on live kernel threads */
#define THREAD_NAME_MAX 64

typedef enum {
    THREAD_STATE_UNUSED = 0,
    THREAD_STATE_READY,
    THREAD_STATE_RUNNING,
    THREAD_STATE_BLOCKED,
    THREAD_STATE_ZOMBIE
} thread_state_t;

typedef enum {
    THREAD_BLOCK_NONE = 0,
    THREAD_BLOCK_IPC,
    THREAD_BLOCK_WAIT_PROCESS,
    THREAD_BLOCK_WAIT_THREAD
} thread_block_reason_t;

typedef struct thread {
    uint32_t tid;
    uint32_t owner_pid;
    thread_state_t state;
    thread_block_reason_t block_reason;
    uint8_t in_ready_queue;
    uint8_t is_kernel_worker;
    /* Set while this thread is transitioning from RUNNING to BLOCKED on the
     * current CPU and has not yet completed process_yield.  The scheduler
     * skips (and re-enqueues) READY threads with this flag set to prevent
     * a second CPU from restoring an in-progress context save. */
    uint8_t blocking_transition;
    uintptr_t kstack_base;
    uintptr_t kstack_top;
    uintptr_t kstack_alloc_base_phys;
    uint32_t kstack_pages;
    uintptr_t worker_entry;
    void *worker_arg;
    uint32_t time_slice_ticks;
    uint32_t ticks_remaining;
    uint64_t ticks_total;
    process_context_t ctx;
    uint32_t wait_target_pid;
    uint32_t join_waiter_tid;
    uint8_t detached;
    int32_t exit_status;
    /* Saved per-thread copy of cpu_local()->wasm3_heap_bound_pid.  Captured in
     * process_yield before every context switch and restored to the CPU when
     * this thread is next scheduled, preventing cross-thread heap-PID leakage
     * when multiple wasm_driver threads interleave on the same CPU. */
    uint32_t wasm3_heap_bound_pid;
    char name_storage[THREAD_NAME_MAX];
    const char *name;
} thread_t;

/* Initialize the thread table; called once during kernel startup. */
void thread_init(void);

/* Spawn the main thread for a newly created process; sets initial state to BLOCKED. */
int thread_spawn_main(uint32_t owner_pid, const char *name, uint32_t *out_tid);

/* Create an additional thread in owner_pid's process with caller-specified initial state. */
int thread_spawn_in_owner(uint32_t owner_pid,
                          const char *name,
                          thread_state_t initial_state,
                          thread_block_reason_t initial_reason,
                          uint32_t *out_tid);

thread_t *thread_get(uint32_t tid);

/* Find the main thread (first-spawned) belonging to owner_pid. */
thread_t *thread_find_main_for_pid(uint32_t owner_pid);

/* Return the tid of the index-th thread owned by owner_pid. */
int thread_owner_tid_at(uint32_t owner_pid, uint32_t index, uint32_t *out_tid);

/* Transition all threads owned by owner_pid to ZOMBIE with exit_status. */
void thread_mark_owner_exited(uint32_t owner_pid, int32_t exit_status);

/* Free all thread records belonging to owner_pid (called after process reap). */
void thread_reap_owner(uint32_t owner_pid);

void thread_set_state(uint32_t tid, thread_state_t state, thread_block_reason_t reason);

/* Wake tid if it is currently blocked; returns non-zero if a wakeup was delivered. */
int thread_wake_if_blocked(uint32_t tid);

void thread_set_exit_status(uint32_t tid, int32_t exit_status);
void thread_reap(uint32_t tid);
void thread_set_current(uint32_t tid);
uint32_t thread_current_tid(void);

#endif
