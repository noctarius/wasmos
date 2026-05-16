#include "kernel_threading_selftest_runtime.h"

#include "ipc.h"
#include "process.h"
#include "thread.h"
#include "serial.h"

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

typedef struct {
    threading_internal_smoke_state_t *state;
    uint8_t role;
} threading_wait_join_worker_arg_t;

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

static threading_internal_smoke_state_t g_threading_internal_smoke_state;
static threading_internal_worker_arg_t g_threading_worker_a_arg;
static threading_internal_worker_arg_t g_threading_worker_b_arg;
static threading_wait_join_worker_arg_t g_threading_wait_join_target_arg;
static threading_wait_join_worker_arg_t g_threading_wait_join_waiter_arg;
static threading_join_order_smoke_state_t g_threading_join_order_smoke_state;
static threading_join_order_worker_arg_t g_threading_join_target_arg;
static threading_join_order_worker_arg_t g_threading_join_waiter_arg;
static threading_ipc_stress_state_t g_threading_ipc_stress_state;
static threading_ipc_worker_arg_t g_threading_ipc_receiver_arg;
static threading_ipc_worker_arg_t g_threading_ipc_sender_arg;
static uint8_t g_ring3_thread_lifecycle_smoke_enabled = 0;

static process_run_result_t
threading_wait_target_entry(process_t *process, void *arg)
{
    threading_internal_smoke_state_t *state = (threading_internal_smoke_state_t *)arg;
    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }
    for (;;) {
        __asm__ volatile("pause");
    }
}

static process_run_result_t
threading_wait_killer_entry(process_t *process, uint32_t tid, void *arg)
{
    threading_internal_smoke_state_t *state = (threading_internal_smoke_state_t *)arg;
    (void)tid;
    if (!process || !state) {
        return PROCESS_RUN_EXITED;
    }
    if (!state->wait_started) {
        return PROCESS_RUN_YIELDED;
    }
    if (!state->wait_kill_sent) {
        if (process_kill(state->wait_target_pid, 42) != 0) {
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        state->wait_kill_sent = 1;
    }
    process_set_exit_status(process, 0);
    return PROCESS_RUN_THREAD_EXITED;
}

static process_run_result_t
threading_wait_join_worker_entry(process_t *process, uint32_t tid, void *arg)
{
    threading_wait_join_worker_arg_t *worker_arg = (threading_wait_join_worker_arg_t *)arg;
    int32_t exit_status = 0;
    int join_rc = 0;
    (void)tid;
    if (!process || !worker_arg || !worker_arg->state) {
        return PROCESS_RUN_EXITED;
    }
    if (worker_arg->role == 0) {
        if (!worker_arg->state->wait_join_blocked) {
            return PROCESS_RUN_YIELDED;
        }
        process_set_exit_status(process, 42);
        return PROCESS_RUN_THREAD_EXITED;
    }
    if (!worker_arg->state->wait_join_spawned) {
        if (process_thread_spawn_worker_internal(process->pid,
                                                 "thr-wait-join-target",
                                                 threading_wait_join_worker_entry,
                                                 &g_threading_wait_join_target_arg,
                                                 &worker_arg->state->wait_join_target_tid) != 0) {
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        worker_arg->state->wait_join_spawned = 1;
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
threading_internal_worker_entry(process_t *process, uint32_t tid, void *arg)
{
    threading_internal_worker_arg_t *worker_arg = (threading_internal_worker_arg_t *)arg;
    (void)tid;
    if (!process || !worker_arg || !worker_arg->state) {
        return PROCESS_RUN_EXITED;
    }
    if (worker_arg->which == 0) {
        worker_arg->state->worker_a_ran = 1;
        return PROCESS_RUN_THREAD_EXITED;
    }
    if (!worker_arg->state->worker_a_ran) {
        return PROCESS_RUN_YIELDED;
    }
    if (g_ring3_thread_lifecycle_smoke_enabled &&
        !worker_arg->state->wait_join_spawned &&
        !worker_arg->state->wait_join_waiter_tid) {
        if (process_thread_spawn_worker_internal(process->pid,
                                                 "thr-wait-join-waiter",
                                                 threading_wait_join_worker_entry,
                                                 &g_threading_wait_join_waiter_arg,
                                                 &worker_arg->state->wait_join_waiter_tid) != 0) {
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
    }
    worker_arg->state->worker_b_ran = 1;
    return PROCESS_RUN_THREAD_EXITED;
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
    (void)tid;
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

int
kernel_threading_selftest_spawn(uint32_t init_pid, uint8_t ring3_thread_lifecycle_smoke_enabled)
{
    uint32_t threading_internal_smoke_pid = 0;
    uint32_t threading_join_order_smoke_pid = 0;
    uint32_t threading_ipc_stress_pid = 0;

    g_ring3_thread_lifecycle_smoke_enabled = ring3_thread_lifecycle_smoke_enabled;

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
        return -1;
    }
    if (process_spawn_as(init_pid,
                         "threading-join-order-smoke",
                         threading_join_order_smoke_entry,
                         &g_threading_join_order_smoke_state,
                         &threading_join_order_smoke_pid) != 0) {
        serial_write("[kernel] threading join-order smoke spawn failed\n");
        return -1;
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
        return -1;
    }

    return 0;
}
