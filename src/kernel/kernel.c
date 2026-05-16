#include "boot.h"
#include "cpu.h"
#include "ipc.h"
#include "memory.h"
#include "memory_service.h"
#include "paging.h"
#include "process.h"
#include "thread.h"
#include "process_manager.h"
#include "syscall.h"
#include "serial.h"
#include "timer.h"
#include "wasmos_app.h"
#include "wasm_chardev.h"
#include "wasm3_probe.h"
#include "wasm3_link.h"
#include "physmem.h"
#include "io.h"
#include "framebuffer.h"
#include "capability.h"
#include "slab.h"
#include "kernel_init_runtime.h"
#include "kernel_boot_runtime.h"
#include "kernel_selftest_runtime.h"

#include <stdint.h>
#include <string.h>
#include "wasm3.h"

/*
 * kernel.c owns the high-level bootstrap choreography after the architecture
 * entry path has established a stack and cleared BSS. The file intentionally
 * keeps policy limited to early bring-up and the kernel-owned init task.
 */

static uint32_t g_chardev_service_endpoint = IPC_ENDPOINT_NONE;
static const boot_info_t *g_boot_info;
static boot_info_t g_boot_info_shadow;
extern const uint8_t _binary_ring3_native_probe_bin_start[];
extern const uint8_t _binary_ring3_native_probe_bin_end[];
extern const uint8_t _binary_ring3_thread_lifecycle_probe_bin_start[];
extern const uint8_t _binary_ring3_thread_lifecycle_probe_bin_end[];

typedef struct {
    uint32_t worker_a_tid;
    uint32_t worker_b_tid;
    uint32_t wait_killer_tid;
    uint32_t wait_target_pid;
    uint32_t wait_join_target_tid;
    uint32_t wait_join_waiter_tid;
    uint8_t wait_join_spawned;
    uint8_t wait_join_blocked;
    uint8_t worker_a_ran;
    uint8_t worker_b_ran;
    uint8_t wait_started;
    uint8_t wait_done;
    uint8_t wait_kill_sent;
    int32_t wait_exit_status;
    uint8_t spawned;
    uint8_t done;
} threading_internal_smoke_state_t;
typedef struct {
    threading_internal_smoke_state_t *state;
    uint8_t which;
} threading_internal_worker_arg_t;
static threading_internal_smoke_state_t g_threading_internal_smoke_state;
static threading_internal_worker_arg_t g_threading_worker_a_arg;
static threading_internal_worker_arg_t g_threading_worker_b_arg;
typedef struct {
    threading_internal_smoke_state_t *state;
    uint8_t role;
} threading_wait_join_worker_arg_t;
static threading_wait_join_worker_arg_t g_threading_wait_join_target_arg;
static threading_wait_join_worker_arg_t g_threading_wait_join_waiter_arg;
typedef struct {
    uint32_t target_tid;
    uint32_t waiter_tid;
    uint8_t waiter_blocked;
    uint8_t target_exited;
    uint8_t waiter_done;
    int32_t waiter_exit_status;
    uint8_t spawned;
    uint8_t done;
} threading_join_order_smoke_state_t;
typedef struct {
    threading_join_order_smoke_state_t *state;
    uint8_t role;
} threading_join_order_worker_arg_t;
static threading_join_order_smoke_state_t g_threading_join_order_smoke_state;
static threading_join_order_worker_arg_t g_threading_join_target_arg;
static threading_join_order_worker_arg_t g_threading_join_waiter_arg;
typedef struct {
    uint32_t endpoint;
    uint32_t sender_endpoint;
    uint32_t receiver_tid;
    uint32_t sender_tid;
    uint32_t sent_count;
    uint32_t recv_count;
    uint32_t recv_empty_count;
    uint8_t sender_done;
    uint8_t spawned;
    uint8_t done;
} threading_ipc_stress_state_t;
typedef struct {
    threading_ipc_stress_state_t *state;
    uint8_t is_sender;
} threading_ipc_worker_arg_t;
static threading_ipc_stress_state_t g_threading_ipc_stress_state;
static threading_ipc_worker_arg_t g_threading_ipc_receiver_arg;
static threading_ipc_worker_arg_t g_threading_ipc_sender_arg;
static const uint8_t g_preempt_test_enabled = 0;
#ifndef WASMOS_RING3_SMOKE_DEFAULT
#define WASMOS_RING3_SMOKE_DEFAULT 0
#endif
#ifndef WASMOS_RING3_THREAD_LIFECYCLE_SMOKE_DEFAULT
#define WASMOS_RING3_THREAD_LIFECYCLE_SMOKE_DEFAULT 0
#endif
/* Keep adversarial smoke probes opt-in for dedicated ring3 test targets; they
 * add noise to normal boot/CLI workflows and are not required for baseline
 * ring3 policy mode. */
static const uint8_t g_ring3_smoke_enabled = WASMOS_RING3_SMOKE_DEFAULT;
static const uint8_t g_ring3_thread_lifecycle_smoke_enabled =
    WASMOS_RING3_THREAD_LIFECYCLE_SMOKE_DEFAULT;

uint8_t
kernel_ring3_smoke_enabled(void)
{
    return g_ring3_smoke_enabled;
}

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
} ring3_fault_policy_state_t;
static ring3_fault_policy_state_t g_ring3_fault_policy_state;
static const uint8_t g_ring3_fault_churn_rounds = 6;

static init_state_t g_init_state;

static int
boot_info_build_shadow(const boot_info_t *src, boot_info_t *dst);
static int
spawn_ring3_fault_ud_probe_process(uint32_t parent_pid, uint32_t *out_pid);
static int
spawn_ring3_fault_gp_probe_process(uint32_t parent_pid, uint32_t *out_pid);
static int
spawn_ring3_fault_bp_probe_process(uint32_t parent_pid, uint32_t *out_pid);
static void
run_shmem_owner_isolation_test(uint32_t owner_context_id, uint32_t foreign_context_id);
static void
run_shmem_misuse_matrix_test(uint32_t owner_context_id, uint32_t foreign_context_id, uint8_t ring3_mode);

static int
boot_info_build_shadow(const boot_info_t *src, boot_info_t *dst)
{
    return kernel_boot_build_bootinfo_shadow(src, dst);
}

static void
run_low_slot_sweep_diagnostic(void)
{
    kernel_boot_run_low_slot_sweep_diagnostic();
}

static int
wasmos_endpoint_resolve(uint32_t owner_context_id,
                        const uint8_t *name,
                        uint32_t name_len,
                        uint32_t rights,
                        uint32_t *out_endpoint)
{
    (void)owner_context_id;
    (void)rights;
    if (!out_endpoint) {
        return -1;
    }
    if (str_eq_bytes(name, name_len, "chardev") &&
        g_chardev_service_endpoint != IPC_ENDPOINT_NONE) {
        *out_endpoint = g_chardev_service_endpoint;
        return 0;
    }
    if (str_eq_bytes(name, name_len, "proc")) {
        uint32_t proc_ep = process_manager_endpoint();
        if (proc_ep != IPC_ENDPOINT_NONE) {
            *out_endpoint = proc_ep;
            return 0;
        }
    }
    if (str_eq_bytes(name, name_len, "block")) {
        uint32_t block_ep = process_manager_block_endpoint();
        if (block_ep != IPC_ENDPOINT_NONE) {
            *out_endpoint = block_ep;
            return 0;
        }
    }
    if (str_eq_bytes(name, name_len, "fs")) {
        uint32_t fs_ep = process_manager_fs_endpoint();
        if (fs_ep != IPC_ENDPOINT_NONE) {
            *out_endpoint = fs_ep;
            return 0;
        }
    }
    return -1;
}

static int
wasmos_capability_grant(uint32_t owner_context_id,
                        const uint8_t *name,
                        uint32_t name_len,
                        uint32_t flags)
{
    if (str_eq_bytes(name, name_len, "ipc.basic")) {
        return 0;
    }
    if (capability_grant_name(owner_context_id, name, name_len, flags) == 0) {
        return 0;
    }
    return -1;
}

static process_run_result_t
chardev_server_entry(process_t *process, void *arg)
{
    (void)process;
    (void)arg;

    int rc = wasm_chardev_run();
    process_set_exit_status(process, rc == 0 ? 0 : -1);
    return PROCESS_RUN_EXITED;
}

