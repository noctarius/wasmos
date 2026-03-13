#include "boot.h"
#include "ipc.h"
#include "io.h"
#include "physmem.h"
#include "process.h"
#include "process_manager.h"
#include "serial.h"
#include "timer.h"
#include "wasm3_link.h"
#include "wasmos_app.h"

#include <stdint.h>

typedef struct {
    uint32_t pid;
    uint8_t valid;
    ipc_message_t message;
} wasm_ipc_last_slot_t;

typedef struct {
    uint32_t pid;
    uint64_t buffer_phys;
} wasm_block_slot_t;

static wasm_ipc_last_slot_t g_wasm_last_slots[PROCESS_MAX_COUNT];
static wasm_block_slot_t g_wasm_block_slots[PROCESS_MAX_COUNT];
static const boot_info_t *g_wasm_boot_info;

static void
serial_write_hex64_unlocked_local(uint64_t value)
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
    serial_write_unlocked(buf);
}

static int
boot_module_name_at(uint32_t index, char *out, uint32_t out_len, uint32_t *out_name_len)
{
    if (!g_wasm_boot_info || !out || out_len == 0 ||
        !(g_wasm_boot_info->flags & BOOT_INFO_FLAG_MODULES_PRESENT) ||
        !g_wasm_boot_info->modules ||
        g_wasm_boot_info->module_entry_size < sizeof(boot_module_t)) {
        return -1;
    }
    if (index >= g_wasm_boot_info->module_count) {
        return -1;
    }

    const uint8_t *mods = (const uint8_t *)g_wasm_boot_info->modules;
    const boot_module_t *mod =
        (const boot_module_t *)(mods + index * g_wasm_boot_info->module_entry_size);
    if (!mod || mod->type != BOOT_MODULE_TYPE_WASMOS_APP || mod->base == 0 ||
        mod->size == 0 || mod->size > 0xFFFFFFFFULL) {
        return -1;
    }

    wasmos_app_desc_t desc;
    if (wasmos_app_parse((const uint8_t *)(uintptr_t)mod->base, (uint32_t)mod->size, &desc) != 0) {
        return -1;
    }

    uint32_t copy_len = desc.name_len;
    if (copy_len >= out_len) {
        copy_len = out_len - 1;
    }
    for (uint32_t i = 0; i < copy_len; ++i) {
        out[i] = (char)desc.name[i];
    }
    out[copy_len] = '\0';
    if (out_name_len) {
        *out_name_len = desc.name_len;
    }
    return 0;
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

m3ApiRawFunction(wasmos_ipc_create_endpoint)
{
    m3ApiReturnType(int32_t)
    uint32_t context_id = 0;
    uint32_t endpoint = IPC_ENDPOINT_NONE;

    preempt_safepoint();
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }
    if (ipc_endpoint_create(context_id, &endpoint) != IPC_OK) {
        m3ApiReturn(-1);
    }
    preempt_safepoint();
    m3ApiReturn((int32_t)endpoint);
}

m3ApiRawFunction(wasmos_ipc_create_notification)
{
    m3ApiReturnType(int32_t)
    uint32_t context_id = 0;
    uint32_t endpoint = IPC_ENDPOINT_NONE;

    preempt_safepoint();
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }
    if (ipc_notification_create(context_id, &endpoint) != IPC_OK) {
        m3ApiReturn(-1);
    }
    preempt_safepoint();
    m3ApiReturn((int32_t)endpoint);
}

