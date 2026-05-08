#ifndef WASMOS_THREAD_H
#define WASMOS_THREAD_H

#include <stdint.h>
#include "process.h"

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
    THREAD_BLOCK_WAIT_THREAD
} thread_block_reason_t;

typedef struct thread {
    uint32_t tid;
    uint32_t owner_pid;
    thread_state_t state;
    thread_block_reason_t block_reason;
    uint8_t in_ready_queue;
    uint8_t is_kernel_worker;
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
    uint32_t join_waiter_tid;
    uint8_t detached;
    int32_t exit_status;
    char name_storage[THREAD_NAME_MAX];
    const char *name;
} thread_t;

void thread_init(void);
int thread_spawn_main(uint32_t owner_pid, const char *name, uint32_t *out_tid);
int thread_spawn_in_owner(uint32_t owner_pid,
                          const char *name,
                          thread_state_t initial_state,
                          thread_block_reason_t initial_reason,
                          uint32_t *out_tid);
thread_t *thread_get(uint32_t tid);
thread_t *thread_find_main_for_pid(uint32_t owner_pid);
int thread_owner_tid_at(uint32_t owner_pid, uint32_t index, uint32_t *out_tid);
void thread_mark_owner_exited(uint32_t owner_pid, int32_t exit_status);
void thread_reap_owner(uint32_t owner_pid);
void thread_set_state(uint32_t tid, thread_state_t state, thread_block_reason_t reason);
void thread_set_exit_status(uint32_t tid, int32_t exit_status);
void thread_reap(uint32_t tid);
void thread_set_current(uint32_t tid);
uint32_t thread_current_tid(void);

#endif