static process_run_result_t
idle_entry(process_t *process, void *arg)
{
    (void)process;
    (void)arg;
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static process_run_result_t
threading_internal_worker_entry(process_t *process, uint32_t tid, void *arg)
{
    threading_internal_worker_arg_t *worker_arg = (threading_internal_worker_arg_t *)arg;
    (void)process;
    (void)tid;
    if (!worker_arg || !worker_arg->state) {
        return PROCESS_RUN_EXITED;
    }
    if (worker_arg->which == 0) {
        worker_arg->state->worker_a_ran = 1;
        return PROCESS_RUN_EXITED;
    }
    worker_arg->state->worker_b_ran = 1;
    return PROCESS_RUN_EXITED;
}

static process_run_result_t
threading_wait_join_worker_entry(process_t *process, uint32_t tid, void *arg);

static process_run_result_t
threading_wait_target_entry(process_t *process, void *arg)
{
    threading_internal_smoke_state_t *state = (threading_internal_smoke_state_t *)arg;
    if (!process || !state) {
        return PROCESS_RUN_YIELDED;
    }
    if (!state->wait_join_spawned) {
        if (process_thread_spawn_worker_internal(process->pid,
                                                 "thr-wait-join-target",
                                                 threading_wait_join_worker_entry,
                                                 &g_threading_wait_join_target_arg,
                                                 &state->wait_join_target_tid) != 0 ||
            process_thread_spawn_worker_internal(process->pid,
                                                 "thr-wait-join-waiter",
                                                 threading_wait_join_worker_entry,
                                                 &g_threading_wait_join_waiter_arg,
                                                 &state->wait_join_waiter_tid) != 0) {
            return PROCESS_RUN_EXITED;
        }
        state->wait_join_spawned = 1;
    }
    return PROCESS_RUN_YIELDED;
}

static process_run_result_t
threading_wait_killer_entry(process_t *process, uint32_t tid, void *arg)
{
    threading_internal_smoke_state_t *state = (threading_internal_smoke_state_t *)arg;
    (void)tid;
    if (!process || !state || state->wait_target_pid == 0 || !state->wait_started) {
        return PROCESS_RUN_YIELDED;
    }
    if (!state->wait_join_blocked) {
        return PROCESS_RUN_YIELDED;
    }
    if (!state->wait_kill_sent) {
        if (process_kill(state->wait_target_pid, 42) != 0) {
            return PROCESS_RUN_EXITED;
        }
        state->wait_kill_sent = 1;
    }
    return PROCESS_RUN_EXITED;
}

static process_run_result_t
threading_wait_join_worker_entry(process_t *process, uint32_t tid, void *arg)
{
    threading_wait_join_worker_arg_t *worker_arg = (threading_wait_join_worker_arg_t *)arg;
    int32_t exit_status = -1;
    int join_rc = -1;
    (void)tid;
    if (!process || !worker_arg || !worker_arg->state) {
        return PROCESS_RUN_EXITED;
    }
    if (worker_arg->role == 0) {
        if (!worker_arg->state->wait_join_blocked) {
            return PROCESS_RUN_YIELDED;
        }
        return PROCESS_RUN_YIELDED;
    }
    join_rc = process_thread_join(process, worker_arg->state->wait_join_target_tid, &exit_status);
    if (join_rc > 0) {
        worker_arg->state->wait_join_blocked = 1;
        return PROCESS_RUN_BLOCKED;
    }
    return PROCESS_RUN_EXITED;
}

static process_run_result_t
threading_join_order_worker_entry(process_t *process, uint32_t tid, void *arg)
{
    threading_join_order_worker_arg_t *worker_arg = (threading_join_order_worker_arg_t *)arg;
    int32_t exit_status = -1;
    int join_rc = -1;
    (void)tid;
    if (!process || !worker_arg || !worker_arg->state) {
        return PROCESS_RUN_EXITED;
    }
    if (worker_arg->role == 0) {
        if (!worker_arg->state->waiter_blocked) {
            return PROCESS_RUN_YIELDED;
        }
        worker_arg->state->target_exited = 1;
        process_set_exit_status(process, 11);
        return PROCESS_RUN_THREAD_EXITED;
    }
    join_rc = process_thread_join(process, worker_arg->state->target_tid, &exit_status);
    if (join_rc > 0) {
        worker_arg->state->waiter_blocked = 1;
        return PROCESS_RUN_BLOCKED;
    }
    if (join_rc == 0) {
        worker_arg->state->waiter_done = 1;
        worker_arg->state->waiter_exit_status = exit_status;
        return PROCESS_RUN_EXITED;
    }
    process_set_exit_status(process, -1);
    return PROCESS_RUN_EXITED;
}

static process_run_result_t
threading_join_order_smoke_entry(process_t *process, void *arg)
{
    threading_join_order_smoke_state_t *state = (threading_join_order_smoke_state_t *)arg;
    thread_t *waiter = 0;
    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }
    if (state->done) {
        return PROCESS_RUN_EXITED;
    }
    if (!state->spawned) {
        if (process_thread_spawn_worker_internal(process->pid,
                                                 "thr-join-target",
                                                 threading_join_order_worker_entry,
                                                 &g_threading_join_target_arg,
                                                 &state->target_tid) != 0) {
            serial_write("[test] threading join wake order failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (process_thread_spawn_worker_internal(process->pid,
                                                 "thr-join-waiter",
                                                 threading_join_order_worker_entry,
                                                 &g_threading_join_waiter_arg,
                                                 &state->waiter_tid) != 0) {
            serial_write("[test] threading join wake order failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        g_threading_join_target_arg.state->target_tid = state->target_tid;
        g_threading_join_waiter_arg.state->target_tid = state->target_tid;
        state->spawned = 1;
        return PROCESS_RUN_YIELDED;
    }
    waiter = thread_get(state->waiter_tid);
    if (waiter &&
        waiter->state == THREAD_STATE_ZOMBIE &&
        state->waiter_blocked &&
        state->target_exited &&
        state->waiter_done &&
        state->waiter_exit_status == 11) {
        serial_write("[test] threading join wake order ok\n");
        state->done = 1;
        process_set_exit_status(process, 0);
        return PROCESS_RUN_EXITED;
    }
    return PROCESS_RUN_YIELDED;
}

static process_run_result_t
threading_internal_smoke_entry(process_t *process, void *arg)
{
    threading_internal_smoke_state_t *state = (threading_internal_smoke_state_t *)arg;
    thread_t *b = 0;
    int32_t exit_status = 0;
    int wait_rc = 0;
    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }
    if (state->done) {
        return PROCESS_RUN_EXITED;
    }
    if (!state->spawned) {
        if (process_thread_spawn_worker_internal(process->pid,
                                                 "thr-smoke-a",
                                                 threading_internal_worker_entry,
                                                 &g_threading_worker_a_arg,
                                                 &state->worker_a_tid) != 0 ||
            process_thread_spawn_worker_internal(process->pid,
                                                 "thr-smoke-b",
                                                 threading_internal_worker_entry,
                                                 &g_threading_worker_b_arg,
                                                 &state->worker_b_tid) != 0) {
            serial_write("[test] threading internal worker spawn failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (g_ring3_thread_lifecycle_smoke_enabled) {
            if (process_spawn_as(process->pid,
                                 "thr-wait-target",
                                 threading_wait_target_entry,
                                 state,
                                 &state->wait_target_pid) != 0 ||
                process_thread_spawn_worker_internal(process->pid,
                                                     "thr-wait-killer",
                                                     threading_wait_killer_entry,
                                                     state,
                                                     &state->wait_killer_tid) != 0) {
                serial_write("[test] threading wait/kill setup failed\n");
                process_set_exit_status(process, -1);
                return PROCESS_RUN_EXITED;
            }
        }
        state->spawned = 1;
        return PROCESS_RUN_YIELDED;
    }
    if (g_ring3_thread_lifecycle_smoke_enabled && !state->wait_done) {
        state->wait_started = 1;
        wait_rc = process_wait(process, state->wait_target_pid, &exit_status);
        if (wait_rc == 0) {
            state->wait_done = 1;
            state->wait_exit_status = exit_status;
        } else if (wait_rc < 0) {
            serial_write("[test] threading wait/kill failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        } else {
            return PROCESS_RUN_BLOCKED;
        }
    }
    b = thread_get(state->worker_b_tid);
    if (b &&
        b->state == THREAD_STATE_ZOMBIE &&
        state->worker_a_ran &&
        state->worker_b_ran &&
        (!g_ring3_thread_lifecycle_smoke_enabled ||
         (state->wait_done &&
          state->wait_exit_status == 42 &&
          state->wait_join_blocked &&
          state->wait_kill_sent))) {
        serial_write("[test] threading internal worker ok\n");
        if (g_ring3_thread_lifecycle_smoke_enabled) {
            serial_write("[test] threading join after kill order ok\n");
            serial_write("[test] threading join kill wake ok\n");
            serial_write("[test] threading wait kill wake ok\n");
        }
        state->done = 1;
        process_set_exit_status(process, 0);
        return PROCESS_RUN_EXITED;
    }
    return PROCESS_RUN_YIELDED;
}

static process_run_result_t
threading_ipc_worker_entry(process_t *process, uint32_t tid, void *arg)
{
    threading_ipc_worker_arg_t *worker_arg = (threading_ipc_worker_arg_t *)arg;
    threading_ipc_stress_state_t *state = 0;
    ipc_message_t msg;
    const uint32_t target_messages = 8u;
    if (!process || !worker_arg || !(state = worker_arg->state)) {
        return PROCESS_RUN_EXITED;
    }

    if (worker_arg->is_sender) {
        if (state->recv_empty_count == 0) {
            return PROCESS_RUN_YIELDED;
        }
        if (state->sent_count >= target_messages) {
            state->sender_done = 1;
            return PROCESS_RUN_EXITED;
        }
        msg.type = 1;
        msg.source = state->sender_endpoint;
        msg.destination = IPC_ENDPOINT_NONE;
        msg.request_id = state->sent_count + 1u;
        msg.arg0 = state->sent_count;
        msg.arg1 = 0;
        msg.arg2 = 0;
        msg.arg3 = 0;
        if (ipc_send_from(process->context_id, state->endpoint, &msg) != IPC_OK) {
            return PROCESS_RUN_EXITED;
        }
        state->sent_count++;
        return PROCESS_RUN_YIELDED;
    }

    if (state->recv_count >= target_messages) {
        return PROCESS_RUN_EXITED;
    }
    if (ipc_recv_for(process->context_id, state->endpoint, &msg) == IPC_EMPTY) {
        state->recv_empty_count++;
        return PROCESS_RUN_YIELDED;
    }
    state->recv_count++;
    return PROCESS_RUN_YIELDED;
}

static process_run_result_t
threading_ipc_stress_entry(process_t *process, void *arg)
{
    threading_ipc_stress_state_t *state = (threading_ipc_stress_state_t *)arg;
    thread_t *receiver = 0;
    thread_t *sender = 0;
    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }
    if (state->done) {
        return PROCESS_RUN_EXITED;
    }
    if (!state->spawned) {
        if (ipc_endpoint_create(process->context_id, &state->endpoint) != IPC_OK ||
            ipc_endpoint_create(process->context_id, &state->sender_endpoint) != IPC_OK ||
            process_thread_spawn_worker_internal(process->pid,
                                                 "thr-ipc-recv",
                                                 threading_ipc_worker_entry,
                                                 &g_threading_ipc_receiver_arg,
                                                 &state->receiver_tid) != 0 ||
            process_thread_spawn_worker_internal(process->pid,
                                                 "thr-ipc-send",
                                                 threading_ipc_worker_entry,
                                                 &g_threading_ipc_sender_arg,
                                                 &state->sender_tid) != 0) {
            serial_write("[test] threading ipc stress setup failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        state->spawned = 1;
        return PROCESS_RUN_YIELDED;
    }

    receiver = thread_get(state->receiver_tid);
    sender = thread_get(state->sender_tid);
    if (receiver && sender &&
        receiver->state == THREAD_STATE_ZOMBIE &&
        sender->state == THREAD_STATE_ZOMBIE &&
        state->sender_done &&
        state->sent_count == 8u &&
        state->recv_count == 8u &&
        state->recv_empty_count > 0) {
        serial_write("[test] threading ipc stress ok\n");
        state->done = 1;
        process_set_exit_status(process, 0);
        return PROCESS_RUN_EXITED;
    }
    return PROCESS_RUN_YIELDED;
}

static process_run_result_t
ring3_probe_bootstrap_entry(process_t *process, void *arg)
{
    (void)arg;
    if (process) {
        process_set_exit_status(process, -1);
    }
    return PROCESS_RUN_EXITED;
}

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
        rc = process_get_exit_status(state->fault_pid, &exit_status);
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
        rc = process_get_exit_status(state->fault_write_pid, &exit_status);
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
        rc = process_get_exit_status(state->fault_exec_pid, &exit_status);
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
        rc = process_get_exit_status(state->fault_ud_pid, &exit_status);
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
        rc = process_get_exit_status(state->fault_gp_pid, &exit_status);
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
        rc = process_get_exit_status(state->fault_de_pid, &exit_status);
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
        rc = process_get_exit_status(state->fault_db_pid, &exit_status);
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
        rc = process_get_exit_status(state->fault_bp_pid, &exit_status);
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
        rc = process_get_exit_status(state->fault_of_pid, &exit_status);
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
        rc = process_get_exit_status(state->fault_nm_pid, &exit_status);
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
        rc = process_get_exit_status(state->fault_ss_pid, &exit_status);
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
        rc = process_get_exit_status(state->fault_ac_pid, &exit_status);
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
            if (state->churn_round >= g_ring3_fault_churn_rounds) {
                state->churn_done = 1;
                serial_write("[test] ring3 mixed stress ok\n");
            } else {
                if (state->churn_pid == 0) {
                    int spawn_rc = ((state->churn_round & 1u) == 0u)
                        ? spawn_ring3_fault_ud_probe_process(process->pid, &state->churn_pid)
                        : spawn_ring3_fault_gp_probe_process(process->pid, &state->churn_pid);
                    if (spawn_rc != 0 || state->churn_pid == 0) {
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

static void
run_shmem_owner_isolation_test(uint32_t owner_context_id, uint32_t foreign_context_id)
{
    uint32_t shmem_id = 0;
    uint64_t phys = 0;
    uint64_t pages = 0;

    if (owner_context_id == 0 || foreign_context_id == 0 || owner_context_id == foreign_context_id) {
        return;
    }
    if (mm_shared_create(owner_context_id, 1, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE,
                         &shmem_id, &phys) != 0 || shmem_id == 0 || phys == 0) {
        serial_write("[test] shmem owner setup failed\n");
        return;
    }
    if (mm_shared_retain(owner_context_id, shmem_id) != 0) {
        serial_write("[test] shmem owner setup failed\n");
        return;
    }
    if (mm_shared_get_phys(foreign_context_id, shmem_id, &phys, &pages) == 0 ||
        mm_shared_retain(foreign_context_id, shmem_id) == 0 ||
        mm_shared_release(foreign_context_id, shmem_id) == 0) {
        serial_write("[test] shmem owner deny mismatch\n");
    } else {
        serial_write("[test] shmem owner deny ok\n");
    }
    if (mm_shared_grant(owner_context_id, shmem_id, foreign_context_id) != 0 ||
        mm_shared_get_phys(foreign_context_id, shmem_id, &phys, &pages) != 0 ||
        mm_shared_retain(foreign_context_id, shmem_id) != 0 ||
        mm_shared_release(foreign_context_id, shmem_id) != 0) {
        serial_write("[test] shmem grant allow mismatch\n");
    } else {
        serial_write("[test] shmem grant allow ok\n");
    }
    if (mm_shared_release(owner_context_id, shmem_id) != 0) {
        serial_write("[test] shmem owner cleanup failed\n");
    }
}

static void
run_ring3_shmem_isolation_test(uint32_t owner_context_id, uint32_t foreign_context_id)
{
    uint32_t shmem_id = 0;
    uint64_t phys = 0;
    uint64_t pages = 0;

    if (owner_context_id == 0 || foreign_context_id == 0 || owner_context_id == foreign_context_id) {
        serial_write("[test] ring3 shmem setup failed\n");
        return;
    }
    if (mm_shared_create(owner_context_id, 1, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE,
                         &shmem_id, &phys) != 0 || shmem_id == 0 || phys == 0) {
        serial_write("[test] ring3 shmem setup failed\n");
        return;
    }
    if (mm_shared_retain(owner_context_id, shmem_id) != 0) {
        serial_write("[test] ring3 shmem setup failed\n");
        return;
    }
    if (mm_shared_get_phys(foreign_context_id, shmem_id, &phys, &pages) == 0 ||
        mm_shared_retain(foreign_context_id, shmem_id) == 0 ||
        mm_shared_release(foreign_context_id, shmem_id) == 0) {
        serial_write("[test] ring3 shmem owner deny mismatch\n");
    } else {
        serial_write("[test] ring3 shmem owner deny ok\n");
    }
    if (mm_shared_grant(owner_context_id, shmem_id, foreign_context_id) != 0 ||
        mm_shared_get_phys(foreign_context_id, shmem_id, &phys, &pages) != 0 ||
        mm_shared_retain(foreign_context_id, shmem_id) != 0 ||
        mm_shared_release(foreign_context_id, shmem_id) != 0) {
        serial_write("[test] ring3 shmem grant allow mismatch\n");
    } else {
        serial_write("[test] ring3 shmem grant allow ok\n");
    }
    if (mm_shared_release(owner_context_id, shmem_id) != 0) {
        serial_write("[test] ring3 shmem cleanup failed\n");
    }
}

static void
run_shmem_misuse_matrix_test(uint32_t owner_context_id, uint32_t foreign_context_id, uint8_t ring3_mode)
{
    uint32_t shmem_id = 0;
    uint64_t phys = 0;
    uint64_t pages = 0;
    uint64_t map_base = 0;
    uint8_t ok = 1;
    mm_context_t *foreign_ctx = 0;

    if (owner_context_id == 0 || foreign_context_id == 0 || owner_context_id == foreign_context_id) {
        return;
    }
    foreign_ctx = mm_context_get(foreign_context_id);
    if (!foreign_ctx) {
        ok = 0;
        goto done;
    }

    if (mm_shared_create(owner_context_id, 1, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE,
                         &shmem_id, &phys) != 0 || shmem_id == 0 || phys == 0) {
        ok = 0;
        goto done;
    }
    if (mm_shared_retain(owner_context_id, shmem_id) != 0) {
        ok = 0;
        goto done;
    }

    if (mm_shared_get_phys(foreign_context_id, shmem_id + 0x100, &phys, &pages) == 0 ||
        mm_shared_retain(foreign_context_id, shmem_id + 0x100) == 0 ||
        mm_shared_release(foreign_context_id, shmem_id + 0x100) == 0 ||
        mm_shared_map(foreign_ctx, shmem_id + 0x100, MEM_REGION_FLAG_READ, &map_base) == 0) {
        ok = 0;
    }

    if (mm_shared_grant(foreign_context_id, shmem_id, owner_context_id) == 0 ||
        mm_shared_revoke(foreign_context_id, shmem_id, owner_context_id) == 0) {
        ok = 0;
    }

    if (mm_shared_map(foreign_ctx, shmem_id, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, &map_base) == 0) {
        ok = 0;
    }
    if (mm_shared_grant(owner_context_id, shmem_id, foreign_context_id) != 0) {
        ok = 0;
    }
    if (mm_shared_map(foreign_ctx, shmem_id, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, &map_base) != 0 ||
        map_base == 0 ||
        mm_shared_unmap(foreign_ctx, shmem_id) != 0) {
        ok = 0;
    }
    if (mm_shared_revoke(owner_context_id, shmem_id, foreign_context_id) != 0 ||
        mm_shared_revoke(owner_context_id, shmem_id, foreign_context_id) != 0) {
        ok = 0;
    }
    if (mm_shared_map(foreign_ctx, shmem_id, MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE, &map_base) == 0 ||
        mm_shared_release(foreign_context_id, shmem_id) == 0) {
        ok = 0;
    }

done:
    if (shmem_id) {
        if (mm_shared_release(owner_context_id, shmem_id) != 0) {
            ok = 0;
        }
        if (mm_shared_release(owner_context_id, shmem_id) == 0) {
            ok = 0;
        }
    }
    if (ring3_mode) {
        serial_write(ok ? "[test] ring3 shmem misuse matrix ok\n"
                        : "[test] ring3 shmem misuse matrix mismatch\n");
    } else {
        serial_write(ok ? "[test] shmem misuse matrix ok\n"
                        : "[test] shmem misuse matrix mismatch\n");
    }
}

static int
map_linear_pages(uint64_t root_table,
                 uint64_t virt_base,
                 uint64_t phys_base,
                 uint32_t size,
                 uint32_t map_flags)
{
    if (!root_table || !virt_base || !phys_base || size == 0) {
        return -1;
    }
    uint64_t page_count = (size + 0xFFFULL) / 0x1000ULL;
    for (uint64_t i = 0; i < page_count; ++i) {
        uint64_t v = virt_base + i * 0x1000ULL;
        uint64_t p = phys_base + i * 0x1000ULL;
        (void)paging_unmap_4k_in_root(root_table, v);
        if (paging_map_4k_in_root(root_table, v, p, map_flags) != 0) {
            return -1;
        }
    }
    return 0;
}

static int
spawn_ring3_smoke_process(uint32_t parent_pid, uint32_t *out_pid)
{
    /* Ring3 stress loop:
     * - probe IPC syscall boundary with an invalid notify endpoint (deny path)
     *   and a process-owned notification endpoint (allow path)
     * - probe IPC call boundary with invalid/permission-denied endpoints and a
     *   kernel echo endpoint (allow path)
     * - issue an explicit YIELD syscall from CPL3
     * - execute many GETPID syscalls from CPL3 to exercise timer-IRQ preempt
     *   + trampoline return under repeated user->kernel transitions
     * - exit cleanly once done. */
    static const uint8_t ring3_code[] = {
        0xBF, 0xFF, 0xFF, 0xFF, 0xFF, /* mov edi, 0xFFFFFFFF (invalid ep) */
        0xB8, 0x05, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_NOTIFY */
        0xCD, 0x80,                   /* int 0x80 */
        0x48, 0xBF,                   /* mov rdi, 0x0000000100000000 (arg width invalid) */
        0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0xB8, 0x05, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_NOTIFY */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0x00, 0x00, 0x00, 0x00, /* mov edi, <ring3 notify ep> (patched) */
        0xB8, 0x05, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_NOTIFY */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0x00, 0x00, 0x00, 0x00, /* mov edi, <kernel notify control ep> (patched) */
        0xB8, 0x05, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_NOTIFY */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0xFF, 0xFF, 0xFF, 0xFF, /* mov edi, 0xFFFFFFFF (invalid ep) */
        0xBE, 0x21, 0x43, 0x00, 0x00, /* mov esi, 0x4321 (msg type) */
        0xBA, 0xBE, 0xBA, 0xFE, 0xCA, /* mov edx, 0xCAFEBABE (arg0) */
        0xB8, 0x06, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_CALL */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0x00, 0x00, 0x00, 0x00, /* mov edi, <kernel denied call ep> (patched) */
        0xBE, 0x55, 0x22, 0x00, 0x00, /* mov esi, 0x2255 (msg type) */
        0xBA, 0xCD, 0xAB, 0x00, 0x00, /* mov edx, 0x0000ABCD (arg0) */
        0xB8, 0x06, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_CALL */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0x00, 0x00, 0x00, 0x00, /* mov edi, <process-manager call ep> (patched) */
        0xBE, 0x56, 0x66, 0x00, 0x00, /* mov esi, 0x6656 (control deny probe type) */
        0xBA, 0xC0, 0xDE, 0x00, 0x00, /* mov edx, 0x0000DEC0 (arg0) */
        0xB8, 0x06, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_CALL */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0x00, 0x00, 0x00, 0x00, /* mov edi, <kernel echo call ep> (patched) */
        0xBE, 0x78, 0x56, 0x00, 0x00, /* mov esi, 0x5678 (msg type) */
        0xBA, 0xEF, 0xBE, 0xAD, 0xDE, /* mov edx, 0xDEADBEEF (arg0) */
        0xB8, 0x06, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_CALL */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0x00, 0x00, 0x00, 0x00, /* mov edi, <kernel echo call ep> (patched) */
        0xBE, 0xBC, 0x9A, 0x00, 0x00, /* mov esi, 0x9ABC (correlation probe type) */
        0xBA, 0x78, 0x56, 0x34, 0x12, /* mov edx, 0x12345678 (probe arg0) */
        0xB8, 0x06, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_CALL */
        0xCD, 0x80,                   /* int 0x80 */
        0xBF, 0x00, 0x00, 0x00, 0x00, /* mov edi, <kernel echo call ep> (patched) */
        0xBE, 0xBD, 0x9A, 0x00, 0x00, /* mov esi, 0x9ABD (source-auth probe type) */
        0xBA, 0x0D, 0xF0, 0xAD, 0x0B, /* mov edx, 0x0BADF00D (probe arg0) */
        0xB8, 0x06, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_IPC_CALL */
        0xCD, 0x80,                   /* int 0x80 */
        0xB8, 0x03, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_YIELD */
        0xCD, 0x80,                   /* int 0x80 */
        0xB9, 0x00, 0x40, 0x00, 0x00, /* mov ecx, 16384 */
        0xB8, 0x01, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_GETPID */
        0xCD, 0x80,                   /* int 0x80 */
        0xFF, 0xC9,                   /* dec ecx */
        0x75, 0xF5,                   /* jnz <mov eax, GETPID> */
        0x31, 0xFF,                   /* xor edi, edi (exit status 0) */
        0xB8, 0x02, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_EXIT */
        0xCD, 0x80,                   /* int 0x80 */
        0xEB, 0xFE                    /* should not return: spin if it does */
    };
    uint8_t ring3_code_patched[sizeof(ring3_code)];

    process_t *proc = 0;
    mm_context_t *ctx = 0;
    mem_region_t linear = {0};
    mem_region_t stack = {0};
    uint64_t user_rip = 0;
    uint64_t user_rsp = 0;
    uint32_t ring3_notify_ep = IPC_ENDPOINT_NONE;
    uint32_t ring3_notify_control_ep = IPC_ENDPOINT_NONE;
    uint32_t ring3_call_denied_ep = IPC_ENDPOINT_NONE;
    uint32_t ring3_call_control_ep = IPC_ENDPOINT_NONE;
    uint32_t ring3_call_echo_ep = IPC_ENDPOINT_NONE;

    if (!out_pid) {
        return -1;
    }
    if (process_spawn_as(parent_pid, "ring3-smoke", ring3_probe_bootstrap_entry, 0, out_pid) != 0) {
        return -1;
    }

    proc = process_get(*out_pid);
    if (!proc) {
        return -1;
    }
    if (ipc_notification_create(proc->context_id, &ring3_notify_ep) != IPC_OK ||
        ring3_notify_ep == IPC_ENDPOINT_NONE) {
        return -1;
    }
    if (ipc_notification_create(IPC_CONTEXT_KERNEL, &ring3_notify_control_ep) != IPC_OK ||
        ring3_notify_control_ep == IPC_ENDPOINT_NONE) {
        return -1;
    }
    if (ipc_endpoint_create(IPC_CONTEXT_KERNEL, &ring3_call_denied_ep) != IPC_OK ||
        ring3_call_denied_ep == IPC_ENDPOINT_NONE) {
        return -1;
    }
    if (ipc_endpoint_create(IPC_CONTEXT_KERNEL, &ring3_call_echo_ep) != IPC_OK ||
        ring3_call_echo_ep == IPC_ENDPOINT_NONE) {
        return -1;
    }
    ring3_call_control_ep = process_manager_endpoint();
    if (ring3_call_control_ep == IPC_ENDPOINT_NONE) {
        return -1;
    }
    syscall_set_ipc_call_echo_endpoint(ring3_call_echo_ep);
    syscall_set_ipc_call_control_deny_endpoint(ring3_call_control_ep);
    syscall_set_ipc_notify_control_deny_endpoint(ring3_notify_control_ep);
    ctx = mm_context_get(proc->context_id);
    if (!ctx) {
        return -1;
    }
    if (mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &linear) != 0 ||
        mm_context_region_for_type(ctx, MEM_REGION_STACK, &stack) != 0) {
        return -1;
    }
    if (linear.phys_base == 0 || linear.size < sizeof(ring3_code) || stack.base == 0 || stack.size < 16u) {
        return -1;
    }

    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         (uint32_t)sizeof(ring3_code),
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    memcpy(ring3_code_patched, ring3_code, sizeof(ring3_code_patched));
    /* Patch mov edi immediate for the valid ring3-owned notification endpoint.
     * Layout offset: first mov(5) + mov eax(5) + int80(2) + mov edi opcode(1). */
    {
        const uint32_t ep_imm_off = 30u;
        ring3_code_patched[ep_imm_off + 0] = (uint8_t)(ring3_notify_ep & 0xFFu);
        ring3_code_patched[ep_imm_off + 1] = (uint8_t)((ring3_notify_ep >> 8) & 0xFFu);
        ring3_code_patched[ep_imm_off + 2] = (uint8_t)((ring3_notify_ep >> 16) & 0xFFu);
        ring3_code_patched[ep_imm_off + 3] = (uint8_t)((ring3_notify_ep >> 24) & 0xFFu);
    }
    {
        const uint32_t ep_imm_off = 42u;
        ring3_code_patched[ep_imm_off + 0] = (uint8_t)(ring3_notify_control_ep & 0xFFu);
        ring3_code_patched[ep_imm_off + 1] = (uint8_t)((ring3_notify_control_ep >> 8) & 0xFFu);
        ring3_code_patched[ep_imm_off + 2] = (uint8_t)((ring3_notify_control_ep >> 16) & 0xFFu);
        ring3_code_patched[ep_imm_off + 3] = (uint8_t)((ring3_notify_control_ep >> 24) & 0xFFu);
    }
    {
        const uint32_t ep_imm_off = 76u;
        ring3_code_patched[ep_imm_off + 0] = (uint8_t)(ring3_call_denied_ep & 0xFFu);
        ring3_code_patched[ep_imm_off + 1] = (uint8_t)((ring3_call_denied_ep >> 8) & 0xFFu);
        ring3_code_patched[ep_imm_off + 2] = (uint8_t)((ring3_call_denied_ep >> 16) & 0xFFu);
        ring3_code_patched[ep_imm_off + 3] = (uint8_t)((ring3_call_denied_ep >> 24) & 0xFFu);
    }
    {
        const uint32_t ep_imm_off = 98u;
        ring3_code_patched[ep_imm_off + 0] = (uint8_t)(ring3_call_control_ep & 0xFFu);
        ring3_code_patched[ep_imm_off + 1] = (uint8_t)((ring3_call_control_ep >> 8) & 0xFFu);
        ring3_code_patched[ep_imm_off + 2] = (uint8_t)((ring3_call_control_ep >> 16) & 0xFFu);
        ring3_code_patched[ep_imm_off + 3] = (uint8_t)((ring3_call_control_ep >> 24) & 0xFFu);
    }
    {
        const uint32_t ep_imm_off = 120u;
        ring3_code_patched[ep_imm_off + 0] = (uint8_t)(ring3_call_echo_ep & 0xFFu);
        ring3_code_patched[ep_imm_off + 1] = (uint8_t)((ring3_call_echo_ep >> 8) & 0xFFu);
        ring3_code_patched[ep_imm_off + 2] = (uint8_t)((ring3_call_echo_ep >> 16) & 0xFFu);
        ring3_code_patched[ep_imm_off + 3] = (uint8_t)((ring3_call_echo_ep >> 24) & 0xFFu);
    }
    {
        const uint32_t ep_imm_off = 142u;
        ring3_code_patched[ep_imm_off + 0] = (uint8_t)(ring3_call_echo_ep & 0xFFu);
        ring3_code_patched[ep_imm_off + 1] = (uint8_t)((ring3_call_echo_ep >> 8) & 0xFFu);
        ring3_code_patched[ep_imm_off + 2] = (uint8_t)((ring3_call_echo_ep >> 16) & 0xFFu);
        ring3_code_patched[ep_imm_off + 3] = (uint8_t)((ring3_call_echo_ep >> 24) & 0xFFu);
    }
    {
        const uint32_t ep_imm_off = 164u;
        ring3_code_patched[ep_imm_off + 0] = (uint8_t)(ring3_call_echo_ep & 0xFFu);
        ring3_code_patched[ep_imm_off + 1] = (uint8_t)((ring3_call_echo_ep >> 8) & 0xFFu);
        ring3_code_patched[ep_imm_off + 2] = (uint8_t)((ring3_call_echo_ep >> 16) & 0xFFu);
        ring3_code_patched[ep_imm_off + 3] = (uint8_t)((ring3_call_echo_ep >> 24) & 0xFFu);
    }
    if (mm_copy_to_user(proc->context_id,
                        linear.base,
                        ring3_code_patched,
                        (uint32_t)sizeof(ring3_code_patched)) != 0) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         (uint32_t)sizeof(ring3_code),
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_EXEC | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        mem_region_t *region = &ctx->regions[i];
        if (region->type == MEM_REGION_WASM_LINEAR) {
            region->flags |= MEM_REGION_FLAG_EXEC;
            region->flags &= ~MEM_REGION_FLAG_WRITE;
            break;
        }
    }

    user_rip = linear.base;
    user_rsp = stack.base + stack.size - 16u;
    if (process_set_user_entry(*out_pid, user_rip, user_rsp) != 0) {
        return -1;
    }

    serial_printf("[kernel] ring3 smoke pid=%016llx\n", (unsigned long long)*out_pid);
    return 0;
}

static int
spawn_ring3_native_probe_process(uint32_t parent_pid, uint32_t *out_pid)
{
    process_t *proc = 0;
    mm_context_t *ctx = 0;
    mem_region_t linear = {0};
    mem_region_t stack = {0};
    uint64_t user_rip = 0;
    uint64_t user_rsp = 0;
    const uint8_t *src = _binary_ring3_native_probe_bin_start;
    uint32_t code_size = (uint32_t)((uintptr_t)_binary_ring3_native_probe_bin_end -
                                    (uintptr_t)_binary_ring3_native_probe_bin_start);

    if (!out_pid || !src || code_size == 0) {
        return -1;
    }
    if (process_spawn_as(parent_pid, "ring3-native", ring3_probe_bootstrap_entry, 0, out_pid) != 0) {
        return -1;
    }
    proc = process_get(*out_pid);
    if (!proc) {
        return -1;
    }
    ctx = mm_context_get(proc->context_id);
    if (!ctx) {
        return -1;
    }
    if (mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &linear) != 0 ||
        mm_context_region_for_type(ctx, MEM_REGION_STACK, &stack) != 0) {
        return -1;
    }
    if (linear.phys_base == 0 || linear.size < code_size || stack.base == 0 || stack.size < 16u) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    if (mm_copy_to_user(proc->context_id, linear.base, src, code_size) != 0) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_EXEC | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        mem_region_t *region = &ctx->regions[i];
        if (region->type == MEM_REGION_WASM_LINEAR) {
            region->flags |= MEM_REGION_FLAG_EXEC;
            region->flags &= ~MEM_REGION_FLAG_WRITE;
            break;
        }
    }
    user_rip = linear.base;
    user_rsp = stack.base + stack.size - 16u;
    if (process_set_user_entry(*out_pid, user_rip, user_rsp) != 0) {
        return -1;
    }
    serial_printf("[kernel] ring3 native pid=%016llx\n", (unsigned long long)*out_pid);
    return 0;
}

static int
spawn_ring3_thread_lifecycle_probe_process(uint32_t parent_pid, uint32_t *out_pid)
{
    process_t *proc = 0;
    mm_context_t *ctx = 0;
    mem_region_t linear = {0};
    mem_region_t stack = {0};
    uint64_t user_rip = 0;
    uint64_t user_rsp = 0;
    const uint8_t *src = _binary_ring3_thread_lifecycle_probe_bin_start;
    uint32_t code_size = (uint32_t)((uintptr_t)_binary_ring3_thread_lifecycle_probe_bin_end -
                                    (uintptr_t)_binary_ring3_thread_lifecycle_probe_bin_start);

    if (!out_pid || !src || code_size == 0) {
        return -1;
    }
    if (process_spawn_as(parent_pid, "ring3-threading", ring3_probe_bootstrap_entry, 0, out_pid) != 0) {
        return -1;
    }
    proc = process_get(*out_pid);
    if (!proc) {
        return -1;
    }
    ctx = mm_context_get(proc->context_id);
    if (!ctx) {
        return -1;
    }
    if (mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &linear) != 0 ||
        mm_context_region_for_type(ctx, MEM_REGION_STACK, &stack) != 0) {
        return -1;
    }
    if (linear.phys_base == 0 || linear.size < code_size || stack.base == 0 || stack.size < 16u) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    if (mm_copy_to_user(proc->context_id, linear.base, src, code_size) != 0) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_EXEC | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        mem_region_t *region = &ctx->regions[i];
        if (region->type == MEM_REGION_WASM_LINEAR) {
            region->flags |= MEM_REGION_FLAG_EXEC;
            region->flags &= ~MEM_REGION_FLAG_WRITE;
            break;
        }
    }
    user_rip = linear.base;
    user_rsp = stack.base + stack.size - 16u;
    if (process_set_user_entry(*out_pid, user_rip, user_rsp) != 0) {
        return -1;
    }
    serial_printf("[kernel] ring3 threading pid=%016llx\n", (unsigned long long)*out_pid);
    return 0;
}

static int
spawn_ring3_fault_probe_named(uint32_t parent_pid,
                              const char *name,
                              const uint8_t *code,
                              uint32_t code_size,
                              uint32_t *out_pid);

static int
spawn_ring3_fault_probe_process(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t ring3_fault_code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,       /* mov eax, WASMOS_SYSCALL_GETPID */
        0xCD, 0x80,                         /* int 0x80 */
        0x48, 0x8B, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, /* mov rax, [0] */
        0xEB, 0xFE                          /* spin if fault unexpectedly returns */
    };
    return spawn_ring3_fault_probe_named(parent_pid,
                                         "ring3-fault",
                                         ring3_fault_code,
                                         (uint32_t)sizeof(ring3_fault_code),
                                         out_pid);
}

