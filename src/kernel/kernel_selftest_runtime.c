#include "kernel_selftest_runtime.h"

#include "ipc.h"
#include "klog.h"
#include "memory.h"
#include "paging.h"
#include "process.h"
#include "process_manager.h"
#include "wasmos_driver_abi.h"
#include "string.h"

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
    uint32_t reply_endpoint;
    uint32_t request_id;
    uint32_t attempts;
    uint8_t phase;
    /* Per-spawn caller buffer owned by this self-test context: it stages the
     * path for PM to read and must stay live until PM has consumed it. Released
     * before the next acquire and finally reclaimed on process exit. */
    xfer_buffer_owner_t caller_buf;
} broker_spawn_request_state_t;

/* Acquire a self-test-owned transfer buffer, write `path` into it at offset 0,
 * and return the (buffer_id<<12)|path_len arg1 encoding the spawn-path IPC
 * protocol expects. Releases any previously-held buffer first. Returns 0 on
 * failure. */
static uint32_t broker_selftest_stage_path(broker_spawn_request_state_t* state, uint32_t context_id,
                                           const char* path, uint32_t path_len) {
    uint64_t phys = 0u;
    uint8_t* buf = 0;

    if (state->caller_buf.buffer.buffer_id != 0u) {
        (void)xfer_buffer_release_owned(&state->caller_buf);
        state->caller_buf.buffer.buffer_id = 0u;
    }
    if (path_len == 0u || path_len > 0xFFFu) {
        return 0u;
    }
    if (xfer_buffer_acquire(BUFFER_KIND_TRANSFER, context_id, path_len, &state->caller_buf) !=
        XFER_BUFFER_OK) {
        return 0u;
    }
    phys = xfer_buffer_object_phys(&state->caller_buf.buffer);
    if (phys == 0u) {
        return 0u;
    }
    buf = ptr_cast(uint8_t, (phys | KERNEL_HIGHER_HALF_BASE));
    memcpy(buf, path, path_len);
    return (state->caller_buf.buffer.buffer_id << 12) | (path_len & 0xFFFu);
}

static broker_spawn_request_state_t g_broker_spawn_request_state;

