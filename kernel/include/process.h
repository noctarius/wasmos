#ifndef WASMOS_PROCESS_H
#define WASMOS_PROCESS_H

#include <stdint.h>

#define PROCESS_MAX_COUNT 16

typedef enum {
    PROCESS_STATE_UNUSED = 0,
    PROCESS_STATE_READY,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_BLOCKED,
    PROCESS_STATE_TERMINATED
} process_state_t;

typedef enum {
    PROCESS_RUN_YIELDED = 0,
    PROCESS_RUN_IDLE = 1,
    PROCESS_RUN_BLOCKED = 2,
    PROCESS_RUN_EXITED = 3
} process_run_result_t;

struct process;
typedef process_run_result_t (*process_entry_t)(struct process *process, void *arg);

typedef struct process {
    uint32_t pid;
    uint32_t context_id;
    process_state_t state;
    process_entry_t entry;
    void *arg;
    const char *name;
} process_t;

void process_init(void);
int process_spawn(const char *name, process_entry_t entry, void *arg, uint32_t *out_pid);
process_t *process_get(uint32_t pid);
int process_schedule_once(void);
uint32_t process_count_active(void);

#endif