static int
spawn_ring3_fault_write_probe_process(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t ring3_fault_write_code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,       /* mov eax, WASMOS_SYSCALL_GETPID */
        0xCD, 0x80,                         /* int 0x80 */
        0xC7, 0x05, 0x00, 0x00, 0x00, 0x00, /* mov dword ptr [rip+0], imm32 */
        0x34, 0x12, 0x00, 0x00,             /*   0x1234 */
        0xEB, 0xFE                          /* spin if fault unexpectedly returns */
    };
    return spawn_ring3_fault_probe_named(parent_pid,
                                         "ring3-fault-write",
                                         ring3_fault_write_code,
                                         (uint32_t)sizeof(ring3_fault_write_code),
                                         out_pid);
}

static int
spawn_ring3_fault_exec_probe_process(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t ring3_fault_exec_code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_GETPID */
        0xCD, 0x80,                   /* int 0x80 */
        0x50,                         /* push rax (touch/map stack page) */
        0x48, 0x8D, 0x44, 0x24, 0x00, /* lea rax, [rsp+0] */
        0xFF, 0xE0,                   /* jmp rax (stack is mapped but non-exec) */
        0xEB, 0xFE                    /* spin if fault unexpectedly returns */
    };
    return spawn_ring3_fault_probe_named(parent_pid,
                                         "ring3-fault-exec",
                                         ring3_fault_exec_code,
                                         (uint32_t)sizeof(ring3_fault_exec_code),
                                         out_pid);
}