m3ApiRawFunction(wasmos_ipc_send)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, destination_endpoint)
    m3ApiGetArg(int32_t, source_endpoint)
    m3ApiGetArg(int32_t, type)
    m3ApiGetArg(int32_t, request_id)
    m3ApiGetArg(int32_t, arg0)
    m3ApiGetArg(int32_t, arg1)
    m3ApiGetArg(int32_t, arg2)
    m3ApiGetArg(int32_t, arg3)
    uint32_t context_id = 0;
    ipc_message_t req;

    preempt_safepoint();
    if (destination_endpoint < 0 || source_endpoint < 0) {
        m3ApiReturn(-1);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }

    req.type = (uint32_t)type;
    req.source = (uint32_t)source_endpoint;
    req.destination = (uint32_t)destination_endpoint;
    req.request_id = (uint32_t)request_id;
    req.arg0 = (uint32_t)arg0;
    req.arg1 = (uint32_t)arg1;
    req.arg2 = (uint32_t)arg2;
    req.arg3 = (uint32_t)arg3;

    int rc = ipc_send_from(context_id, (uint32_t)destination_endpoint, &req);
    preempt_safepoint();
    m3ApiReturn(rc);
}

m3ApiRawFunction(wasmos_ipc_recv)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, endpoint)
    uint32_t context_id = 0;
    uint32_t pid = process_current_pid();
    wasm_ipc_last_slot_t *slot;
    int rc;
    process_t *process;

    if (endpoint < 0 || current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }

    slot = wasm_ipc_slot_for_pid(pid);
    if (!slot) {
        m3ApiReturn(-1);
    }

    process = process_get(pid);
    if (!process) {
        m3ApiReturn(-1);
    }
    process->in_hostcall = 1;

    preempt_safepoint();
    for (;;) {
        process->block_reason = PROCESS_BLOCK_IPC;
        rc = ipc_recv_for(context_id, (uint32_t)endpoint, &slot->message);
        if (rc == IPC_EMPTY) {
            process_block_on_ipc(process);
            rc = ipc_recv_for(context_id, (uint32_t)endpoint, &slot->message);
            if (rc == IPC_OK) {
                process->state = PROCESS_STATE_RUNNING;
                process->block_reason = PROCESS_BLOCK_NONE;
                process->in_hostcall = 0;
                slot->valid = 1;
                preempt_safepoint();
                m3ApiReturn(1);
            }
            if (rc != IPC_EMPTY) {
                process->state = PROCESS_STATE_RUNNING;
                process->block_reason = PROCESS_BLOCK_NONE;
                process->in_hostcall = 0;
                m3ApiReturn(-1);
            }
            process_yield(PROCESS_RUN_BLOCKED);
            preempt_safepoint();
            continue;
        }
        if (rc != IPC_OK) {
            process->block_reason = PROCESS_BLOCK_NONE;
            process->in_hostcall = 0;
            m3ApiReturn(-1);
        }
        process->block_reason = PROCESS_BLOCK_NONE;
        process->in_hostcall = 0;
        slot->valid = 1;
        preempt_safepoint();
        m3ApiReturn(1);
    }
}

m3ApiRawFunction(wasmos_ipc_wait)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, endpoint)
    uint32_t context_id = 0;
    int rc;

    preempt_safepoint();
    if (endpoint < 0 || current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }

    rc = ipc_wait_for(context_id, (uint32_t)endpoint);
    if (rc == IPC_EMPTY) {
        preempt_safepoint();
        m3ApiReturn(0);
    }
    if (rc != IPC_OK) {
        m3ApiReturn(-1);
    }
    preempt_safepoint();
    m3ApiReturn(1);
}

m3ApiRawFunction(wasmos_ipc_notify)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, endpoint)
    uint32_t context_id = 0;

    preempt_safepoint();
    if (endpoint < 0 || current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }
    int rc = ipc_notify_from(context_id, (uint32_t)endpoint) == IPC_OK ? 0 : -1;
    preempt_safepoint();
    m3ApiReturn(rc);
}

