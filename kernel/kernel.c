#include "boot.h"
#include "cpu.h"
#include "ipc.h"
#include "memory.h"
#include "memory_service.h"
#include "paging.h"
#include "process.h"
#include "serial.h"
#include "wamr_context.h"
#include "wamr_runtime.h"
#include "wasm_chardev.h"
#include "wasm_driver.h"

#include <stdint.h>

typedef struct WASMExecEnv *wasm_exec_env_t;

extern const uint8_t _binary_chardev_client_wasm_start[];
extern const uint8_t _binary_chardev_client_wasm_end[];

typedef struct {
    uint32_t pid;
    uint8_t valid;
    ipc_message_t message;
} wasm_ipc_last_slot_t;

typedef struct {
    uint32_t chardev_endpoint;
    uint8_t started;
    wasm_driver_t driver;
} chardev_wasm_client_state_t;

static wasm_ipc_last_slot_t g_wasm_last_slots[PROCESS_MAX_COUNT];
static chardev_wasm_client_state_t g_chardev_wasm_client;
typedef struct {
    uint64_t addr;
    uint8_t stage;
} pf_test_state_t;
static pf_test_state_t g_pf_test_state;

static void
serial_write_hex64(uint64_t value)
{
    char buf[21];
    static const char hex[] = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(value >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\n';
    buf[19] = '\0';
    serial_write(buf);
}

static uint32_t
wasm_blob_size(const uint8_t *start, const uint8_t *end)
{
    return (uint32_t)((uintptr_t)end - (uintptr_t)start);
}

static void
wasm_ipc_slots_init(void)
{
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        g_wasm_last_slots[i].pid = 0;
        g_wasm_last_slots[i].valid = 0;
    }
}

static wasm_ipc_last_slot_t *
wasm_ipc_slot_for_pid(uint32_t pid)
{
    wasm_ipc_last_slot_t *empty = 0;

    if (pid == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_wasm_last_slots[i].pid == pid) {
            return &g_wasm_last_slots[i];
        }
        if (!empty && g_wasm_last_slots[i].pid == 0) {
            empty = &g_wasm_last_slots[i];
        }
    }

    if (empty) {
        empty->pid = pid;
        empty->valid = 0;
    }
    return empty;
}

static int
current_process_context(uint32_t *out_context_id)
{
    uint32_t pid = process_current_pid();
    process_t *proc = process_get(pid);

    if (!proc || !out_context_id) {
        return -1;
    }

    *out_context_id = proc->context_id;
    return 0;
}

static int32_t
native_ipc_create_endpoint(wasm_exec_env_t exec_env)
{
    uint32_t context_id = 0;
    uint32_t endpoint = IPC_ENDPOINT_NONE;

    (void)exec_env;

    if (current_process_context(&context_id) != 0) {
        return -1;
    }
    if (ipc_endpoint_create(context_id, &endpoint) != IPC_OK) {
        return -1;
    }
    return (int32_t)endpoint;
}

static int32_t
native_ipc_create_notification(wasm_exec_env_t exec_env)
{
    uint32_t context_id = 0;
    uint32_t endpoint = IPC_ENDPOINT_NONE;

    (void)exec_env;

    if (current_process_context(&context_id) != 0) {
        return -1;
    }
    if (ipc_notification_create(context_id, &endpoint) != IPC_OK) {
        return -1;
    }
    return (int32_t)endpoint;
}

static int32_t
native_ipc_send(wasm_exec_env_t exec_env,
                int32_t destination_endpoint,
                int32_t source_endpoint,
                int32_t type,
                int32_t request_id,
                int32_t arg0,
                int32_t arg1)
{
    uint32_t context_id = 0;
    ipc_message_t req;

    (void)exec_env;

    if (destination_endpoint < 0 || source_endpoint < 0) {
        return -1;
    }

    if (current_process_context(&context_id) != 0) {
        return -1;
    }

    req.type = (uint32_t)type;
    req.source = (uint32_t)source_endpoint;
    req.destination = (uint32_t)destination_endpoint;
    req.request_id = (uint32_t)request_id;
    req.arg0 = (uint32_t)arg0;
    req.arg1 = (uint32_t)arg1;
    req.arg2 = 0;
    req.arg3 = 0;

    return ipc_send_from(context_id, (uint32_t)destination_endpoint, &req);
}