static int
spawn_ring3_fault_ud_probe_process(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t ring3_fault_ud_code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_GETPID */
        0xCD, 0x80,                   /* int 0x80 */
        0x0F, 0x0B,                   /* ud2 */
        0xEB, 0xFE                    /* spin if fault unexpectedly returns */
    };
    return spawn_ring3_fault_probe_named(parent_pid,
                                         "ring3-fault-ud",
                                         ring3_fault_ud_code,
                                         (uint32_t)sizeof(ring3_fault_ud_code),
                                         out_pid);
}

static int
spawn_ring3_fault_gp_probe_process(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t ring3_fault_gp_code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_GETPID */
        0xCD, 0x80,                   /* int 0x80 */
        0xFA,                         /* cli (privileged in CPL3 -> #GP) */
        0xEB, 0xFE                    /* spin if fault unexpectedly returns */
    };
    return spawn_ring3_fault_probe_named(parent_pid,
                                         "ring3-fault-gp",
                                         ring3_fault_gp_code,
                                         (uint32_t)sizeof(ring3_fault_gp_code),
                                         out_pid);
}

static int
spawn_ring3_fault_de_probe_process(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t ring3_fault_de_code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_GETPID */
        0xCD, 0x80,                   /* int 0x80 */
        0x31, 0xD2,                   /* xor edx, edx */
        0x31, 0xC0,                   /* xor eax, eax */
        0xF7, 0xF2,                   /* div edx -> #DE */
        0xEB, 0xFE                    /* spin if fault unexpectedly returns */
    };
    return spawn_ring3_fault_probe_named(parent_pid,
                                         "ring3-fault-de",
                                         ring3_fault_de_code,
                                         (uint32_t)sizeof(ring3_fault_de_code),
                                         out_pid);
}

