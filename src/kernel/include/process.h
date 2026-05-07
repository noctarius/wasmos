#ifndef WASMOS_PROCESS_H
#define WASMOS_PROCESS_H

#include <stdint.h>
#include <stddef.h>

#define PROCESS_MAX_COUNT 48
#define PROCESS_NAME_MAX 64
// Round-robin scheduler time slice (fixed ticks per run).
#define PROCESS_DEFAULT_SLICE_TICKS 5u
#define PROCESS_STACK_SIZE 524288u
#define PROCESS_CTX_CANARY_VALUE 0xC0FFEE0DD15EA5EULL

typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
    uint64_t cs;
    uint64_t ss;
    uint64_t user_rsp;
    uint64_t root_table;
} process_context_t;

_Static_assert(offsetof(process_context_t, r15) == 0, "process_context_t r15 offset mismatch");
_Static_assert(offsetof(process_context_t, rdi) == 64, "process_context_t rdi offset mismatch");
_Static_assert(offsetof(process_context_t, rsp) == 120, "process_context_t rsp offset mismatch");
_Static_assert(offsetof(process_context_t, rip) == 128, "process_context_t rip offset mismatch");
_Static_assert(offsetof(process_context_t, rflags) == 136, "process_context_t rflags offset mismatch");
_Static_assert(offsetof(process_context_t, cs) == 144, "process_context_t cs offset mismatch");
_Static_assert(offsetof(process_context_t, ss) == 152, "process_context_t ss offset mismatch");
_Static_assert(offsetof(process_context_t, user_rsp) == 160, "process_context_t user_rsp offset mismatch");
_Static_assert(offsetof(process_context_t, root_table) == 168, "process_context_t root_table offset mismatch");

typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t user_rsp;
    uint64_t user_ss;
} irq_frame_t;

_Static_assert(offsetof(irq_frame_t, r15) == 0, "irq_frame_t r15 offset mismatch");
_Static_assert(offsetof(irq_frame_t, rax) == 112, "irq_frame_t rax offset mismatch");
_Static_assert(offsetof(irq_frame_t, rip) == 120, "irq_frame_t rip offset mismatch");
_Static_assert(offsetof(irq_frame_t, user_rsp) == 144, "irq_frame_t user_rsp offset mismatch");

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
    uint32_t main_tid;
    uint32_t thread_count;
    uint32_t live_thread_count;
    uint8_t exiting;
    process_state_t state;
    process_block_reason_t block_reason;
    uint32_t wait_target_pid;
    int32_t exit_status;
    uint32_t time_slice_ticks;
    uint32_t ticks_remaining;
    uint64_t ticks_total;
    uint8_t in_ready_queue;
    uint8_t is_idle;
    uint8_t in_hostcall;
    uint64_t ctx_canary_pre;
    process_context_t ctx;
    uint64_t ctx_canary_post;
    uintptr_t stack_base;
    uintptr_t stack_top;
    uintptr_t stack_alloc_base_phys;
    uint32_t stack_pages;
    process_entry_t entry;
    void *arg;
    char name_storage[PROCESS_NAME_MAX];
    const char *name;
} process_t;

void process_init(void);
int process_spawn(const char *name, process_entry_t entry, void *arg, uint32_t *out_pid);
int process_spawn_as(uint32_t parent_pid, const char *name, process_entry_t entry, void *arg, uint32_t *out_pid);
int process_spawn_idle(const char *name, process_entry_t entry, void *arg, uint32_t *out_pid);
int process_thread_spawn_internal(uint32_t owner_pid, const char *name, uint32_t *out_tid);
process_t *process_get(uint32_t pid);
process_t *process_find_by_context(uint32_t context_id);
uint32_t process_current_pid(void);
void process_set_exit_status(process_t *process, int32_t exit_status);
void process_block_on_ipc(process_t *process);
int process_wait(process_t *process, uint32_t target_pid, int32_t *out_exit_status);
int process_kill(uint32_t pid, int32_t exit_status);
int process_get_exit_status(uint32_t pid, int32_t *out_exit_status);
uint32_t process_wake_by_context(uint32_t context_id);
int process_wake_thread(uint32_t tid);
int process_schedule_once(void);
void process_yield(process_run_result_t result);
void process_tick(void);
int process_should_resched(void);
void process_clear_resched(void);
int process_preempt_from_irq(irq_frame_t *frame);
void preempt_disable(void);
void preempt_enable(void);
int preempt_is_enabled(void);
uint32_t preempt_disable_depth(void);
void critical_section_enter(void);
void critical_section_leave(void);
void preempt_safepoint(void);
void pm_preempt_safe_enter(void);
void pm_preempt_safe_leave(void);
uint64_t process_watchdog_issue_count(void);
uint32_t process_count_active(void);
uint32_t process_ready_count(void);
int process_info_at(uint32_t index, uint32_t *out_pid, const char **out_name);
int process_info_at_ex(uint32_t index, uint32_t *out_pid, uint32_t *out_parent_pid, const char **out_name);
int process_set_user_entry(uint32_t pid, uint64_t rip, uint64_t user_rsp);

#endif