static int32_t
native_ipc_recv(wasm_exec_env_t exec_env, int32_t endpoint)
{
    uint32_t context_id = 0;
    uint32_t pid = process_current_pid();
    wasm_ipc_last_slot_t *slot;
    int rc;

    (void)exec_env;

    if (endpoint < 0 || current_process_context(&context_id) != 0) {
        return -1;
    }

    slot = wasm_ipc_slot_for_pid(pid);
    if (!slot) {
        return -1;
    }

    rc = ipc_recv_for(context_id, (uint32_t)endpoint, &slot->message);
    if (rc == IPC_EMPTY) {
        return 0;
    }
    if (rc != IPC_OK) {
        return -1;
    }

    slot->valid = 1;
    return 1;
}

static int32_t
native_ipc_wait(wasm_exec_env_t exec_env, int32_t endpoint)
{
    uint32_t context_id = 0;
    int rc;

    (void)exec_env;

    if (endpoint < 0 || current_process_context(&context_id) != 0) {
        return -1;
    }

    rc = ipc_wait_for(context_id, (uint32_t)endpoint);
    if (rc == IPC_EMPTY) {
        return 0;
    }
    if (rc != IPC_OK) {
        return -1;
    }
    return 1;
}

static int32_t
native_ipc_notify(wasm_exec_env_t exec_env, int32_t endpoint)
{
    uint32_t context_id = 0;

    (void)exec_env;

    if (endpoint < 0 || current_process_context(&context_id) != 0) {
        return -1;
    }
    return ipc_notify_from(context_id, (uint32_t)endpoint) == IPC_OK ? 0 : -1;
}

static int32_t
native_ipc_last_field(wasm_exec_env_t exec_env, int32_t field)
{
    uint32_t pid = process_current_pid();
    wasm_ipc_last_slot_t *slot = wasm_ipc_slot_for_pid(pid);

    (void)exec_env;

    if (!slot || !slot->valid) {
        return -1;
    }

    switch ((uint32_t)field) {
        case WASMOS_IPC_FIELD_TYPE:
            return (int32_t)slot->message.type;
        case WASMOS_IPC_FIELD_REQUEST_ID:
            return (int32_t)slot->message.request_id;
        case WASMOS_IPC_FIELD_ARG0:
            return (int32_t)slot->message.arg0;
        case WASMOS_IPC_FIELD_ARG1:
            return (int32_t)slot->message.arg1;
        case WASMOS_IPC_FIELD_SOURCE:
            return (int32_t)slot->message.source;
        case WASMOS_IPC_FIELD_DESTINATION:
            return (int32_t)slot->message.destination;
        default:
            return -1;
    }
}

static int
register_wasm_ipc_natives(void)
{
    static const wamr_native_symbol_t symbols[] = {
        { "ipc_create_endpoint", native_ipc_create_endpoint, "()i", 0 },
        { "ipc_create_notification", native_ipc_create_notification, "()i", 0 },
        { "ipc_send", native_ipc_send, "(iiiiii)i", 0 },
        { "ipc_recv", native_ipc_recv, "(i)i", 0 },
        { "ipc_wait", native_ipc_wait, "(i)i", 0 },
        { "ipc_notify", native_ipc_notify, "(i)i", 0 },
        { "ipc_last_field", native_ipc_last_field, "(i)i", 0 },
    };

    if (!wamr_register_natives("wasmos", symbols,
                               (uint32_t)(sizeof(symbols) / sizeof(symbols[0])))) {
        serial_write("[kernel] wasm native registration failed\n");
        return -1;
    }

    serial_write("[kernel] wasm native registration ok\n");
    return 0;
}

static process_run_result_t
chardev_server_entry(process_t *process, void *arg)
{
    (void)process;
    (void)arg;

    int rc = wasm_chardev_service_once();
    if (rc == 0) {
        return PROCESS_RUN_YIELDED;
    }
    if (rc == 1) {
        process_block_on_ipc(process);
        return PROCESS_RUN_BLOCKED;
    }
    return PROCESS_RUN_IDLE;
}