static int
spawn_ring3_fault_db_probe_process(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t ring3_fault_db_code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_GETPID */
        0xCD, 0x80,                   /* int 0x80 */
        0xF1,                         /* icebp/int1 -> #DB */
        0xEB, 0xFE                    /* spin if fault unexpectedly returns */
    };
    return spawn_ring3_fault_probe_named(parent_pid,
                                         "ring3-fault-db",
                                         ring3_fault_db_code,
                                         (uint32_t)sizeof(ring3_fault_db_code),
                                         out_pid);
}

static int
spawn_ring3_fault_bp_probe_process(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t ring3_fault_bp_code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_GETPID */
        0xCD, 0x80,                   /* int 0x80 */
        0xCC,                         /* int3 -> #BP */
        0xEB, 0xFE                    /* spin if fault unexpectedly returns */
    };
    return spawn_ring3_fault_probe_named(parent_pid,
                                         "ring3-fault-bp",
                                         ring3_fault_bp_code,
                                         (uint32_t)sizeof(ring3_fault_bp_code),
                                         out_pid);
}

static int
spawn_ring3_fault_of_probe_process(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t ring3_fault_of_code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_GETPID */
        0xCD, 0x80,                   /* int 0x80 */
        0xCD, 0x04,                   /* int 4 (may classify as #OF/#GP) */
        0x0F, 0x0B,                   /* ud2 fallback */
        0xEB, 0xFE                    /* spin if fault unexpectedly returns */
    };
    return spawn_ring3_fault_probe_named(parent_pid,
                                         "ring3-fault-of",
                                         ring3_fault_of_code,
                                         (uint32_t)sizeof(ring3_fault_of_code),
                                         out_pid);
}

