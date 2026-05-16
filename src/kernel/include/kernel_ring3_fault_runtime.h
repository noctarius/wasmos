#ifndef WASMOS_KERNEL_RING3_FAULT_RUNTIME_H
#define WASMOS_KERNEL_RING3_FAULT_RUNTIME_H

#include <stdint.h>

typedef struct {
    uint32_t fault_pid;
    uint32_t fault_write_pid;
    uint32_t fault_exec_pid;
    uint32_t fault_ud_pid;
    uint32_t fault_gp_pid;
    uint32_t fault_de_pid;
    uint32_t fault_db_pid;
    uint32_t fault_bp_pid;
    uint32_t fault_of_pid;
    uint32_t fault_nm_pid;
    uint32_t fault_ss_pid;
    uint32_t fault_ac_pid;
} ring3_fault_policy_probes_t;

typedef int (*ring3_fault_churn_spawn_fn)(uint32_t parent_pid, uint8_t churn_round, uint32_t *out_pid);

int kernel_ring3_fault_policy_spawn(uint32_t init_pid,
                                    const ring3_fault_policy_probes_t *probes,
                                    uint8_t churn_rounds,
                                    ring3_fault_churn_spawn_fn churn_spawn);

#endif