m3ApiRawFunction(wasmos_ipc_last_field)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, field)
    uint32_t pid = process_current_pid();
    wasm_ipc_last_slot_t *slot = wasm_ipc_slot_for_pid(pid);

    if (!slot || !slot->valid) {
        m3ApiReturn(-1);
    }

    switch ((uint32_t)field) {
        case WASMOS_IPC_FIELD_TYPE:
            m3ApiReturn((int32_t)slot->message.type);
        case WASMOS_IPC_FIELD_REQUEST_ID:
            m3ApiReturn((int32_t)slot->message.request_id);
        case WASMOS_IPC_FIELD_ARG0:
            m3ApiReturn((int32_t)slot->message.arg0);
        case WASMOS_IPC_FIELD_ARG1:
            m3ApiReturn((int32_t)slot->message.arg1);
        case WASMOS_IPC_FIELD_SOURCE:
            m3ApiReturn((int32_t)slot->message.source);
        case WASMOS_IPC_FIELD_DESTINATION:
            m3ApiReturn((int32_t)slot->message.destination);
        case WASMOS_IPC_FIELD_ARG2:
            m3ApiReturn((int32_t)slot->message.arg2);
        case WASMOS_IPC_FIELD_ARG3:
            m3ApiReturn((int32_t)slot->message.arg3);
        default:
            m3ApiReturn(-1);
    }
}

#define WASM_BLOCK_BUFFER_PAGES 2u

m3ApiRawFunction(wasmos_block_buffer_phys)
{
    m3ApiReturnType(int32_t)
    uint32_t pid = process_current_pid();
    wasm_block_slot_t *slot = wasm_block_slot_for_pid(pid);

    if (!slot) {
        m3ApiReturn(-1);
    }
    if (slot->buffer_phys == 0) {
        uint64_t phys = pfa_alloc_pages_below(WASM_BLOCK_BUFFER_PAGES, 0x100000000ULL);
        if (!phys) {
            m3ApiReturn(-1);
        }
        slot->buffer_phys = phys;
    }

    if (slot->buffer_phys > 0xFFFFFFFFULL) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)slot->buffer_phys);
}

m3ApiRawFunction(wasmos_block_buffer_copy)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, phys)
    m3ApiGetArgMem(uint8_t *, ptr)
    m3ApiGetArg(int32_t, len)
    m3ApiGetArg(int32_t, offset)

    if (len <= 0 || offset < 0 || phys <= 0) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(ptr, (uint32_t)len);

    const uint8_t *src = (const uint8_t *)(uintptr_t)((uint32_t)phys + (uint32_t)offset);
    for (int32_t i = 0; i < len; ++i) {
        ptr[i] = src[i];
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_block_buffer_write)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, phys)
    m3ApiGetArgMem(const uint8_t *, ptr)
    m3ApiGetArg(int32_t, len)
    m3ApiGetArg(int32_t, offset)

    if (len <= 0 || offset < 0 || phys <= 0) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(ptr, (uint32_t)len);

    uint8_t *dst = (uint8_t *)(uintptr_t)((uint32_t)phys + (uint32_t)offset);
    for (int32_t i = 0; i < len; ++i) {
        dst[i] = ptr[i];
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_fs_buffer_size)
{
    m3ApiReturnType(int32_t)
    m3ApiReturn((int32_t)process_manager_fs_buffer_size());
}

m3ApiRawFunction(wasmos_fs_buffer_write)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(const uint8_t *, ptr)
    m3ApiGetArg(int32_t, len)
    m3ApiGetArg(int32_t, offset)

    if (len <= 0 || offset < 0) {
        m3ApiReturn(-1);
    }
    uint32_t max_len = process_manager_fs_buffer_size();
    if ((uint32_t)offset + (uint32_t)len > max_len) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(ptr, (uint32_t)len);

    uint8_t *dst = (uint8_t *)process_manager_fs_buffer();
    for (int32_t i = 0; i < len; ++i) {
        dst[offset + i] = ptr[i];
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_io_in8)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, port)
    if (port < 0 || port > 0xFFFF) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)inb((uint16_t)port));
}

m3ApiRawFunction(wasmos_io_in16)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, port)
    if (port < 0 || port > 0xFFFF) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)inw((uint16_t)port));
}