static process_run_result_t
chardev_wasm_client_entry(process_t *process, void *arg)
{
    chardev_wasm_client_state_t *state = (chardev_wasm_client_state_t *)arg;
    ipc_message_t step_msg;
    int32_t step_result = WASMOS_WASM_STEP_FAILED;

    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }

    if (!state->started) {
        wasm_driver_manifest_t manifest;
        manifest.name = "chardev-client";
        manifest.module_bytes = _binary_chardev_client_wasm_start;
        manifest.module_size = wasm_blob_size(_binary_chardev_client_wasm_start,
                                              _binary_chardev_client_wasm_end);
        manifest.init_export = 0;
        manifest.dispatch_export = "chardev_client_step";
        manifest.stack_size = 64 * 1024;
        manifest.heap_size = 64 * 1024;

        if (wasm_driver_start(&state->driver, &manifest, process->context_id) != 0) {
            serial_write("[test] wasm client start failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        state->started = 1;
    }

    step_msg.type = 0;
    step_msg.source = 0;
    step_msg.destination = 0;
    step_msg.request_id = 0;
    step_msg.arg0 = state->chardev_endpoint;
    step_msg.arg1 = 0;
    step_msg.arg2 = 0;
    step_msg.arg3 = 0;

    if (wasm_driver_dispatch(&state->driver, &step_msg, &step_result) != 0) {
        serial_write("[test] wasm client dispatch failed\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }

    if (step_result == WASMOS_WASM_STEP_BLOCKED) {
        process_block_on_ipc(process);
        return PROCESS_RUN_BLOCKED;
    }
    if (step_result == WASMOS_WASM_STEP_DONE) {
        serial_write("[test] wasm chardev ipc roundtrip ok\n");
        process_set_exit_status(process, 0);
        return PROCESS_RUN_EXITED;
    }
    if (step_result == WASMOS_WASM_STEP_FAILED) {
        serial_write("[test] wasm chardev ipc roundtrip failed\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }

    return PROCESS_RUN_YIELDED;
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
            serial_write("[test] page fault region lookup failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        state->addr = linear.base;
        if (paging_unmap_4k(state->addr) != 0) {
            serial_write("[test] page fault unmap failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        state->stage = 1;
    }

    volatile uint8_t *ptr = (volatile uint8_t *)(uintptr_t)state->addr;
    uint8_t value = *ptr;
    *ptr = (uint8_t)(value + 1);
    serial_write("[test] page fault recovered\n");
    process_set_exit_status(process, 0);
    return PROCESS_RUN_EXITED;
}

static void
run_kernel_loop(void)
{
    for (;;) {
        if (process_schedule_once() != 0) {
            __asm__ volatile("pause");
        }
    }
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
    uint32_t test_pid = 0;
    uint32_t pf_test_pid = 0;

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
    serial_write("[kernel] boot_info version=");
    serial_write_hex64(boot_info->version);
    serial_write("[kernel] boot_info size=");
    serial_write_hex64(boot_info->size);
    cpu_init();

    mm_init(boot_info);
    ipc_init();
    process_init();
    wasm_ipc_slots_init();

    serial_write("[kernel] wamr init on-demand\n");

    if (process_spawn("mem-service", memory_service_entry, 0, &mem_service_pid) != 0) {
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

    if (process_spawn("chardev-server", chardev_server_entry, 0, &chardev_pid) != 0) {
        serial_write("[kernel] chardev process spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    serial_write("[kernel] chardev pid=");
    serial_write_hex64(chardev_pid);

    chardev_proc = process_get(chardev_pid);
    if (!chardev_proc || wasm_chardev_init(chardev_proc->context_id) != 0) {
        serial_write("[kernel] chardev service init failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    if (register_wasm_ipc_natives() != 0) {
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

    g_chardev_wasm_client.chardev_endpoint = chardev_endpoint;
    g_chardev_wasm_client.started = 0;

    if (process_spawn("chardev-test-client-wasm", chardev_wasm_client_entry,
                      &g_chardev_wasm_client, &test_pid) != 0) {
        serial_write("[kernel] wasm test process spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    serial_write("[kernel] chardev wasm test pid=");
    serial_write_hex64(test_pid);

    g_pf_test_state.addr = 0;
    g_pf_test_state.stage = 0;
    if (process_spawn("pagefault-test", page_fault_test_entry, &g_pf_test_state, &pf_test_pid) != 0) {
        serial_write("[kernel] page fault test spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    serial_write("[kernel] page fault test pid=");
    serial_write_hex64(pf_test_pid);

    serial_write("[kernel] scheduler loop\n");
    run_kernel_loop();
}