static const char* kernel_selftest_spawn_error_name(int32_t err) {
    switch (err) {
    case -(int32_t)WASMOS_ERR_PROC_SPAWN_BAD_ENDPOINT:
        return "-(int32_t)WASMOS_ERR_PROC_SPAWN_BAD_ENDPOINT";
    case -(int32_t)WASMOS_ERR_PROC_SPAWN_NO_CALLER:
        return "-(int32_t)WASMOS_ERR_PROC_SPAWN_NO_CALLER";
    case -(int32_t)WASMOS_ERR_PROC_SPAWN_BAD_PATH:
        return "-(int32_t)WASMOS_ERR_PROC_SPAWN_BAD_PATH";
    case -(int32_t)WASMOS_ERR_PROC_SPAWN_CALLER_FSBUF:
        return "-(int32_t)WASMOS_ERR_PROC_SPAWN_CALLER_FSBUF";
    case -(int32_t)WASMOS_ERR_PROC_SPAWN_ARGS_TOOBIG:
        return "-(int32_t)WASMOS_ERR_PROC_SPAWN_ARGS_TOOBIG";
    case -(int32_t)WASMOS_ERR_PROC_SPAWN_NO_PM_FSBUF:
        return "-(int32_t)WASMOS_ERR_PROC_SPAWN_NO_PM_FSBUF";
    case -(int32_t)WASMOS_ERR_PROC_SPAWN_FS_READ:
        return "-(int32_t)WASMOS_ERR_PROC_SPAWN_FS_READ";
    case -(int32_t)WASMOS_ERR_PROC_SPAWN_SPAWN_FAILED:
        return "-(int32_t)WASMOS_ERR_PROC_SPAWN_SPAWN_FAILED";
    case -(int32_t)WASMOS_ERR_PROC_SPAWN_BROKER_IPC:
        return "-(int32_t)WASMOS_ERR_PROC_SPAWN_BROKER_IPC";
    case -(int32_t)WASMOS_ERR_PROC_SPAWN_BROKER_PLAN:
        return "-(int32_t)WASMOS_ERR_PROC_SPAWN_BROKER_PLAN";
    case -(int32_t)WASMOS_ERR_PROC_SPAWN_BROKER_DEFERRED:
        return "-(int32_t)WASMOS_ERR_PROC_SPAWN_BROKER_DEFERRED";
    case -(int32_t)WASMOS_ERR_PROC_PM_BUSY:
        return "-(int32_t)WASMOS_ERR_PROC_PM_BUSY";
    case -(int32_t)WASMOS_ERR_PROC_PM_BAD_ENDPOINT:
        return "-(int32_t)WASMOS_ERR_PROC_PM_BAD_ENDPOINT";
    case -(int32_t)WASMOS_ERR_PROC_PM_NO_CALLER:
        return "-(int32_t)WASMOS_ERR_PROC_PM_NO_CALLER";
    case -(int32_t)WASMOS_ERR_PROC_PM_INVALID_NAME:
        return "-(int32_t)WASMOS_ERR_PROC_PM_INVALID_NAME";
    case -(int32_t)WASMOS_ERR_PROC_PM_INVALID_MODULE:
        return "-(int32_t)WASMOS_ERR_PROC_PM_INVALID_MODULE";
    case -(int32_t)WASMOS_ERR_PROC_PM_FS_UNAVAILABLE:
        return "-(int32_t)WASMOS_ERR_PROC_PM_FS_UNAVAILABLE";
    case -(int32_t)WASMOS_ERR_PROC_PM_FS_REQUEST:
        return "-(int32_t)WASMOS_ERR_PROC_PM_FS_REQUEST";
    case -(int32_t)WASMOS_ERR_PROC_PM_BAD_PATH:
        return "-(int32_t)WASMOS_ERR_PROC_PM_BAD_PATH";
    case -(int32_t)WASMOS_ERR_PROC_PM_PATH_RESOLVE:
        return "-(int32_t)WASMOS_ERR_PROC_PM_PATH_RESOLVE";
    case -(int32_t)WASMOS_ERR_PROC_PM_SPAWN_FAILED:
        return "-(int32_t)WASMOS_ERR_PROC_PM_SPAWN_FAILED";
    case -(int32_t)WASMOS_ERR_PROC_PM_CAPS_APPLY:
        return "-(int32_t)WASMOS_ERR_PROC_PM_CAPS_APPLY";
    case -(int32_t)WASMOS_ERR_PROC_PM_BAD_CAPS:
        return "-(int32_t)WASMOS_ERR_PROC_PM_BAD_CAPS";
    case -(int32_t)WASMOS_ERR_PROC_PM_BAD_USER_PTR:
        return "-(int32_t)WASMOS_ERR_PROC_PM_BAD_USER_PTR";
    case -(int32_t)WASMOS_ERR_PROC_PM_USER_COPY:
        return "-(int32_t)WASMOS_ERR_PROC_PM_USER_COPY";
    case -(int32_t)WASMOS_ERR_PROC_PM_META_LOOKUP:
        return "-(int32_t)WASMOS_ERR_PROC_PM_META_LOOKUP";
    case -(int32_t)WASMOS_ERR_PROC_PM_META_NOT_DRIVER:
        return "-(int32_t)WASMOS_ERR_PROC_PM_META_NOT_DRIVER";
    case -(int32_t)WASMOS_ERR_PROC_PM_META_BAD_INDEX:
        return "-(int32_t)WASMOS_ERR_PROC_PM_META_BAD_INDEX";
    case -(int32_t)WASMOS_ERR_PROC_PM_META_BAD_SOURCE:
        return "-(int32_t)WASMOS_ERR_PROC_PM_META_BAD_SOURCE";
    case -(int32_t)WASMOS_ERR_PROC_PM_CALLER_FSBUF:
        return "-(int32_t)WASMOS_ERR_PROC_PM_CALLER_FSBUF";
    case -(int32_t)WASMOS_ERR_PROC_PM_REPLY_SEND:
        return "-(int32_t)WASMOS_ERR_PROC_PM_REPLY_SEND";
    case -(int32_t)WASMOS_ERR_PROC_PM_FS_REPLY:
        return "-(int32_t)WASMOS_ERR_PROC_PM_FS_REPLY";
    case -(int32_t)WASMOS_ERR_PROC_PM_BAD_BROKER:
        return "-(int32_t)WASMOS_ERR_PROC_PM_BAD_BROKER";
    case -(int32_t)WASMOS_ERR_PROC_PM_BAD_HANDLER:
        return "-(int32_t)WASMOS_ERR_PROC_PM_BAD_HANDLER";
    case -(int32_t)WASMOS_ERR_PROC_PM_SUBSYSTEM_REG:
        return "-(int32_t)WASMOS_ERR_PROC_PM_SUBSYSTEM_REG";
    case -(int32_t)WASMOS_ERR_PROC_PM_HANDLER_REG:
        return "-(int32_t)WASMOS_ERR_PROC_PM_HANDLER_REG";
    case -(int32_t)WASMOS_ERR_PROC_PM_NOT_AUTHORIZED:
        return "-(int32_t)WASMOS_ERR_PROC_PM_NOT_AUTHORIZED";
    default:
        return "UNKNOWN";
    }
}