m3ApiRawFunction(wasmos_io_out8)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, port)
    m3ApiGetArg(int32_t, value)
    if (port < 0 || port > 0xFFFF || value < 0 || value > 0xFF) {
        m3ApiReturn(-1);
    }
    outb((uint16_t)port, (uint8_t)value);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_io_out16)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, port)
    m3ApiGetArg(int32_t, value)
    if (port < 0 || port > 0xFFFF || value < 0 || value > 0xFFFF) {
        m3ApiReturn(-1);
    }
    outw((uint16_t)port, (uint16_t)value);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_io_wait)
{
    m3ApiReturnType(int32_t)
    io_wait();
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_system_halt)
{
    m3ApiReturnType(int32_t)
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    for (;;) {
        __asm__ volatile("hlt");
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_system_reboot)
{
    m3ApiReturnType(int32_t)
    outb(0x64, 0xFE);
    outb(0xCF9, 0x06);
    outb(0xCF9, 0x0E);
    for (;;) {
        __asm__ volatile("hlt");
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_acpi_rsdp_info)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(uint8_t *, out_ptr)
    m3ApiGetArgMem(uint32_t *, out_len_ptr)
    m3ApiGetArg(int32_t, max_len)

    if (max_len <= 0) {
        m3ApiReturn(-1);
    }
    if (!g_wasm_boot_info || !g_wasm_boot_info->rsdp || g_wasm_boot_info->rsdp_length == 0) {
        m3ApiReturn(-1);
    }
    uint32_t len = g_wasm_boot_info->rsdp_length;
    if (len > (uint32_t)max_len) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(out_ptr, len);
    m3ApiCheckMem(out_len_ptr, sizeof(uint32_t));

    const uint8_t *src = (const uint8_t *)(uintptr_t)g_wasm_boot_info->rsdp;
    for (uint32_t i = 0; i < len; ++i) {
        out_ptr[i] = src[i];
    }
    *out_len_ptr = len;
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_boot_module_name)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, index)
    m3ApiGetArgMem(char *, out_ptr)
    m3ApiGetArg(int32_t, out_len)

    if (index < 0 || out_len <= 0) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(out_ptr, (uint32_t)out_len);

    uint32_t name_len = 0;
    if (boot_module_name_at((uint32_t)index, out_ptr, (uint32_t)out_len, &name_len) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)name_len);
}

m3ApiRawFunction(wasmos_console_write)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(const char *, ptr)
    m3ApiGetArg(int32_t, len)

    if (len <= 0) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(ptr, (uint32_t)len);

    char buf[128];
    int32_t remaining = len;
    while (remaining > 0) {
        int32_t chunk = remaining > (int32_t)(sizeof(buf) - 1) ? (int32_t)(sizeof(buf) - 1) : remaining;
        for (int32_t i = 0; i < chunk; ++i) {
            buf[i] = ptr[len - remaining + i];
        }
        buf[chunk] = '\0';
        serial_write(buf);
        remaining -= chunk;
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_debug_mark)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, tag)
    serial_write_unlocked("[wasm] debug_mark tag=");
    serial_write_hex64_unlocked_local((uint64_t)(uint32_t)tag);
    serial_write_unlocked("[wasm] debug_mark pid=");
    serial_write_hex64_unlocked_local((uint64_t)process_current_pid());
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_console_read)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(char *, ptr)
    m3ApiGetArg(int32_t, len)

    if (len <= 0) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(ptr, 1);
    uint8_t ch = 0;
    int rc = serial_read_char(&ch);
    if (rc <= 0) {
        m3ApiReturn(rc);
    }
    ptr[0] = (char)ch;
    m3ApiReturn(1);
}

m3ApiRawFunction(wasmos_proc_count)
{
    m3ApiReturnType(int32_t)
    m3ApiReturn((int32_t)process_count_active());
}

m3ApiRawFunction(wasmos_proc_exit)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, status)
    process_t *proc = process_get(process_current_pid());
    if (!proc) {
        m3ApiReturn(-1);
    }
    process_set_exit_status(proc, status);
    process_yield(PROCESS_RUN_EXITED);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_sched_ticks)
{
    m3ApiReturnType(int32_t)
    m3ApiReturn((int32_t)timer_ticks());
}

m3ApiRawFunction(wasmos_sched_ready_count)
{
    m3ApiReturnType(int32_t)
    m3ApiReturn((int32_t)process_ready_count());
}