static int
spawn_ring3_fault_nm_probe_process(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t ring3_fault_nm_code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, /* mov eax, WASMOS_SYSCALL_GETPID */
        0xCD, 0x80,                   /* int 0x80 */
        0xD9, 0xE8,                   /* fld1 (may classify as #NM) */
        0x0F, 0x0B,                   /* ud2 fallback */
        0xEB, 0xFE                    /* spin if fault unexpectedly returns */
    };
    return spawn_ring3_fault_probe_named(parent_pid,
                                         "ring3-fault-nm",
                                         ring3_fault_nm_code,
                                         (uint32_t)sizeof(ring3_fault_nm_code),
                                         out_pid);
}

static int
spawn_ring3_fault_ss_probe_process(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t ring3_fault_ss_code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,                   /* mov eax, WASMOS_SYSCALL_GETPID */
        0xCD, 0x80,                                     /* int 0x80 */
        0x48, 0xBC, 0x00, 0x00, 0x00, 0x00,             /* movabs rsp, */
        0x00, 0x00, 0x01, 0x00,                         /*   0x0001000000000000 (non-canonical) */
        0x50,                                           /* push rax -> #SS/#GP */
        0x0F, 0x0B,                                     /* ud2 fallback */
        0xEB, 0xFE                                      /* spin if fault unexpectedly returns */
    };
    return spawn_ring3_fault_probe_named(parent_pid,
                                         "ring3-fault-ss",
                                         ring3_fault_ss_code,
                                         (uint32_t)sizeof(ring3_fault_ss_code),
                                         out_pid);
}

