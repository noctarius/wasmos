#include "kernel_selftest_runtime.h"

#include "ipc.h"
#include "klog.h"
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "process_manager.h"
#include "string.h"
#include "subsystem_registry.h"
#include "wasmos_app.h"
#include "wasmos_driver_abi.h"

typedef struct {
    uint64_t addr;
    uint8_t stage;
} pf_test_state_t;

typedef struct {
    uint32_t endpoint;
    uint32_t sender_endpoint;
    uint32_t sender_ticks;
    uint8_t done;
} ipc_test_state_t;

typedef struct {
    uint8_t observer_runs;
    uint8_t done;
    uint8_t stop_busy;
} preempt_test_state_t;

static pf_test_state_t g_pf_test_state;
static ipc_test_state_t g_ipc_test_state;
static preempt_test_state_t g_preempt_test_state;

typedef struct {
    uint32_t endpoint;
    uint32_t request_count;
} broker_stub_state_t;

typedef struct {
    uint32_t reply_endpoint;
    uint32_t request_id;
    uint32_t attempts;
    uint8_t phase;
} broker_spawn_request_state_t;

static broker_stub_state_t g_broker_stub_state;
static broker_spawn_request_state_t g_broker_spawn_request_state;

#define BROKER_TEST_REQUEST_TAG "BRTST"
#define BROKER_TEST_HANDLER_NAME "broker-smoke"
#define BROKER_TEST_PATH "/init/apps/broker_smoke.bro"
#define BROKER_TEST_HOST_PATH "/init/apps/broker_host_smoke.wap"
#define BROKER_TEST_HOST_ARGS "from-broker"
#define BROKER_TEST_MAX_ATTEMPTS 64u

static const wasmos_exec_match_node_t g_broker_test_match_nodes[] = {
    {
        .kind = WASMOS_EXEC_MATCH_EXTENSION,
        .left_index = 0u,
        .right_index = 0u,
        .value_len = 4u,
        .value.text = ".bro",
    },
};

static int
broker_request_string_at(const uint8_t *buffer,
                         uint32_t buffer_size,
                         uint32_t offset,
                         uint32_t len,
                         const char **out_text)
{
    const char *text = 0;

    if (!buffer || !out_text) {
        return -1;
    }
    *out_text = 0;
    if (len == 0u) {
        return 0;
    }
    if (offset >= buffer_size || len > (buffer_size - offset)) {
        return -1;
    }
    text = (const char *)(buffer + offset);
    for (uint32_t i = 0; i < len; ++i) {
        if (text[i] == '\0') {
            return -1;
        }
    }
    if (offset + len >= buffer_size || text[len] != '\0') {
        return -1;
    }
    *out_text = text;
    return 0;
}

static int
broker_plan_append_string(uint8_t *buffer,
                          uint32_t buffer_size,
                          uint32_t *io_offset,
                          const char *text,
                          uint32_t *out_offset,
                          uint32_t *out_len)
{
    uint32_t len = 0u;
    uint32_t offset = 0u;

    if (!buffer || !io_offset || !text || !out_offset || !out_len) {
        return -1;
    }
    len = (uint32_t)strlen(text);
    offset = *io_offset;
    if (offset >= buffer_size || len + 1u > (buffer_size - offset)) {
        return -1;
    }
    memcpy(buffer + offset, text, len);
    buffer[offset + len] = '\0';
    *out_offset = offset;
    *out_len = len;
    *io_offset = offset + len + 1u;
    return 0;
}

static int
kernel_selftest_register_broker_stub(uint32_t broker_endpoint)
{
    /* TODO: Replace this kernel-owned broker registration path with a
     * user-space registration API once broker services can self-register. */
    if (wasmos_subsystem_register_broker(BROKER_TEST_REQUEST_TAG,
                                         WASMOS_SUBSYSTEM_TAG_NATIVE,
                                         BROKER_TEST_REQUEST_TAG,
                                         broker_endpoint,
                                         0u,
                                         0u,
                                         0u) != 0) {
        return -1;
    }
    if (wasmos_subsystem_registry_register_exec_handler(BROKER_TEST_HANDLER_NAME,
                                                        BROKER_TEST_REQUEST_TAG,
                                                        100u,
                                                        1u,
                                                        g_broker_test_match_nodes,
                                                        1u,
                                                        0u) != 0) {
        return -1;
    }
    return 0;
}

