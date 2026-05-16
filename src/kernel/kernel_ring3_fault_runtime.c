#include "kernel_ring3_fault_runtime.h"

#include "process.h"
#include "serial.h"

typedef struct {
    ring3_fault_policy_probes_t probes;
    uint8_t fault_ok;
    uint8_t fault_write_ok;
    uint8_t fault_exec_ok;
    uint8_t fault_ud_ok;
    uint8_t fault_gp_ok;
    uint8_t fault_de_ok;
    uint8_t fault_db_ok;
    uint8_t fault_bp_ok;
    uint8_t fault_of_ok;
    uint8_t fault_nm_ok;
    uint8_t fault_ss_ok;
    uint8_t fault_ac_ok;
    uint8_t containment_ok_logged;
    uint32_t churn_pid;
    uint8_t churn_round;
    uint8_t churn_done;
    uint8_t done;
    uint8_t churn_rounds;
} ring3_fault_policy_state_t;

static ring3_fault_policy_state_t g_ring3_fault_policy_state;
static ring3_fault_churn_spawn_fn g_ring3_fault_churn_spawn;

static process_run_result_t
ring3_fault_policy_entry(process_t *process, void *arg)
{
    ring3_fault_policy_state_t *state = (ring3_fault_policy_state_t *)arg;
    int32_t exit_status = 0;
    int rc = 0;

    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }
    if (state->done) {
        return PROCESS_RUN_EXITED;
    }

    if (!state->fault_ok) {
        rc = process_get_exit_status(state->probes.fault_pid, &exit_status);
        if (rc == 0) {
            if (exit_status == -11) {
                state->fault_ok = 1;
                serial_write("[test] ring3 fault exit status ok\n");
            } else {
                serial_write("[test] ring3 fault exit status mismatch\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
    }
    if (!state->fault_write_ok) {
        rc = process_get_exit_status(state->probes.fault_write_pid, &exit_status);
        if (rc == 0) {
            if (exit_status == -11) {
                state->fault_write_ok = 1;
                serial_write("[test] ring3 fault write exit status ok\n");
            } else {
                serial_write("[test] ring3 fault write exit status mismatch\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
    }
    if (!state->fault_exec_ok) {
        rc = process_get_exit_status(state->probes.fault_exec_pid, &exit_status);
        if (rc == 0) {
            if (exit_status == -11) {
                state->fault_exec_ok = 1;
                serial_write("[test] ring3 fault exec exit status ok\n");
            } else {
                serial_write("[test] ring3 fault exec exit status mismatch\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
    }
    if (!state->fault_ud_ok) {
        rc = process_get_exit_status(state->probes.fault_ud_pid, &exit_status);
        if (rc == 0) {
            if (exit_status == -11) {
                state->fault_ud_ok = 1;
                serial_write("[test] ring3 fault ud exit status ok\n");
            } else {
                serial_write("[test] ring3 fault ud exit status mismatch\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
    }
    if (!state->fault_gp_ok) {
        rc = process_get_exit_status(state->probes.fault_gp_pid, &exit_status);
        if (rc == 0) {
            if (exit_status == -11) {
                state->fault_gp_ok = 1;
                serial_write("[test] ring3 fault gp exit status ok\n");
            } else {
                serial_write("[test] ring3 fault gp exit status mismatch\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
    }
    if (!state->fault_de_ok) {
        rc = process_get_exit_status(state->probes.fault_de_pid, &exit_status);
        if (rc == 0) {
            if (exit_status == -11) {
                state->fault_de_ok = 1;
                serial_write("[test] ring3 fault de exit status ok\n");
            } else {
                serial_write("[test] ring3 fault de exit status mismatch\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
    }
    if (!state->fault_db_ok) {
        rc = process_get_exit_status(state->probes.fault_db_pid, &exit_status);
        if (rc == 0) {
            if (exit_status == -11) {
                state->fault_db_ok = 1;
                serial_write("[test] ring3 fault db exit status ok\n");
            } else {
                serial_write("[test] ring3 fault db exit status mismatch\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
    }
    if (!state->fault_bp_ok) {
        rc = process_get_exit_status(state->probes.fault_bp_pid, &exit_status);
        if (rc == 0) {
            if (exit_status == -11) {
                state->fault_bp_ok = 1;
                serial_write("[test] ring3 fault bp exit status ok\n");
            } else {
                serial_write("[test] ring3 fault bp exit status mismatch\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
    }
    if (!state->fault_of_ok) {
        rc = process_get_exit_status(state->probes.fault_of_pid, &exit_status);
        if (rc == 0) {
            if (exit_status == -11) {
                state->fault_of_ok = 1;
                serial_write("[test] ring3 fault of exit status ok\n");
            } else {
                serial_write("[test] ring3 fault of exit status mismatch\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
    }
    if (!state->fault_nm_ok) {
        rc = process_get_exit_status(state->probes.fault_nm_pid, &exit_status);
        if (rc == 0) {
            if (exit_status == -11) {
                state->fault_nm_ok = 1;
                serial_write("[test] ring3 fault nm exit status ok\n");
            } else {
                serial_write("[test] ring3 fault nm exit status mismatch\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
    }
    if (!state->fault_ss_ok) {
        rc = process_get_exit_status(state->probes.fault_ss_pid, &exit_status);
        if (rc == 0) {
            if (exit_status == -11) {
                state->fault_ss_ok = 1;
                serial_write("[test] ring3 fault ss exit status ok\n");
            } else {
                serial_write("[test] ring3 fault ss exit status mismatch\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
    }
    if (!state->fault_ac_ok) {
        rc = process_get_exit_status(state->probes.fault_ac_pid, &exit_status);
        if (rc == 0) {
            if (exit_status == -11) {
                state->fault_ac_ok = 1;
                serial_write("[test] ring3 fault ac exit status ok\n");
            } else {
                serial_write("[test] ring3 fault ac exit status mismatch\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
    }

    if (state->fault_ok && state->fault_write_ok && state->fault_exec_ok &&
        state->fault_ud_ok && state->fault_gp_ok && state->fault_de_ok &&
        state->fault_db_ok && state->fault_bp_ok && state->fault_of_ok && state->fault_nm_ok &&
        state->fault_ss_ok && state->fault_ac_ok) {
        process_t *init_proc = process_get(process->parent_pid);
        if (!init_proc || init_proc->state == PROCESS_STATE_ZOMBIE) {
            serial_write("[test] ring3 containment liveness mismatch\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (!state->containment_ok_logged) {
            state->containment_ok_logged = 1;
            serial_write("[test] ring3 containment liveness ok\n");
        }
        if (!state->churn_done) {
            if (state->churn_round >= state->churn_rounds) {
                state->churn_done = 1;
                serial_write("[test] ring3 mixed stress ok\n");
            } else {
                if (state->churn_pid == 0) {
                    if (!g_ring3_fault_churn_spawn ||
                        g_ring3_fault_churn_spawn(process->pid, state->churn_round, &state->churn_pid) != 0 ||
                        state->churn_pid == 0) {
                        serial_write("[test] ring3 mixed stress spawn failed\n");
                        process_set_exit_status(process, -1);
                        return PROCESS_RUN_EXITED;
                    }
                }
                rc = process_get_exit_status(state->churn_pid, &exit_status);
                if (rc == 0) {
                    if (exit_status != -11) {
                        serial_write("[test] ring3 mixed stress exit status mismatch\n");
                        process_set_exit_status(process, -1);
                        return PROCESS_RUN_EXITED;
                    }
                    if (process_wait(process, state->churn_pid, &exit_status) != 0) {
                        serial_write("[test] ring3 mixed stress reap failed\n");
                        process_set_exit_status(process, -1);
                        return PROCESS_RUN_EXITED;
                    }
                    state->churn_pid = 0;
                    state->churn_round++;
                }
                return PROCESS_RUN_YIELDED;
            }
        }
        if (process_watchdog_issue_count() == 0) {
            serial_write("[test] ring3 watchdog clean ok\n");
        } else {
            serial_write("[test] ring3 watchdog clean mismatch\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        state->done = 1;
        process_set_exit_status(process, 0);
        return PROCESS_RUN_EXITED;
    }
    return PROCESS_RUN_YIELDED;
}

int
kernel_ring3_fault_policy_spawn(uint32_t init_pid,
                                const ring3_fault_policy_probes_t *probes,
                                uint8_t churn_rounds,
                                ring3_fault_churn_spawn_fn churn_spawn)
{
    uint32_t ring3_fault_policy_pid = 0;
    if (!probes) {
        return -1;
    }
    g_ring3_fault_policy_state = (ring3_fault_policy_state_t){0};
    g_ring3_fault_policy_state.probes = *probes;
    g_ring3_fault_policy_state.churn_rounds = churn_rounds;
    g_ring3_fault_churn_spawn = churn_spawn;
    if (process_spawn_as(init_pid,
                         "ring3-fault-policy",
                         ring3_fault_policy_entry,
                         &g_ring3_fault_policy_state,
                         &ring3_fault_policy_pid) != 0) {
        serial_write("[kernel] ring3 fault policy spawn failed\n");
        return -1;
    }
    return 0;
}