static int
spawn_ring3_fault_ac_probe_process(uint32_t parent_pid, uint32_t *out_pid)
{
    static const uint8_t ring3_fault_ac_code[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,             /* mov eax, WASMOS_SYSCALL_GETPID */
        0xCD, 0x80,                               /* int 0x80 */
        0x9C,                                     /* pushfq */
        0x58,                                     /* pop rax */
        0x48, 0x0D, 0x00, 0x00, 0x04, 0x00,       /* or rax, 0x40000 (AC) */
        0x50,                                     /* push rax */
        0x9D,                                     /* popfq */
        0x48, 0x8D, 0x44, 0x24, 0x01,             /* lea rax, [rsp+1] */
        0x8B, 0x00,                               /* mov eax, [rax] (unaligned read) */
        0x0F, 0x0B,                               /* ud2 fallback */
        0xEB, 0xFE                                /* spin if fault unexpectedly returns */
    };
    return spawn_ring3_fault_probe_named(parent_pid,
                                         "ring3-fault-ac",
                                         ring3_fault_ac_code,
                                         (uint32_t)sizeof(ring3_fault_ac_code),
                                         out_pid);
}

static int
spawn_ring3_fault_probe_named(uint32_t parent_pid,
                              const char *name,
                              const uint8_t *code,
                              uint32_t code_size,
                              uint32_t *out_pid)
{

    process_t *proc = 0;
    mm_context_t *ctx = 0;
    mem_region_t linear = {0};
    mem_region_t stack = {0};
    uint64_t stack_top_page_virt = 0;
    uint64_t stack_top_page_phys = 0;
    uint64_t user_rip = 0;
    uint64_t user_rsp = 0;
    if (!out_pid || !name || !code || code_size == 0) {
        return -1;
    }
    if (process_spawn_as(parent_pid, name, ring3_probe_bootstrap_entry, 0, out_pid) != 0) {
        return -1;
    }
    proc = process_get(*out_pid);
    if (!proc) {
        return -1;
    }
    ctx = mm_context_get(proc->context_id);
    if (!ctx) {
        return -1;
    }
    if (mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &linear) != 0 ||
        mm_context_region_for_type(ctx, MEM_REGION_STACK, &stack) != 0) {
        return -1;
    }
    if (linear.phys_base == 0 || linear.size < code_size || stack.base == 0 || stack.size < 16u) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    if (mm_copy_to_user(proc->context_id, linear.base, code, code_size) != 0) {
        return -1;
    }
    if (map_linear_pages(ctx->root_table,
                         linear.base,
                         linear.phys_base,
                         code_size,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_EXEC | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    /* Ensure at least one user stack page is present so exec-fault probes can
     * reach their NX jump path instead of terminating on a non-present stack
     * write first. Keep stack non-executable by mapping RW only. */
    stack_top_page_virt = (stack.base + stack.size - 1u) & ~0xFFFULL;
    stack_top_page_phys = (stack.phys_base + stack.size - 1u) & ~0xFFFULL;
    if (map_linear_pages(ctx->root_table,
                         stack_top_page_virt,
                         stack_top_page_phys,
                         0x1000u,
                         MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < ctx->region_count; ++i) {
        mem_region_t *region = &ctx->regions[i];
        if (region->type == MEM_REGION_WASM_LINEAR) {
            region->flags |= MEM_REGION_FLAG_EXEC;
            region->flags &= ~MEM_REGION_FLAG_WRITE;
            break;
        }
    }
    user_rip = linear.base;
    user_rsp = stack.base + stack.size - 16u;
    if (process_set_user_entry(*out_pid, user_rip, user_rsp) != 0) {
        return -1;
    }
    serial_printf("[kernel] %s pid=%016llx\n", name, (unsigned long long)*out_pid);
    return 0;
}

static void
run_kernel_loop(void)
{
    kernel_boot_run_scheduler_loop();
}

void
kmain(boot_info_t *boot_info)
{
    uint32_t chardev_pid = 0;
    process_t *chardev_proc;
    uint32_t chardev_endpoint = IPC_ENDPOINT_NONE;
    uint32_t mem_service_pid = 0;
    process_t *mem_service_proc = 0;
    uint32_t mem_service_endpoint = IPC_ENDPOINT_NONE;
    uint32_t mem_reply_endpoint = IPC_ENDPOINT_NONE;
    uint32_t threading_internal_smoke_pid = 0;
    uint32_t threading_join_order_smoke_pid = 0;
    uint32_t threading_ipc_stress_pid = 0;
    uint32_t ring3_smoke_pid = 0;
    uint32_t ring3_native_pid = 0;
    uint32_t ring3_threading_pid = 0;
    uint32_t ring3_fault_pid = 0;
    uint32_t ring3_fault_write_pid = 0;
    uint32_t ring3_fault_exec_pid = 0;
    uint32_t ring3_fault_ud_pid = 0;
    uint32_t ring3_fault_gp_pid = 0;
    uint32_t ring3_fault_de_pid = 0;
    uint32_t ring3_fault_db_pid = 0;
    uint32_t ring3_fault_bp_pid = 0;
    uint32_t ring3_fault_of_pid = 0;
    uint32_t ring3_fault_nm_pid = 0;
    uint32_t ring3_fault_ss_pid = 0;
    uint32_t ring3_fault_ac_pid = 0;
    uint32_t ring3_fault_policy_pid = 0;
    uint32_t idle_pid = 0;
    uint32_t init_pid = 0;

    (void)boot_info;

    serial_init();
    serial_write("[kernel] kmain\n");
    if (!boot_info || boot_info->version != BOOT_INFO_VERSION ||
        boot_info->size < sizeof(boot_info_t)) {
        serial_write("[kernel] invalid boot_info\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    serial_printf("[kernel] boot_info version=%016llx\n[kernel] boot_info size=%016llx\n",
        (unsigned long long)boot_info->version,
        (unsigned long long)boot_info->size);
    g_boot_info = boot_info;
    framebuffer_init(boot_info);
    cpu_init();

    mm_init(boot_info);
    serial_enable_high_alias(1);
    cpu_relocate_tables_high();
    capability_init();
    slab_init();
    if (boot_info_build_shadow(boot_info, &g_boot_info_shadow) != 0) {
        serial_write("[kernel] boot_info shadow copy failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    boot_info = &g_boot_info_shadow;
    g_boot_info = boot_info;
    ipc_init();
    process_init();
    wasm3_link_init(boot_info);

    serial_write("[kernel] wasm3 init on-demand\n");
    serial_write("[kernel] boot_info shadow active\n");

    if (process_spawn_idle("idle", idle_entry, 0, &idle_pid) != 0) {
        serial_write("[kernel] idle spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    kernel_init_state_reset(&g_init_state, boot_info);
    if (process_spawn("init", kernel_init_entry, &g_init_state, &init_pid) != 0) {
        serial_write("[kernel] init spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    serial_printf("[kernel] init pid=%016llx\n", (unsigned long long)init_pid);

    if (process_spawn_as(init_pid, "mem-service", memory_service_entry, 0, &mem_service_pid) != 0) {
        serial_write("[kernel] mem service spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    mem_service_proc = process_get(mem_service_pid);
    if (!mem_service_proc) {
        serial_write("[kernel] mem service lookup failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    if (ipc_endpoint_create(mem_service_proc->context_id, &mem_service_endpoint) != IPC_OK ||
        ipc_endpoint_create(IPC_CONTEXT_KERNEL, &mem_reply_endpoint) != IPC_OK) {
        serial_write("[kernel] mem service endpoint create failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    memory_service_register(mem_service_proc->context_id, mem_service_endpoint, mem_reply_endpoint);
    serial_write("[kernel] mem service ready\n");

    if (process_spawn_as(init_pid, "chardev-server", chardev_server_entry, 0, &chardev_pid) != 0) {
        serial_write("[kernel] chardev process spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    serial_printf("[kernel] chardev pid=%016llx\n", (unsigned long long)chardev_pid);

    chardev_proc = process_get(chardev_pid);
    if (!chardev_proc || wasm_chardev_init(chardev_proc->context_id) != 0) {
        serial_write("[kernel] chardev service init failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    if (wasm_chardev_endpoint(&chardev_endpoint) != 0) {
        serial_write("[kernel] chardev endpoint lookup failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    g_chardev_service_endpoint = chardev_endpoint;
    run_shmem_owner_isolation_test(mem_service_proc->context_id, chardev_proc->context_id);
    run_shmem_misuse_matrix_test(mem_service_proc->context_id,
                                 chardev_proc->context_id,
                                 g_ring3_smoke_enabled);

    wasmos_app_set_policy_hooks(wasmos_endpoint_resolve, wasmos_capability_grant);

    if (kernel_selftest_spawn_baseline(init_pid, g_preempt_test_enabled) != 0) {
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    g_threading_internal_smoke_state = (threading_internal_smoke_state_t){0};
    g_threading_worker_a_arg.state = &g_threading_internal_smoke_state;
    g_threading_worker_a_arg.which = 0;
    g_threading_worker_b_arg.state = &g_threading_internal_smoke_state;
    g_threading_worker_b_arg.which = 1;
    g_threading_wait_join_target_arg.state = &g_threading_internal_smoke_state;
    g_threading_wait_join_target_arg.role = 0;
    g_threading_wait_join_waiter_arg.state = &g_threading_internal_smoke_state;
    g_threading_wait_join_waiter_arg.role = 1;
    g_threading_join_order_smoke_state = (threading_join_order_smoke_state_t){0};
    g_threading_join_target_arg.state = &g_threading_join_order_smoke_state;
    g_threading_join_target_arg.role = 0;
    g_threading_join_waiter_arg.state = &g_threading_join_order_smoke_state;
    g_threading_join_waiter_arg.role = 1;
    if (process_spawn_as(init_pid,
                         "threading-internal-smoke",
                         threading_internal_smoke_entry,
                         &g_threading_internal_smoke_state,
                         &threading_internal_smoke_pid) != 0) {
        serial_write("[kernel] threading internal smoke spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    if (process_spawn_as(init_pid,
                         "threading-join-order-smoke",
                         threading_join_order_smoke_entry,
                         &g_threading_join_order_smoke_state,
                         &threading_join_order_smoke_pid) != 0) {
        serial_write("[kernel] threading join-order smoke spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    g_threading_ipc_stress_state = (threading_ipc_stress_state_t){0};
    g_threading_ipc_stress_state.endpoint = IPC_ENDPOINT_NONE;
    g_threading_ipc_stress_state.sender_endpoint = IPC_ENDPOINT_NONE;
    g_threading_ipc_receiver_arg.state = &g_threading_ipc_stress_state;
    g_threading_ipc_receiver_arg.is_sender = 0;
    g_threading_ipc_sender_arg.state = &g_threading_ipc_stress_state;
    g_threading_ipc_sender_arg.is_sender = 1;
    if (process_spawn_as(init_pid,
                         "threading-ipc-stress",
                         threading_ipc_stress_entry,
                         &g_threading_ipc_stress_state,
                         &threading_ipc_stress_pid) != 0) {
        serial_write("[kernel] threading ipc stress spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    if (g_ring3_smoke_enabled) {
        if (spawn_ring3_smoke_process(init_pid, &ring3_smoke_pid) != 0) {
            serial_write("[kernel] ring3 smoke spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (spawn_ring3_native_probe_process(init_pid, &ring3_native_pid) != 0) {
            serial_write("[kernel] ring3 native spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (g_ring3_thread_lifecycle_smoke_enabled) {
            if (spawn_ring3_thread_lifecycle_probe_process(init_pid, &ring3_threading_pid) != 0) {
                serial_write("[kernel] ring3 threading spawn failed\n");
                for (;;) {
                    __asm__ volatile("hlt");
                }
            }
        }
        process_t *ring3_smoke_proc = process_get(ring3_smoke_pid);
        process_t *ring3_native_proc = process_get(ring3_native_pid);
        if (!ring3_smoke_proc || !ring3_native_proc) {
            serial_write("[test] ring3 shmem setup failed\n");
        } else {
            run_ring3_shmem_isolation_test(ring3_smoke_proc->context_id, ring3_native_proc->context_id);
        }
        if (spawn_ring3_fault_probe_process(init_pid, &ring3_fault_pid) != 0) {
            serial_write("[kernel] ring3 fault spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (spawn_ring3_fault_write_probe_process(init_pid, &ring3_fault_write_pid) != 0) {
            serial_write("[kernel] ring3 fault write spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (spawn_ring3_fault_exec_probe_process(init_pid, &ring3_fault_exec_pid) != 0) {
            serial_write("[kernel] ring3 fault exec spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (spawn_ring3_fault_ud_probe_process(init_pid, &ring3_fault_ud_pid) != 0) {
            serial_write("[kernel] ring3 fault ud spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (spawn_ring3_fault_gp_probe_process(init_pid, &ring3_fault_gp_pid) != 0) {
            serial_write("[kernel] ring3 fault gp spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (spawn_ring3_fault_de_probe_process(init_pid, &ring3_fault_de_pid) != 0) {
            serial_write("[kernel] ring3 fault de spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (spawn_ring3_fault_db_probe_process(init_pid, &ring3_fault_db_pid) != 0) {
            serial_write("[kernel] ring3 fault db spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (spawn_ring3_fault_bp_probe_process(init_pid, &ring3_fault_bp_pid) != 0) {
            serial_write("[kernel] ring3 fault bp spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (spawn_ring3_fault_of_probe_process(init_pid, &ring3_fault_of_pid) != 0) {
            serial_write("[kernel] ring3 fault of spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (spawn_ring3_fault_nm_probe_process(init_pid, &ring3_fault_nm_pid) != 0) {
            serial_write("[kernel] ring3 fault nm spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (spawn_ring3_fault_ss_probe_process(init_pid, &ring3_fault_ss_pid) != 0) {
            serial_write("[kernel] ring3 fault ss spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        if (spawn_ring3_fault_ac_probe_process(init_pid, &ring3_fault_ac_pid) != 0) {
            serial_write("[kernel] ring3 fault ac spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        g_ring3_fault_policy_state.fault_pid = ring3_fault_pid;
        g_ring3_fault_policy_state.fault_write_pid = ring3_fault_write_pid;
        g_ring3_fault_policy_state.fault_exec_pid = ring3_fault_exec_pid;
        g_ring3_fault_policy_state.fault_ud_pid = ring3_fault_ud_pid;
        g_ring3_fault_policy_state.fault_gp_pid = ring3_fault_gp_pid;
        g_ring3_fault_policy_state.fault_de_pid = ring3_fault_de_pid;
        g_ring3_fault_policy_state.fault_db_pid = ring3_fault_db_pid;
        g_ring3_fault_policy_state.fault_bp_pid = ring3_fault_bp_pid;
        g_ring3_fault_policy_state.fault_of_pid = ring3_fault_of_pid;
        g_ring3_fault_policy_state.fault_nm_pid = ring3_fault_nm_pid;
        g_ring3_fault_policy_state.fault_ss_pid = ring3_fault_ss_pid;
        g_ring3_fault_policy_state.fault_ac_pid = ring3_fault_ac_pid;
        g_ring3_fault_policy_state.fault_ok = 0;
        g_ring3_fault_policy_state.fault_write_ok = 0;
        g_ring3_fault_policy_state.fault_exec_ok = 0;
        g_ring3_fault_policy_state.fault_ud_ok = 0;
        g_ring3_fault_policy_state.fault_gp_ok = 0;
        g_ring3_fault_policy_state.fault_de_ok = 0;
        g_ring3_fault_policy_state.fault_db_ok = 0;
        g_ring3_fault_policy_state.fault_bp_ok = 0;
        g_ring3_fault_policy_state.fault_of_ok = 0;
        g_ring3_fault_policy_state.fault_nm_ok = 0;
        g_ring3_fault_policy_state.fault_ss_ok = 0;
        g_ring3_fault_policy_state.fault_ac_ok = 0;
        g_ring3_fault_policy_state.containment_ok_logged = 0;
        g_ring3_fault_policy_state.churn_pid = 0;
        g_ring3_fault_policy_state.churn_round = 0;
        g_ring3_fault_policy_state.churn_done = 0;
        g_ring3_fault_policy_state.done = 0;
        if (process_spawn_as(init_pid, "ring3-fault-policy", ring3_fault_policy_entry,
                             &g_ring3_fault_policy_state, &ring3_fault_policy_pid) != 0) {
            serial_write("[kernel] ring3 fault policy spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
    }
    run_low_slot_sweep_diagnostic();

    timer_init(250);
    serial_write("[kernel] interrupts on\n");
    cpu_enable_interrupts();

    serial_write("[kernel] scheduler loop\n");
    run_kernel_loop();
}