static int
kernel_selftest_process_ready_named(const char *name)
{
    uint32_t active = 0u;

    if (!name || name[0] == '\0') {
        return 0;
    }
    active = process_count_active();
    for (uint32_t i = 0; i < active; ++i) {
        uint32_t pid = 0u;
        const char *proc_name = 0;
        process_t *proc = 0;
        if (process_info_at(i, &pid, &proc_name) != 0 || !proc_name) {
            continue;
        }
        if (strcmp(proc_name, name) != 0) {
            continue;
        }
        proc = process_get(pid);
        if (proc && proc->ready) {
            return 1;
        }
    }
    return 0;
}

static process_run_result_t
page_fault_test_entry(process_t *process, void *arg)
{
    pf_test_state_t *state = (pf_test_state_t *)arg;
    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }

    if (state->stage == 0) {
        mm_context_t *ctx = mm_context_get(process->context_id);
        mem_region_t linear;
        if (!ctx || mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &linear) != 0) {
            klog_write("[test] page fault region lookup failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        state->addr = linear.base;
        if (mm_handle_page_fault(process->context_id, state->addr, 0, 0) != 0) {
            klog_write("[test] page fault seed map failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (paging_unmap_4k(state->addr) != 0) {
            klog_write("[test] page fault unmap failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        state->stage = 1;
    }

    volatile uint8_t *ptr = (volatile uint8_t *)(uintptr_t)state->addr;
    uint8_t value = *ptr;
    *ptr = (uint8_t)(value + 1);
    klog_write("[test] page fault recovered\n");
    process_set_exit_status(process, 0);
    return PROCESS_RUN_EXITED;
}

static process_run_result_t
ipc_wait_test_entry(process_t *process, void *arg)
{
    ipc_test_state_t *state = (ipc_test_state_t *)arg;
    ipc_message_t msg;

    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }
    if (state->done) {
        return PROCESS_RUN_EXITED;
    }
    if (state->endpoint == IPC_ENDPOINT_NONE) {
        klog_write("[test] ipc endpoint missing\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }

    int rc = ipc_recv_blocking_for(process->context_id, state->endpoint, &msg);
    if (rc != IPC_OK) {
        klog_write("[test] ipc recv failed\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }

    klog_write("[test] ipc wake ok\n");
    state->done = 1;
    process_set_exit_status(process, 0);
    return PROCESS_RUN_EXITED;
}

static process_run_result_t
ipc_send_test_entry(process_t *process, void *arg)
{
    ipc_test_state_t *state = (ipc_test_state_t *)arg;
    ipc_message_t msg;

    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }
    if (state->done) {
        return PROCESS_RUN_EXITED;
    }
    if (state->endpoint == IPC_ENDPOINT_NONE || state->sender_endpoint == IPC_ENDPOINT_NONE) {
        klog_write("[test] ipc sender endpoint missing\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }

    if (state->sender_ticks < 3) {
        state->sender_ticks++;
        return PROCESS_RUN_YIELDED;
    }

    msg.type = 1;
    msg.source = state->sender_endpoint;
    msg.destination = IPC_ENDPOINT_NONE;
    msg.request_id = 1;
    msg.arg0 = 0x1234u;
    msg.arg1 = 0;
    msg.arg2 = 0;
    msg.arg3 = 0;
    if (ipc_send_from(process->context_id, state->endpoint, &msg) != IPC_OK) {
        klog_write("[test] ipc send failed\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }

    return PROCESS_RUN_EXITED;
}

static process_run_result_t
preempt_busy_entry(process_t *process, void *arg)
{
    preempt_test_state_t *state = (preempt_test_state_t *)arg;
    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }
    for (;;) {
        if (state->stop_busy) {
            process_set_exit_status(process, 0);
            return PROCESS_RUN_EXITED;
        }
        __asm__ volatile("pause");
    }
}

static process_run_result_t
preempt_observer_entry(process_t *process, void *arg)
{
    preempt_test_state_t *state = (preempt_test_state_t *)arg;

    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }
    if (state->done) {
        return PROCESS_RUN_EXITED;
    }

    state->observer_runs++;
    if (state->observer_runs >= 3) {
        klog_write("[test] preempt ok\n");
        state->done = 1;
        state->stop_busy = 1;
        process_set_exit_status(process, 0);
        return PROCESS_RUN_EXITED;
    }
    return PROCESS_RUN_YIELDED;
}

static process_run_result_t
broker_stub_entry(process_t *process, void *arg)
{
    broker_stub_state_t *state = (broker_stub_state_t *)arg;
    ipc_message_t msg;
    uint32_t pm_context_id = 0u;
    const uint8_t *borrowed_pm_fs = 0;
    const wasmos_broker_spawn_plan_request_t *request = 0;
    const char *request_path = 0;
    uint8_t *broker_fs = 0;
    wasmos_broker_spawn_plan_response_t *plan = 0;
    uint32_t plan_cursor = 0u;
    uint32_t path_offset = 0u;
    uint32_t path_len = 0u;
    uint32_t args_offset = 0u;
    uint32_t args_len = 0u;

    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }
    if (state->endpoint == IPC_ENDPOINT_NONE) {
        klog_write("[test] broker stub endpoint missing\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }
    {
        int recv_rc = ipc_recv_blocking_for(process->context_id, state->endpoint, &msg);
        if (recv_rc == IPC_EMPTY) {
            return PROCESS_RUN_YIELDED;
        }
        if (recv_rc != IPC_OK) {
            klog_write("[test] broker stub recv failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
    }
    if (msg.type != PROC_BROKER_IPC_SPAWN_PLAN_REQ) {
        return PROCESS_RUN_YIELDED;
    }
    if (ipc_endpoint_owner(msg.source, &pm_context_id) != IPC_OK || pm_context_id == 0u) {
        klog_write("[test] broker stub owner lookup failed\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }
    if (process_manager_buffer_borrow_context(PM_BUFFER_KIND_FILESYSTEM,
                                              process->context_id,
                                              pm_context_id,
                                              PM_BUFFER_BORROW_READ) != 0) {
        klog_write("[test] broker stub borrow failed\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }
    borrowed_pm_fs = (const uint8_t *)process_manager_buffer_for_context(PM_BUFFER_KIND_FILESYSTEM, process->context_id);
    if (!borrowed_pm_fs ||
        msg.arg1 < sizeof(wasmos_broker_spawn_plan_request_t) ||
        msg.arg0 > process_manager_buffer_size(PM_BUFFER_KIND_FILESYSTEM) ||
        msg.arg1 > (process_manager_buffer_size(PM_BUFFER_KIND_FILESYSTEM) - msg.arg0)) {
        (void)process_manager_buffer_release_context(PM_BUFFER_KIND_FILESYSTEM, process->context_id);
        klog_write("[test] broker stub request invalid\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }
    request = (const wasmos_broker_spawn_plan_request_t *)(borrowed_pm_fs + msg.arg0);
    if (request->version != WASMOS_BROKER_SPAWN_PLAN_VERSION ||
        strcmp(request->request_tag, BROKER_TEST_REQUEST_TAG) != 0 ||
        strcmp(request->runtime_tag, WASMOS_SUBSYSTEM_TAG_NATIVE) != 0 ||
        broker_request_string_at(borrowed_pm_fs,
                                 process_manager_buffer_size(PM_BUFFER_KIND_FILESYSTEM),
                                 request->path_offset,
                                 request->path_len,
                                 &request_path) != 0 ||
        !request_path ||
        strcmp(request_path, BROKER_TEST_PATH) != 0) {
        (void)process_manager_buffer_release_context(PM_BUFFER_KIND_FILESYSTEM, process->context_id);
        klog_write("[test] broker stub request mismatch\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }
    (void)process_manager_buffer_release_context(PM_BUFFER_KIND_FILESYSTEM, process->context_id);

    broker_fs = (uint8_t *)process_manager_buffer_for_context(PM_BUFFER_KIND_FILESYSTEM, process->context_id);
    if (!broker_fs) {
        klog_write("[test] broker stub fs buffer missing\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }
    memset(broker_fs, 0, process_manager_buffer_size(PM_BUFFER_KIND_FILESYSTEM));
    plan = (wasmos_broker_spawn_plan_response_t *)broker_fs;
    plan_cursor = (uint32_t)sizeof(*plan);
    if (broker_plan_append_string(broker_fs,
                                  process_manager_buffer_size(PM_BUFFER_KIND_FILESYSTEM),
                                  &plan_cursor,
                                  BROKER_TEST_HOST_PATH,
                                  &path_offset,
                                  &path_len) != 0 ||
        broker_plan_append_string(broker_fs,
                                  process_manager_buffer_size(PM_BUFFER_KIND_FILESYSTEM),
                                  &plan_cursor,
                                  BROKER_TEST_HOST_ARGS,
                                  &args_offset,
                                  &args_len) != 0) {
        klog_write("[test] broker stub plan write failed\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }
    plan->version = WASMOS_BROKER_SPAWN_PLAN_VERSION;
    plan->plan_kind = WASMOS_BROKER_PLAN_KIND_WAP_PATH;
    plan->host_path_offset = path_offset;
    plan->host_path_len = path_len;
    plan->host_args_offset = args_offset;
    plan->host_args_len = args_len;
    memcpy(plan->request_tag, BROKER_TEST_REQUEST_TAG, sizeof(BROKER_TEST_REQUEST_TAG));
    memcpy(plan->runtime_tag, WASMOS_SUBSYSTEM_TAG_NATIVE, sizeof(WASMOS_SUBSYSTEM_TAG_NATIVE));

    msg.type = PROC_BROKER_IPC_SPAWN_PLAN_RESP;
    msg.destination = msg.source;
    msg.source = state->endpoint;
    msg.arg0 = 0u;
    msg.arg1 = plan_cursor;
    msg.arg2 = 0u;
    msg.arg3 = 0u;
    if (ipc_send_from(process->context_id, msg.destination, &msg) != IPC_OK) {
        klog_write("[test] broker stub reply failed\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }
    state->request_count++;
    klog_write("[test] broker stub plan ok\n");
    return PROCESS_RUN_YIELDED;
}

static process_run_result_t
broker_spawn_request_entry(process_t *process, void *arg)
{
    broker_spawn_request_state_t *state = (broker_spawn_request_state_t *)arg;
    ipc_message_t msg;
    uint8_t *fs_buf = 0;
    uint32_t proc_ep = IPC_ENDPOINT_NONE;

    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }
    if (state->reply_endpoint == IPC_ENDPOINT_NONE) {
        if (ipc_endpoint_create(process->context_id, &state->reply_endpoint) != IPC_OK) {
            klog_write("[test] broker spawn reply endpoint failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        state->request_id = 1u;
        state->phase = 0u;
        state->attempts = 0u;
    }

    proc_ep = process_manager_endpoint();
    if (proc_ep == IPC_ENDPOINT_NONE ||
        process_manager_fs_endpoint() == IPC_ENDPOINT_NONE ||
        !kernel_selftest_process_ready_named("device-manager")) {
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == 0u) {
        fs_buf = (uint8_t *)process_manager_buffer_for_context(PM_BUFFER_KIND_FILESYSTEM, process->context_id);
        if (!fs_buf) {
            klog_write("[test] broker spawn fs buffer missing\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        memcpy(fs_buf, BROKER_TEST_PATH, sizeof(BROKER_TEST_PATH) - 1u);
        msg.type = PROC_IPC_SPAWN_PATH;
        msg.source = state->reply_endpoint;
        msg.destination = proc_ep;
        msg.request_id = state->request_id;
        msg.arg0 = PROC_SPAWN_PATH_FLAG_AUTOREAP;
        msg.arg1 = (uint32_t)sizeof(BROKER_TEST_PATH) - 1u;
        msg.arg2 = 0u;
        msg.arg3 = 0u;
        if (ipc_send_from(process->context_id, proc_ep, &msg) != IPC_OK) {
            return PROCESS_RUN_YIELDED;
        }
        state->phase = 1u;
        return PROCESS_RUN_YIELDED;
    }

    {
        int recv_rc = ipc_recv_blocking_for(process->context_id, state->reply_endpoint, &msg);
        if (recv_rc == IPC_EMPTY) {
            return PROCESS_RUN_YIELDED;
        }
        if (recv_rc != IPC_OK) {
            return PROCESS_RUN_YIELDED;
        }
    }
    if (msg.request_id != state->request_id) {
        return PROCESS_RUN_YIELDED;
    }
    if (msg.type == PROC_IPC_RESP) {
        klog_write("[test] broker spawn delegation ok\n");
        process_set_exit_status(process, 0);
        return PROCESS_RUN_EXITED;
    }
    if (msg.type == PROC_IPC_ERROR &&
        state->attempts < BROKER_TEST_MAX_ATTEMPTS &&
        ((uint32_t)msg.arg1 == (uint32_t)PROC_SPAWN_ERR_FS_READ ||
         (uint32_t)msg.arg1 == (uint32_t)PROC_SPAWN_ERR_SPAWN_FAILED)) {
        state->attempts++;
        state->request_id++;
        state->phase = 0u;
        return PROCESS_RUN_YIELDED;
    }

    klog_printf("[test] broker spawn delegation failed err=%016llx\n",
                (unsigned long long)msg.arg1);
    process_set_exit_status(process, -1);
    return PROCESS_RUN_EXITED;
}

int
kernel_selftest_spawn_baseline(uint32_t init_pid, uint8_t preempt_test_enabled)
{
    uint32_t pf_test_pid = 0;
    uint32_t ipc_wait_pid = 0;
    uint32_t ipc_send_pid = 0;
    process_t *ipc_wait_proc = 0;
    process_t *ipc_send_proc = 0;
    uint32_t preempt_busy_pid = 0;
    uint32_t preempt_observer_pid = 0;
    uint32_t broker_stub_pid = 0;
    uint32_t broker_request_pid = 0;
    process_t *broker_stub_proc = 0;

    g_pf_test_state.addr = 0;
    g_pf_test_state.stage = 0;
    if (process_spawn_as(init_pid, "pagefault-test", page_fault_test_entry, &g_pf_test_state, &pf_test_pid) != 0) {
        klog_write("[kernel] page fault test spawn failed\n");
        return -1;
    }

    klog_printf("[kernel] page fault test pid=%016llx\n", (unsigned long long)pf_test_pid);
    /* One-shot self-test: auto-reap on exit so it does not linger as a zombie
     * holding a g_processes[] slot (mirrors the threading self-tests). */
    (void)process_set_auto_reap(pf_test_pid, 1);

    g_ipc_test_state.endpoint = IPC_ENDPOINT_NONE;
    g_ipc_test_state.sender_endpoint = IPC_ENDPOINT_NONE;
    g_ipc_test_state.sender_ticks = 0;
    g_ipc_test_state.done = 0;
    if (process_spawn_as(init_pid, "ipc-wait-test", ipc_wait_test_entry, &g_ipc_test_state, &ipc_wait_pid) != 0 ||
        process_spawn_as(init_pid, "ipc-send-test", ipc_send_test_entry, &g_ipc_test_state, &ipc_send_pid) != 0) {
        klog_write("[kernel] ipc test spawn failed\n");
        return -1;
    }

    ipc_wait_proc = process_get(ipc_wait_pid);
    ipc_send_proc = process_get(ipc_send_pid);
    if (!ipc_wait_proc || !ipc_send_proc) {
        klog_write("[kernel] ipc test lookup failed\n");
        return -1;
    }
    /* One-shot self-tests: auto-reap on exit (see above). */
    (void)process_set_auto_reap(ipc_wait_pid, 1);
    (void)process_set_auto_reap(ipc_send_pid, 1);

    if (ipc_endpoint_create(ipc_wait_proc->context_id, &g_ipc_test_state.endpoint) != IPC_OK ||
        ipc_endpoint_create(ipc_send_proc->context_id, &g_ipc_test_state.sender_endpoint) != IPC_OK) {
        klog_write("[kernel] ipc test endpoint create failed\n");
        return -1;
    }

    g_broker_stub_state.endpoint = IPC_ENDPOINT_NONE;
    g_broker_stub_state.request_count = 0u;
    if (process_spawn_as(init_pid, "broker-stub-test", broker_stub_entry, &g_broker_stub_state, &broker_stub_pid) != 0) {
        klog_write("[kernel] broker stub spawn failed\n");
        return -1;
    }
    broker_stub_proc = process_get(broker_stub_pid);
    if (!broker_stub_proc ||
        ipc_endpoint_create(broker_stub_proc->context_id, &g_broker_stub_state.endpoint) != IPC_OK ||
        kernel_selftest_register_broker_stub(g_broker_stub_state.endpoint) != 0) {
        klog_write("[kernel] broker stub register failed\n");
        return -1;
    }
    g_broker_spawn_request_state.reply_endpoint = IPC_ENDPOINT_NONE;
    g_broker_spawn_request_state.request_id = 1u;
    g_broker_spawn_request_state.attempts = 0u;
    g_broker_spawn_request_state.phase = 0u;
    if (process_spawn_as(init_pid,
                         "broker-spawn-test",
                         broker_spawn_request_entry,
                         &g_broker_spawn_request_state,
                         &broker_request_pid) != 0) {
        klog_write("[kernel] broker spawn request failed\n");
        return -1;
    }
    (void)process_set_auto_reap(broker_request_pid, 1);

    if (preempt_test_enabled) {
        g_preempt_test_state.observer_runs = 0;
        g_preempt_test_state.done = 0;
        g_preempt_test_state.stop_busy = 0;
        if (process_spawn_as(init_pid, "preempt-busy", preempt_busy_entry, &g_preempt_test_state, &preempt_busy_pid) != 0 ||
            process_spawn_as(init_pid, "preempt-observer", preempt_observer_entry, &g_preempt_test_state,
                             &preempt_observer_pid) != 0) {
            klog_write("[kernel] preempt test spawn failed\n");
            return -1;
        }
        (void)process_set_auto_reap(preempt_busy_pid, 1);
        (void)process_set_auto_reap(preempt_observer_pid, 1);
    }

    return 0;
}