m3ApiRawFunction(wasmos_sched_current_pid)
{
    m3ApiReturnType(int32_t)
    m3ApiReturn((int32_t)process_current_pid());
}

m3ApiRawFunction(wasmos_sched_yield)
{
    m3ApiReturnType(int32_t)
    process_yield(PROCESS_RUN_YIELDED);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_proc_info)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, index)
    m3ApiGetArgMem(char *, buf)
    m3ApiGetArg(int32_t, buf_len)

    if (index < 0 || buf_len <= 0) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(buf, (uint32_t)buf_len);

    uint32_t pid = 0;
    const char *name = 0;
    if (process_info_at((uint32_t)index, &pid, &name) != 0) {
        m3ApiReturn(-1);
    }
    int32_t i = 0;
    if (name) {
        while (name[i] && i < buf_len - 1) {
            buf[i] = name[i];
            i++;
        }
    }
    buf[i] = '\0';
    m3ApiReturn((int32_t)pid);
}

m3ApiRawFunction(wasmos_proc_info_ex)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, index)
    m3ApiGetArgMem(char *, buf)
    m3ApiGetArg(int32_t, buf_len)
    m3ApiGetArgMem(uint32_t *, parent_ptr)

    if (index < 0 || buf_len <= 0) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(buf, (uint32_t)buf_len);
    m3ApiCheckMem(parent_ptr, sizeof(uint32_t));

    uint32_t pid = 0;
    uint32_t parent_pid = 0;
    const char *name = 0;
    if (process_info_at_ex((uint32_t)index, &pid, &parent_pid, &name) != 0) {
        m3ApiReturn(-1);
    }
    *parent_ptr = parent_pid;

    int32_t i = 0;
    if (name) {
        while (name[i] && i < buf_len - 1) {
            buf[i] = name[i];
            i++;
        }
    }
    buf[i] = '\0';
    m3ApiReturn((int32_t)pid);
}

m3ApiRawFunction(wasmos_strlen)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(const char *, ptr)

    const uint8_t *start = (const uint8_t *)ptr;
    const uint8_t *end = (const uint8_t *)_mem + m3_GetMemorySize(runtime);
    if ((const uint8_t *)ptr < (const uint8_t *)_mem || start >= end) {
        m3ApiReturn(0);
    }
    int32_t len = 0;
    while (start + len < end && start[len] != '\0') {
        len++;
    }
    m3ApiReturn(len);
}

m3ApiRawFunction(wasmos_env_abort)
{
    m3ApiReturnType(void)
    (void)raw_return;
    m3ApiGetArg(int32_t, msg)
    m3ApiGetArg(int32_t, file)
    m3ApiGetArg(int32_t, line)
    m3ApiGetArg(int32_t, column)
    (void)msg;
    (void)file;
    (void)line;
    (void)column;

    process_t *proc = process_get(process_current_pid());
    if (proc) {
        process_set_exit_status(proc, -1);
        process_yield(PROCESS_RUN_EXITED);
    }
    m3ApiSuccess();
}

static void
wasm3_link_error(const char *name, const char *res)
{
    serial_write("[wasm3] link failed ");
    serial_write(name);
    serial_write(": ");
    serial_write(res ? res : "unknown");
    serial_write("\n");
}

static int
wasm3_link_raw(IM3Module module, const char *mod, const char *name, const char *sig, M3RawCall fn)
{
    M3Result res = m3_LinkRawFunction(module, mod, name, sig, fn);
    if (res && res != m3Err_functionLookupFailed) {
        wasm3_link_error(name, res);
        return -1;
    }
    return 0;
}

void
wasm3_link_init(const boot_info_t *boot_info)
{
    g_wasm_boot_info = boot_info;
    wasm_ipc_slots_init();
}

