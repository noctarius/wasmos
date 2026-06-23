#ifndef WASMOS_THREAD_H
#define WASMOS_THREAD_H

#include <stdint.h>
#include "process.h"

#include "sched_list.h"
#include "sched_event.h"

#define THREAD_MAX_COUNT 128
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
    THREAD_BLOCK_WAIT_THREAD,
    THREAD_BLOCK_EVENT,   /* blocked on a sched_event_t (threadable scheduler) */
} thread_block_reason_t;

typedef struct thread {
    uint32_t tid;
    uint32_t owner_pid;
    thread_state_t state;
    thread_block_reason_t block_reason;
    uint8_t is_kernel_worker;
    uint8_t blocking_transition; /* RUNNING→BLOCKED in-progress (atomic) */
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
    uint32_t wasm3_heap_bound_pid;
#ifdef WASMOS_WARP_RING3
    /* Per-thread WARP ring-3 call state.  A ring-3 export call can block in a
     * hostcall (e.g. blocking IPC), yield, and later resume/migrate to a
     * different CPU, so this state must travel with the thread rather than live
     * in cpu_local.  Set just before the ring-3 IRET and consumed by the
     * WARP_RETURN syscall handler via __builtin_longjmp. */
    uint64_t warp_r3_old_cr3;   /* CR3 to restore when the ring-3 call returns */
    uint8_t  warp_r3_active;    /* 1 while a ring-3 call is in progress */
    void    *warp_r3_jbuf[5];   /* setjmp checkpoint for WARP_RETURN */
#endif
    char name_storage[THREAD_NAME_MAX];
    const char *name;
    /* Per-thread context canaries (moved from process_t). */
    uint64_t        ctx_canary_pre;
    uint64_t        ctx_canary_post;
    /* Scheduler priority and CPU placement. */
    uint8_t         sched_prio;     /* SCHED_PRIO_* */
    uint8_t         sched_sticky;   /* 1 if last run was a voluntary yield (poller);
                                     * work-stealing skips it so idle CPUs do not
                                     * thrash re-running it. Cleared on dispatch. */
    uint32_t        cpu_affinity;   /* allowed CPU bitmask; ~0u = any */
    uint32_t        last_cpu;       /* CPU where thread last ran */
    uint64_t        sched_timeout_tick; /* deadline tick for a timed wait; 0 = none */
    /* Intrusive linkage for scheduler lists. */
    list_head_t     sched_node;     /* linkage in cpu_sched_t.ready_list */
    list_head_t     event_node;     /* linkage in sched_event_t.wait_list */
    /* Event blocking state. */
    sched_event_t  *wait_event;     /* event this thread is blocked on */
    uint32_t        pend_state;     /* SCHED_PEND_* set by waker */
    uint64_t        pend_data;      /* value from waker (futex retcode, etc.) */
    /* Thread-join event (replaces join_waiter_tid for new scheduler). */
    sched_event_t   join_event;
} thread_t;

void thread_init(void);
int thread_spawn_main(uint32_t owner_pid, const char *name, uint32_t *out_tid);
int thread_spawn_in_owner(uint32_t owner_pid,
                          const char *name,
                          thread_state_t initial_state,
                          thread_block_reason_t initial_reason,
                          uint32_t *out_tid);
thread_t *thread_get(uint32_t tid);
/* Return the thread table slot at `index` (0..THREAD_MAX_COUNT-1), or NULL.
 * For table-wide scans (e.g. the scheduler timeout sweep). */
thread_t *thread_table_at(uint32_t index);
thread_t *thread_find_main_for_pid(uint32_t owner_pid);
int thread_owner_tid_at(uint32_t owner_pid, uint32_t index, uint32_t *out_tid);
void thread_mark_owner_exited(uint32_t owner_pid, int32_t exit_status);
void thread_reap_owner(uint32_t owner_pid);
void thread_set_state(uint32_t tid, thread_state_t state, thread_block_reason_t reason);
int thread_wake_if_blocked(uint32_t tid);
void thread_set_exit_status(uint32_t tid, int32_t exit_status);
void thread_reap(uint32_t tid);
void thread_set_current(uint32_t tid);
uint32_t thread_current_tid(void);

#endif
