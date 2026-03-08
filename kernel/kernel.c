#include "boot.h"
#include "cpu.h"
#include "ipc.h"
#include "memory.h"
#include "memory_service.h"
#include "paging.h"
#include "process.h"
#include "process_manager.h"
#include "serial.h"
#include "wasmos_app.h"
#include "wamr_context.h"
#include "wamr_runtime.h"
#include "wasm_chardev.h"
#include "block_ata.h"
#include "physmem.h"

#include <stdint.h>
#include "wasm_export.h"

typedef struct {
    uint32_t pid;
    uint8_t valid;
    ipc_message_t message;
} wasm_ipc_last_slot_t;

static wasm_ipc_last_slot_t g_wasm_last_slots[PROCESS_MAX_COUNT];
typedef struct {
    uint32_t pid;
    uint64_t buffer_phys;
} wasm_block_slot_t;
static wasm_block_slot_t g_wasm_block_slots[PROCESS_MAX_COUNT];
static uint32_t g_chardev_service_endpoint = IPC_ENDPOINT_NONE;
static uint32_t g_block_service_endpoint = IPC_ENDPOINT_NONE;

typedef struct {
    uint64_t addr;
    uint8_t stage;
} pf_test_state_t;
static pf_test_state_t g_pf_test_state;

typedef struct {
    const boot_info_t *boot_info;
    uint8_t started;
} init_state_t;

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

static int
bytes_eq(const uint8_t *a, uint32_t a_len, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    uint32_t i = 0;
    while (b[i]) {
        if (i >= a_len || a[i] != (uint8_t)b[i]) {
            return 0;
        }
        i++;
    }
    return i == a_len;
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
    if (bytes_eq(name, name_len, "chardev") &&
        g_chardev_service_endpoint != IPC_ENDPOINT_NONE) {
        *out_endpoint = g_chardev_service_endpoint;
        return 0;
    }
    if (bytes_eq(name, name_len, "proc")) {
        uint32_t proc_ep = process_manager_endpoint();
        if (proc_ep != IPC_ENDPOINT_NONE) {
            *out_endpoint = proc_ep;
            return 0;
        }
    }
    if (bytes_eq(name, name_len, "block") &&
        g_block_service_endpoint != IPC_ENDPOINT_NONE) {
        *out_endpoint = g_block_service_endpoint;
        return 0;
    }
    if (bytes_eq(name, name_len, "fs")) {
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
    (void)owner_context_id;
    (void)flags;
    if (bytes_eq(name, name_len, "ipc.basic")) {
        return 0;
    }
    return -1;
}

static void
wasm_ipc_slots_init(void)
{
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        g_wasm_last_slots[i].pid = 0;
        g_wasm_last_slots[i].valid = 0;
        g_wasm_block_slots[i].pid = 0;
        g_wasm_block_slots[i].buffer_phys = 0;
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

static wasm_block_slot_t *
wasm_block_slot_for_pid(uint32_t pid)
{
    wasm_block_slot_t *empty = 0;

    if (pid == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_wasm_block_slots[i].pid == pid) {
            return &g_wasm_block_slots[i];
        }
        if (!empty && g_wasm_block_slots[i].pid == 0) {
            empty = &g_wasm_block_slots[i];
        }
    }

    if (empty) {
        empty->pid = pid;
        empty->buffer_phys = 0;
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
                int32_t arg1,
                int32_t arg2,
                int32_t arg3)
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
    req.arg2 = (uint32_t)arg2;
    req.arg3 = (uint32_t)arg3;

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
        case WASMOS_IPC_FIELD_ARG2:
            return (int32_t)slot->message.arg2;
        case WASMOS_IPC_FIELD_ARG3:
            return (int32_t)slot->message.arg3;
        default:
            return -1;
    }
}

#define WASM_BLOCK_BUFFER_PAGES 2u

static int32_t
native_block_buffer_phys(wasm_exec_env_t exec_env)
{
    uint32_t pid = process_current_pid();
    wasm_block_slot_t *slot = wasm_block_slot_for_pid(pid);
    (void)exec_env;

    if (!slot) {
        return -1;
    }
    if (slot->buffer_phys == 0) {
        uint64_t phys = pfa_alloc_pages_below(WASM_BLOCK_BUFFER_PAGES, 0x100000000ULL);
        if (!phys) {
            return -1;
        }
        slot->buffer_phys = phys;
    }

    if (slot->buffer_phys > 0xFFFFFFFFULL) {
        return -1;
    }
    return (int32_t)slot->buffer_phys;
}

static int32_t
native_block_buffer_copy(wasm_exec_env_t exec_env, int32_t phys, int32_t ptr, int32_t len, int32_t offset)
{
    if (ptr < 0 || len <= 0 || offset < 0 || phys <= 0) {
        return -1;
    }

    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    if (!module_inst) {
        return -1;
    }
    if (!wasm_runtime_validate_app_addr(module_inst, (uint64_t)ptr, (uint64_t)len)) {
        return -1;
    }
    uint8_t *dst = (uint8_t *)wasm_runtime_addr_app_to_native(module_inst,
                                                             (uint64_t)ptr);
    if (!dst) {
        return -1;
    }

    const uint8_t *src = (const uint8_t *)(uintptr_t)((uint32_t)phys + (uint32_t)offset);
    for (int32_t i = 0; i < len; ++i) {
        dst[i] = src[i];
    }
    return 0;
}