int
wasm3_link_wasmos(IM3Module module)
{
    if (!module) {
        return -1;
    }
    int rc = 0;
    rc |= wasm3_link_raw(module, "wasmos", "ipc_create_endpoint", "i()", wasmos_ipc_create_endpoint);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_create_notification", "i()", wasmos_ipc_create_notification);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_send", "i(iiiiiiii)", wasmos_ipc_send);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_recv", "i(i)", wasmos_ipc_recv);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_wait", "i(i)", wasmos_ipc_wait);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_notify", "i(i)", wasmos_ipc_notify);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_last_field", "i(i)", wasmos_ipc_last_field);
    rc |= wasm3_link_raw(module, "wasmos", "console_write", "i(*i)", wasmos_console_write);
    rc |= wasm3_link_raw(module, "wasmos", "debug_mark", "i(i)", wasmos_debug_mark);
    rc |= wasm3_link_raw(module, "wasmos", "console_read", "i(*i)", wasmos_console_read);
    rc |= wasm3_link_raw(module, "wasmos", "proc_count", "i()", wasmos_proc_count);
    rc |= wasm3_link_raw(module, "wasmos", "proc_exit", "i(i)", wasmos_proc_exit);
    rc |= wasm3_link_raw(module, "wasmos", "sched_ticks", "i()", wasmos_sched_ticks);
    rc |= wasm3_link_raw(module, "wasmos", "sched_ready_count", "i()", wasmos_sched_ready_count);
    rc |= wasm3_link_raw(module, "wasmos", "sched_current_pid", "i()", wasmos_sched_current_pid);
    rc |= wasm3_link_raw(module, "wasmos", "sched_yield", "i()", wasmos_sched_yield);
    rc |= wasm3_link_raw(module, "wasmos", "proc_info", "i(i*i)", wasmos_proc_info);
    rc |= wasm3_link_raw(module, "wasmos", "proc_info_ex", "i(i*i*)", wasmos_proc_info_ex);
    rc |= wasm3_link_raw(module, "wasmos", "block_buffer_phys", "i()", wasmos_block_buffer_phys);
    rc |= wasm3_link_raw(module, "wasmos", "block_buffer_copy", "i(i*ii)", wasmos_block_buffer_copy);
    rc |= wasm3_link_raw(module, "wasmos", "block_buffer_write", "i(i*ii)", wasmos_block_buffer_write);
    rc |= wasm3_link_raw(module, "wasmos", "fs_buffer_size", "i()", wasmos_fs_buffer_size);
    rc |= wasm3_link_raw(module, "wasmos", "fs_buffer_write", "i(*ii)", wasmos_fs_buffer_write);
    rc |= wasm3_link_raw(module, "wasmos", "system_halt", "i()", wasmos_system_halt);
    rc |= wasm3_link_raw(module, "wasmos", "system_reboot", "i()", wasmos_system_reboot);
    rc |= wasm3_link_raw(module, "wasmos", "acpi_rsdp_info", "i(**i)", wasmos_acpi_rsdp_info);
    rc |= wasm3_link_raw(module, "wasmos", "boot_module_name", "i(i*i)", wasmos_boot_module_name);
    rc |= wasm3_link_raw(module, "wasmos", "io_in8", "i(i)", wasmos_io_in8);
    rc |= wasm3_link_raw(module, "wasmos", "io_in16", "i(i)", wasmos_io_in16);
    rc |= wasm3_link_raw(module, "wasmos", "io_out8", "i(ii)", wasmos_io_out8);
    rc |= wasm3_link_raw(module, "wasmos", "io_out16", "i(ii)", wasmos_io_out16);
    rc |= wasm3_link_raw(module, "wasmos", "io_wait", "i()", wasmos_io_wait);
    if (rc != 0) {
        serial_write("[kernel] wasm3 link errors\n");
        return -1;
    }
    return 0;
}

int
wasm3_link_env(IM3Module module)
{
    if (!module) {
        return -1;
    }
    int rc = 0;
    rc |= wasm3_link_raw(module, "env", "strlen", "i(*)", wasmos_strlen);
    rc |= wasm3_link_raw(module, "env", "abort", "v(iiii)", wasmos_env_abort);
    if (rc != 0) {
        serial_write("[kernel] wasm3 env link errors\n");
        return -1;
    }
    return 0;
}
