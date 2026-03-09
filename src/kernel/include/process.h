#ifndef WASMOS_PROCESS_H
#define WASMOS_PROCESS_H

#include <stdint.h>

#define PROCESS_MAX_COUNT 16
#define PROCESS_DEFAULT_SLICE_TICKS 5u
#define PROCESS_STACK_SIZE 16384u

typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rsp;
    uint64_t rip;
} process_context_t;

typedef enum {
    PROCESS_STATE_UNUSED = 0,
    PROCESS_STATE_READY,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_BLOCKED,
    PROCESS_STATE_ZOMBIE
} process_state_t;

typedef enum {
    PROCESS_RUN_YIELDED = 0,
    PROCESS_RUN_IDLE = 1,
    PROCESS_RUN_BLOCKED = 2,
    PROCESS_RUN_EXITED = 3
} process_run_result_t;

typedef enum {
    PROCESS_BLOCK_NONE = 0,
    PROCESS_BLOCK_IPC = 1,
    PROCESS_BLOCK_WAIT = 2
} process_block_reason_t;

struct process;
typedef process_run_result_t (*process_entry_t)(struct process *process, void *arg);

typedef struct process {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t context_id;
    process_state_t state;
    process_block_reason_t block_reason;
    uint32_t wait_target_pid;
    int32_t exit_status;
    uint32_t time_slice_ticks;
    uint32_t ticks_remaining;
    uint64_t ticks_total;
    uint8_t in_ready_queue;
    process_context_t ctx;
    uintptr_t stack_top;
    process_entry_t entry;
    void *arg;
    const char *name;
} process_t;

void process_init(void);
int process_spawn(const char *name, process_entry_t entry, void *arg, uint32_t *out_pid);
int process_spawn_as(uint32_t parent_pid, const char *name, process_entry_t entry, void *arg, uint32_t *out_pid);
process_t *process_get(uint32_t pid);
process_t *process_find_by_context(uint32_t context_id);
uint32_t process_current_pid(void);
void process_set_exit_status(process_t *process, int32_t exit_status);
void process_block_on_ipc(process_t *process);
int process_wait(process_t *process, uint32_t target_pid, int32_t *out_exit_status);
int process_kill(uint32_t pid, int32_t exit_status);
int process_get_exit_status(uint32_t pid, int32_t *out_exit_status);
uint32_t process_wake_by_context(uint32_t context_id);
int process_schedule_once(void);
void process_yield(process_run_result_t result);
void process_tick(void);
int process_should_resched(void);
void process_clear_resched(void);
uint32_t process_count_active(void);
int process_info_at(uint32_t index, uint32_t *out_pid, const char **out_name);
int process_info_at_ex(uint32_t index, uint32_t *out_pid, uint32_t *out_parent_pid, const char **out_name);

#endif