static int32_t
native_console_write(wasm_exec_env_t exec_env, int32_t ptr, int32_t len)
{
    if (ptr < 0 || len <= 0) {
        return -1;
    }

    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    if (!module_inst) {
        return -1;
    }
    if (!wasm_runtime_validate_app_addr(module_inst, (uint64_t)ptr, (uint64_t)len)) {
        return -1;
    }

    const char *src = (const char *)wasm_runtime_addr_app_to_native(module_inst, (uint64_t)ptr);
    if (!src) {
        return -1;
    }

    char buf[128];
    int32_t remaining = len;
    while (remaining > 0) {
        int32_t chunk = remaining > (int32_t)(sizeof(buf) - 1) ? (int32_t)(sizeof(buf) - 1) : remaining;
        for (int32_t i = 0; i < chunk; ++i) {
            buf[i] = src[len - remaining + i];
        }
        buf[chunk] = '\0';
        serial_write(buf);
        remaining -= chunk;
    }

    return 0;
}

static int32_t
native_console_read(wasm_exec_env_t exec_env, char *ptr, int32_t len)
{
    (void)exec_env;
    if (!ptr || len <= 0) {
        return -1;
    }
    uint8_t ch = 0;
    int rc = serial_read_char(&ch);
    if (rc <= 0) {
        return rc;
    }
    ptr[0] = (char)ch;
    return 1;
}

static int32_t
native_proc_count(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return (int32_t)process_count_active();
}

static int32_t
native_proc_info(wasm_exec_env_t exec_env, int32_t index, char *buf, int32_t buf_len)
{
    (void)exec_env;
    if (index < 0 || !buf || buf_len <= 0) {
        return -1;
    }
    uint32_t pid = 0;
    const char *name = 0;
    if (process_info_at((uint32_t)index, &pid, &name) != 0) {
        return -1;
    }
    int32_t i = 0;
    if (name) {
        while (name[i] && i < buf_len - 1) {
            buf[i] = name[i];
            i++;
        }
    }
    buf[i] = '\0';
    return (int32_t)pid;
}

static int
register_wasm_ipc_natives(void)
{
    static const wamr_native_symbol_t symbols[] = {
        { "ipc_create_endpoint", native_ipc_create_endpoint, "()i", 0 },
        { "ipc_create_notification", native_ipc_create_notification, "()i", 0 },
        { "ipc_send", native_ipc_send, "(iiiiiiii)i", 0 },
        { "ipc_recv", native_ipc_recv, "(i)i", 0 },
        { "ipc_wait", native_ipc_wait, "(i)i", 0 },
        { "ipc_notify", native_ipc_notify, "(i)i", 0 },
        { "ipc_last_field", native_ipc_last_field, "(i)i", 0 },
        { "console_write", native_console_write, "(ii)i", 0 },
        { "console_read", native_console_read, "(*~)i", 0 },
        { "proc_count", native_proc_count, "()i", 0 },
        { "proc_info", native_proc_info, "(i*~)i", 0 },
        { "block_buffer_phys", native_block_buffer_phys, "()i", 0 },
        { "block_buffer_copy", native_block_buffer_copy, "(iiii)i", 0 },
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

static process_run_result_t
block_ata_entry(process_t *process, void *arg)
{
    (void)process;
    (void)arg;

    int rc = block_ata_service_once();
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
init_entry(process_t *process, void *arg)
{
    init_state_t *state = (init_state_t *)arg;
    uint32_t pm_pid = 0;

    if (!process || !state || !state->boot_info) {
        return PROCESS_RUN_IDLE;
    }

    if (!state->started) {
        process_manager_init(state->boot_info);
        if (process_spawn("process-manager", process_manager_entry, 0, &pm_pid) != 0) {
            serial_write("[init] process manager spawn failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }

        serial_write("[init] process manager pid=");
        serial_write_hex64(pm_pid);
        state->started = 1;
    }

    process_block_on_ipc(process);
    return PROCESS_RUN_BLOCKED;
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
    uint32_t pf_test_pid = 0;
    uint32_t init_pid = 0;
    init_state_t init_state;
    uint32_t ata_pid = 0;
    process_t *ata_proc = 0;

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
    g_chardev_service_endpoint = chardev_endpoint;

    if (process_spawn("disk-ata", block_ata_entry, 0, &ata_pid) != 0) {
        serial_write("[kernel] ata process spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    ata_proc = process_get(ata_pid);
    if (!ata_proc || block_ata_init(ata_proc->context_id) != 0) {
        serial_write("[kernel] ata init failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
    if (block_ata_endpoint(&g_block_service_endpoint) != 0) {
        serial_write("[kernel] ata endpoint lookup failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    wasmos_app_set_policy_hooks(wasmos_endpoint_resolve, wasmos_capability_grant);

    init_state.boot_info = boot_info;
    init_state.started = 0;
    if (process_spawn("init", init_entry, &init_state, &init_pid) != 0) {
        serial_write("[kernel] init spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    serial_write("[kernel] init pid=");
    serial_write_hex64(init_pid);

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

    serial_write("[kernel] interrupts on\n");
    cpu_enable_interrupts();

    serial_write("[kernel] scheduler loop\n");
    run_kernel_loop();
}