/* The wamos-script broker realises a delegated `#!` guest script through the
 * standalone wamos-script executor; the requester spawns the broker service,
 * waits for it to register + become ready, then spawns a real `.rc` guest whose
 * shebang line routes it to that broker. */
#define BROKER_TEST_SERVICE_PATH "/init/system/services/wamos_script_broker.wap"
#define BROKER_TEST_PATH "/init/apps/hello.rc"
#define BROKER_TEST_MAX_ATTEMPTS 64u

static int kernel_selftest_process_ready_named(const char* name) {
    uint32_t active = 0u;

    if (!name || name[0] == '\0') {
        return 0;
    }
    active = process_count_active();
    for (uint32_t i = 0; i < active; ++i) {
        uint32_t pid = 0u;
        const char* proc_name = 0;
        process_t* proc = 0;
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

static process_run_result_t page_fault_test_entry(process_t* process, void* arg) {
    pf_test_state_t* state = (pf_test_state_t*)arg;
    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }

    if (state->stage == 0) {
        mm_context_t* ctx = mm_context_get(process->context_id);
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

    volatile uint8_t* ptr = ptr_cast(uint8_t, state->addr);
    uint8_t value = *ptr;
    *ptr = (uint8_t)(value + 1);
    klog_write("[test] page fault recovered\n");
    process_set_exit_status(process, 0);
    return PROCESS_RUN_EXITED;
}

static process_run_result_t ipc_wait_test_entry(process_t* process, void* arg) {
    ipc_test_state_t* state = (ipc_test_state_t*)arg;
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

static process_run_result_t ipc_send_test_entry(process_t* process, void* arg) {
    ipc_test_state_t* state = (ipc_test_state_t*)arg;
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

static process_run_result_t preempt_busy_entry(process_t* process, void* arg) {
    preempt_test_state_t* state = (preempt_test_state_t*)arg;
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

static process_run_result_t preempt_observer_entry(process_t* process, void* arg) {
    preempt_test_state_t* state = (preempt_test_state_t*)arg;

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

static process_run_result_t broker_spawn_request_entry(process_t* process, void* arg) {
    broker_spawn_request_state_t* state = (broker_spawn_request_state_t*)arg;
    ipc_message_t msg;
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
    if (proc_ep == IPC_ENDPOINT_NONE || process_manager_fs_endpoint() == IPC_ENDPOINT_NONE ||
        !kernel_selftest_process_ready_named("font-service")) {
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == 0u) {
        uint32_t arg1 =
            broker_selftest_stage_path(state, process->context_id, BROKER_TEST_SERVICE_PATH,
                                       (uint32_t)sizeof(BROKER_TEST_SERVICE_PATH) - 1u);
        if (arg1 == 0u) {
            klog_write("[test] broker spawn xfer buffer missing\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        msg.type = PROC_IPC_SPAWN_PATH_SYNC;
        msg.source = state->reply_endpoint;
        msg.destination = proc_ep;
        msg.request_id = state->request_id;
        msg.arg0 = 0u;
        msg.arg1 = arg1;
        msg.arg2 = 0u;
        msg.arg3 = 5000u;
        if (ipc_send_from(process->context_id, proc_ep, &msg) != IPC_OK) {
            return PROCESS_RUN_YIELDED;
        }
        state->phase = 1u;
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == 2u) {
        uint32_t arg1 = broker_selftest_stage_path(state, process->context_id, BROKER_TEST_PATH,
                                                   (uint32_t)sizeof(BROKER_TEST_PATH) - 1u);
        if (arg1 == 0u) {
            klog_write("[test] broker spawn xf buffer missing\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        msg.type = PROC_IPC_SPAWN_PATH;
        msg.source = state->reply_endpoint;
        msg.destination = proc_ep;
        msg.request_id = state->request_id;
        msg.arg0 = PROC_SPAWN_PATH_FLAG_AUTOREAP;
        msg.arg1 = arg1;
        msg.arg2 = 0u;
        msg.arg3 = 0u;
        if (ipc_send_from(process->context_id, proc_ep, &msg) != IPC_OK) {
            return PROCESS_RUN_YIELDED;
        }
        state->phase = 3u;
        return PROCESS_RUN_YIELDED;
    }

    int recv_rc = ipc_recv_blocking_for(process->context_id, state->reply_endpoint, &msg);
    if (recv_rc == IPC_EMPTY) {
        return PROCESS_RUN_YIELDED;
    }
    if (recv_rc != IPC_OK) {
        return PROCESS_RUN_YIELDED;
    }

    if (msg.request_id != state->request_id) {
        return PROCESS_RUN_YIELDED;
    }
    if (state->phase == 1u && msg.type == PROC_IPC_RESP) {
        state->request_id++;
        state->phase = 2u;
        return PROCESS_RUN_YIELDED;
    }
    if (state->phase == 3u && msg.type == PROC_IPC_RESP) {
        klog_write("[test] broker spawn delegation ok\n");
        process_set_exit_status(process, 0);
        return PROCESS_RUN_EXITED;
    }
    if (msg.type == PROC_IPC_ERROR && state->attempts < BROKER_TEST_MAX_ATTEMPTS &&
        ((uint32_t)msg.arg1 == (uint32_t)-(int32_t)WASMOS_ERR_PROC_SPAWN_FS_READ ||
         (uint32_t)msg.arg1 == (uint32_t)-(int32_t)WASMOS_ERR_PROC_SPAWN_SPAWN_FAILED ||
         (uint32_t)msg.arg1 == (uint32_t)-(int32_t)WASMOS_ERR_PROC_PM_BUSY)) {
        state->attempts++;
        state->request_id++;
        state->phase = (state->phase == 1u) ? 0u : 2u;
        return PROCESS_RUN_YIELDED;
    }

    klog_printf("[test] broker spawn delegation failed err=%016llx (%s)\n",
                (unsigned long long)msg.arg1, kernel_selftest_spawn_error_name((int32_t)msg.arg1));
    process_set_exit_status(process, -1);
    return PROCESS_RUN_EXITED;
}

int kernel_selftest_spawn_baseline(uint32_t init_pid, uint8_t preempt_test_enabled) {
    uint32_t pf_test_pid = 0;
    uint32_t ipc_wait_pid = 0;
    uint32_t ipc_send_pid = 0;
    process_t* ipc_wait_proc = 0;
    process_t* ipc_send_proc = 0;
    uint32_t preempt_busy_pid = 0;
    uint32_t preempt_observer_pid = 0;

    g_pf_test_state.addr = 0;
    g_pf_test_state.stage = 0;
    if (process_spawn_as(init_pid, "pagefault-test", page_fault_test_entry, &g_pf_test_state,
                         &pf_test_pid) != 0) {
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
    if (process_spawn_as(init_pid, "ipc-wait-test", ipc_wait_test_entry, &g_ipc_test_state,
                         &ipc_wait_pid) != 0 ||
        process_spawn_as(init_pid, "ipc-send-test", ipc_send_test_entry, &g_ipc_test_state,
                         &ipc_send_pid) != 0) {
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
        ipc_endpoint_create(ipc_send_proc->context_id, &g_ipc_test_state.sender_endpoint) !=
            IPC_OK) {
        klog_write("[kernel] ipc test endpoint create failed\n");
        return -1;
    }

    /* Broker spawn self-test: spawn the wamos-script broker and drive a `#!`
     * guest script through it (delegated spawn plan -> standalone executor).
     * Re-enabled now that the sysinit/fontsvc/gfx post-start path is green. */
    g_broker_spawn_request_state.reply_endpoint = IPC_ENDPOINT_NONE;
    g_broker_spawn_request_state.request_id = 1u;
    g_broker_spawn_request_state.attempts = 0u;
    g_broker_spawn_request_state.phase = 0u;

    uint32_t broker_request_pid = 0;
    if (process_spawn_as(init_pid, "broker-spawn-test", broker_spawn_request_entry,
                         &g_broker_spawn_request_state, &broker_request_pid) != 0) {
        klog_write("[kernel] broker spawn request failed\n");
        return -1;
    }
    (void)process_set_auto_reap(broker_request_pid, 1);

    if (preempt_test_enabled) {
        g_preempt_test_state.observer_runs = 0;
        g_preempt_test_state.done = 0;
        g_preempt_test_state.stop_busy = 0;
        if (process_spawn_as(init_pid, "preempt-busy", preempt_busy_entry, &g_preempt_test_state,
                             &preempt_busy_pid) != 0 ||
            process_spawn_as(init_pid, "preempt-observer", preempt_observer_entry,
                             &g_preempt_test_state, &preempt_observer_pid) != 0) {
            klog_write("[kernel] preempt test spawn failed\n");
            return -1;
        }
        (void)process_set_auto_reap(preempt_busy_pid, 1);
        (void)process_set_auto_reap(preempt_observer_pid, 1);
    }

    return 0;
}
