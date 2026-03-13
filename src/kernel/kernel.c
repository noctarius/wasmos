#include "boot.h"
#include "cpu.h"
#include "ipc.h"
#include "memory.h"
#include "memory_service.h"
#include "paging.h"
#include "process.h"
#include "process_manager.h"
#include "serial.h"
#include "timer.h"
#include "wasmos_app.h"
#include "wasm_chardev.h"
#include "wasm3_probe.h"
#include "wasm3_link.h"
#include "physmem.h"
#include "io.h"

#include <stdint.h>
#include "wasm3.h"

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
static const boot_info_t *g_boot_info;

typedef struct {
    uint64_t addr;
    uint8_t stage;
} pf_test_state_t;
static pf_test_state_t g_pf_test_state;

typedef struct {
    uint32_t endpoint;
    uint32_t sender_endpoint;
    uint32_t sender_ticks;
    uint8_t done;
} ipc_test_state_t;
static ipc_test_state_t g_ipc_test_state;

typedef struct {
    uint8_t observer_runs;
    uint8_t done;
    uint8_t stop_busy;
} preempt_test_state_t;
static preempt_test_state_t g_preempt_test_state;
static const uint8_t g_preempt_test_enabled = 0;
static const uint8_t g_skip_wamr_boot = 0;

typedef struct {
    const boot_info_t *boot_info;
    uint8_t started;
    uint8_t phase;
    uint8_t pending_kind;
    uint32_t reply_endpoint;
    uint32_t request_id;
    uint32_t native_min_index;
    uint32_t native_smoke_index;
    uint32_t smoke_index;
    uint32_t sysinit_index;
    uint8_t wasm3_probe_done;
} init_state_t;

static int
bytes_eq(const uint8_t *a, uint32_t a_len, const char *b);

static int
boot_module_name_at(uint32_t index, char *out, uint32_t out_len, uint32_t *out_name_len);

static uint32_t
boot_module_index_by_app_name(const boot_info_t *info, const char *name)
{
    if (!info || !name || !(info->flags & BOOT_INFO_FLAG_MODULES_PRESENT)) {
        return 0xFFFFFFFFu;
    }
    if (!info->modules || info->module_entry_size < sizeof(boot_module_t)) {
        return 0xFFFFFFFFu;
    }
    const uint8_t *mods = (const uint8_t *)info->modules;
    for (uint32_t i = 0; i < info->module_count; ++i) {
        const boot_module_t *mod = (const boot_module_t *)(mods + i * info->module_entry_size);
        if (!mod || mod->type != BOOT_MODULE_TYPE_WASMOS_APP || mod->base == 0 ||
            mod->size == 0 || mod->size > 0xFFFFFFFFULL) {
            continue;
        }
        wasmos_app_desc_t desc;
        if (wasmos_app_parse((const uint8_t *)(uintptr_t)mod->base, (uint32_t)mod->size, &desc) != 0) {
            continue;
        }
        if (bytes_eq(desc.name, desc.name_len, name)) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

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

static void
serial_write_hex64_unlocked(uint64_t value)
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
boot_module_name_at(uint32_t index, char *out, uint32_t out_len, uint32_t *out_name_len)
{
    if (!g_boot_info || !out || out_len == 0 ||
        !(g_boot_info->flags & BOOT_INFO_FLAG_MODULES_PRESENT) ||
        !g_boot_info->modules ||
        g_boot_info->module_entry_size < sizeof(boot_module_t)) {
        return -1;
    }
    if (index >= g_boot_info->module_count) {
        return -1;
    }
    const uint8_t *mods = (const uint8_t *)g_boot_info->modules;
    const boot_module_t *mod = (const boot_module_t *)(mods + index * g_boot_info->module_entry_size);
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
    if (bytes_eq(name, name_len, "block")) {
        uint32_t block_ep = process_manager_block_endpoint();
        if (block_ep != IPC_ENDPOINT_NONE) {
            *out_endpoint = block_ep;
            return 0;
        }
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

    preempt_safepoint();
    for (;;) {
        rc = ipc_recv_for(context_id, (uint32_t)endpoint, &slot->message);
        if (rc == IPC_EMPTY) {
            process_block_on_ipc(process);
            process_yield(PROCESS_RUN_BLOCKED);
            preempt_safepoint();
            continue;
        }
        if (rc != IPC_OK) {
            m3ApiReturn(-1);
        }
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
    if (!g_boot_info || !g_boot_info->rsdp || g_boot_info->rsdp_length == 0) {
        m3ApiReturn(-1);
    }
    uint32_t len = g_boot_info->rsdp_length;
    if (len > (uint32_t)max_len) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(out_ptr, len);
    m3ApiCheckMem(out_len_ptr, sizeof(uint32_t));

    const uint8_t *src = (const uint8_t *)(uintptr_t)g_boot_info->rsdp;
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
    serial_write_hex64_unlocked((uint64_t)(uint32_t)tag);
    serial_write_unlocked("[wasm] debug_mark pid=");
    serial_write_hex64_unlocked((uint64_t)process_current_pid());
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
    if (wasm3_link_raw(module, "env", "strlen", "i(*)", wasmos_strlen) != 0) {
        serial_write("[kernel] wasm3 env link errors\n");
        return -1;
    }
    return 0;
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
        serial_write("[test] ipc endpoint missing\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }

    int rc = ipc_recv_for(process->context_id, state->endpoint, &msg);
    if (rc == IPC_EMPTY) {
        process_block_on_ipc(process);
        return PROCESS_RUN_BLOCKED;
    }
    if (rc != IPC_OK) {
        serial_write("[test] ipc recv failed\n");
        process_set_exit_status(process, -1);
        return PROCESS_RUN_EXITED;
    }

    serial_write("[test] ipc wake ok\n");
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
        serial_write("[test] ipc sender endpoint missing\n");
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
        serial_write("[test] ipc send failed\n");
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
idle_entry(process_t *process, void *arg)
{
    (void)process;
    (void)arg;
    for (;;) {
        __asm__ volatile("hlt");
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
        serial_write("[test] preempt ok\n");
        state->done = 1;
        state->stop_busy = 1;
        process_set_exit_status(process, 0);
        return PROCESS_RUN_EXITED;
    }
    return PROCESS_RUN_YIELDED;
}

static process_run_result_t
init_entry(process_t *process, void *arg)
{
    init_state_t *state = (init_state_t *)arg;
    uint32_t pm_pid = 0;
    ipc_message_t msg;

    if (!process || !state || !state->boot_info) {
        return PROCESS_RUN_IDLE;
    }

    if (!state->started) {
        state->native_min_index = boot_module_index_by_app_name(state->boot_info, "native-call-min");
        state->native_smoke_index = boot_module_index_by_app_name(state->boot_info, "native-call-smoke");
        state->smoke_index = boot_module_index_by_app_name(state->boot_info, "init-smoke");
        state->sysinit_index = boot_module_index_by_app_name(state->boot_info, "sysinit");
        state->wasm3_probe_done = 0;
        state->reply_endpoint = IPC_ENDPOINT_NONE;
        state->request_id = 1;
        state->pending_kind = 0;
        state->phase = 0;
        if (g_skip_wamr_boot) {
            serial_write("[init] wamr boot bypass enabled\n");
            state->started = 1;
            return PROCESS_RUN_YIELDED;
        }

        process_manager_init(state->boot_info);
        if (process_spawn_as(process->pid, "process-manager", process_manager_entry, 0, &pm_pid) != 0) {
            serial_write("[init] process manager spawn failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }

        serial_write("[init] process manager pid=");
        serial_write_hex64(pm_pid);
        state->started = 1;
        if (state->sysinit_index == 0xFFFFFFFFu) {
            serial_write("[init] sysinit module not found\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
    }

    if (g_skip_wamr_boot) {
        if (!state->wasm3_probe_done && state->native_min_index != 0xFFFFFFFFu) {
            serial_write("[init] wasm3 probe native-call-min\n");
            int wasm3_rc = wasm3_probe_run(state->boot_info, state->native_min_index);
            serial_write("[init] wasm3 probe rc=");
            serial_write_hex64((uint64_t)(uint32_t)wasm3_rc);
            state->wasm3_probe_done = 1;
        }
        process_block_on_ipc(process);
        return PROCESS_RUN_BLOCKED;
    }

    if (state->phase == 0) {
        if (!state->wasm3_probe_done && state->native_min_index != 0xFFFFFFFFu) {
            serial_write("[init] wasm3 probe native-call-min\n");
            int wasm3_rc = wasm3_probe_run(state->boot_info, state->native_min_index);
            serial_write("[init] wasm3 probe rc=");
            serial_write_hex64((uint64_t)(uint32_t)wasm3_rc);
            state->wasm3_probe_done = 1;
        }
        uint32_t proc_ep = process_manager_endpoint();
        if (proc_ep == IPC_ENDPOINT_NONE) {
            return PROCESS_RUN_YIELDED;
        }
        if (ipc_endpoint_create(process->context_id, &state->reply_endpoint) != IPC_OK) {
            serial_write("[init] reply endpoint create failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        msg.type = PROC_IPC_SPAWN;
        msg.source = state->reply_endpoint;
        msg.destination = proc_ep;
        msg.request_id = state->request_id;
        if (state->native_min_index != 0xFFFFFFFFu) {
            serial_write("[init] spawn native-call-min\n");
            msg.arg0 = state->native_min_index;
            state->pending_kind = 1;
            state->phase = 1;
        } else if (state->native_smoke_index != 0xFFFFFFFFu) {
            serial_write("[init] spawn native-call-smoke\n");
            msg.arg0 = state->native_smoke_index;
            state->pending_kind = 2;
            state->phase = 1;
        } else if (state->smoke_index != 0xFFFFFFFFu) {
            serial_write("[init] spawn init-smoke\n");
            msg.arg0 = state->smoke_index;
            state->pending_kind = 3;
            state->phase = 1;
        } else {
            serial_write("[init] spawn sysinit\n");
            msg.arg0 = state->sysinit_index;
            state->pending_kind = 4;
            state->phase = 3;
        }
        msg.arg1 = 0;
        msg.arg2 = 0;
        msg.arg3 = 0;
        serial_write("[init] proc ep=");
        serial_write_hex64(proc_ep);
        serial_write("[init] reply ep=");
        serial_write_hex64(state->reply_endpoint);
        serial_write("[init] ctx=");
        serial_write_hex64(process->context_id);
        uint32_t proc_owner = 0;
        uint32_t reply_owner = 0;
        if (ipc_endpoint_owner(proc_ep, &proc_owner) == IPC_OK) {
            serial_write("[init] proc owner=");
            serial_write_hex64(proc_owner);
        }
        if (ipc_endpoint_owner(state->reply_endpoint, &reply_owner) == IPC_OK) {
            serial_write("[init] reply owner=");
            serial_write_hex64(reply_owner);
        }
        serial_write_unlocked("[init] preempt depth=");
        serial_write_hex64_unlocked(preempt_disable_depth());
        serial_write_unlocked("[init] send enter\n");
        int send_rc = ipc_send_from(process->context_id, proc_ep, &msg);
        serial_write_unlocked("[init] send rc=");
        serial_write_hex64_unlocked((uint64_t)(uint32_t)send_rc);
        if (send_rc != IPC_OK) {
            serial_write("[init] spawn request failed rc=");
            serial_write_hex64((uint64_t)(uint32_t)send_rc);
            serial_write("[init] spawn request failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        uint32_t queued = 0;
        int count_rc = ipc_endpoint_count(proc_ep, &queued);
        if (count_rc == IPC_OK) {
            serial_write_unlocked("[init] proc queue=");
            serial_write_hex64_unlocked(queued);
        } else {
            serial_write_unlocked("[init] proc queue rc=");
            serial_write_hex64_unlocked((uint64_t)(uint32_t)count_rc);
        }
        return PROCESS_RUN_YIELDED;
    }

    if (state->phase == 1) {
        int recv_rc = ipc_recv_for(process->context_id, state->reply_endpoint, &msg);
        if (recv_rc == IPC_EMPTY) {
            process_block_on_ipc(process);
            return PROCESS_RUN_BLOCKED;
        }
        if (recv_rc != IPC_OK) {
            return PROCESS_RUN_YIELDED;
        }
        if (msg.request_id != state->request_id || msg.type == PROC_IPC_ERROR) {
            if (state->pending_kind == 1) {
                serial_write("[init] native-call-min spawn failed\n");
            } else if (state->pending_kind == 2) {
                serial_write("[init] native-call-smoke spawn failed\n");
            } else if (state->pending_kind == 3) {
                serial_write("[init] init-smoke spawn failed\n");
            } else {
                serial_write("[init] sysinit spawn failed\n");
            }
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (state->pending_kind == 1) {
            serial_write("[init] native-call-min spawn ok\n");
            state->pending_kind = 0;
            if (state->native_smoke_index != 0xFFFFFFFFu) {
                state->request_id++;
                msg.type = PROC_IPC_SPAWN;
                msg.source = state->reply_endpoint;
                msg.destination = process_manager_endpoint();
                msg.request_id = state->request_id;
                msg.arg0 = state->native_smoke_index;
                msg.arg1 = 0;
                msg.arg2 = 0;
                msg.arg3 = 0;
                serial_write("[init] spawn native-call-smoke\n");
                int send_rc = ipc_send_from(process->context_id, msg.destination, &msg);
                if (send_rc != IPC_OK) {
                    serial_write("[init] native-call-smoke spawn request failed rc=");
                    serial_write_hex64((uint64_t)(uint32_t)send_rc);
                    serial_write("[init] native-call-smoke spawn request failed\n");
                    process_set_exit_status(process, -1);
                    return PROCESS_RUN_EXITED;
                }
                state->pending_kind = 2;
                state->phase = 2;
                return PROCESS_RUN_YIELDED;
            }
        } else if (state->pending_kind == 2) {
            serial_write("[init] native-call-smoke spawn ok\n");
            state->pending_kind = 0;
            if (state->smoke_index != 0xFFFFFFFFu) {
                state->request_id++;
                msg.type = PROC_IPC_SPAWN;
                msg.source = state->reply_endpoint;
                msg.destination = process_manager_endpoint();
                msg.request_id = state->request_id;
                msg.arg0 = state->smoke_index;
                msg.arg1 = 0;
                msg.arg2 = 0;
                msg.arg3 = 0;
                serial_write("[init] spawn init-smoke\n");
                int send_rc = ipc_send_from(process->context_id, msg.destination, &msg);
                if (send_rc != IPC_OK) {
                    serial_write("[init] init-smoke spawn request failed rc=");
                    serial_write_hex64((uint64_t)(uint32_t)send_rc);
                    serial_write("[init] init-smoke spawn request failed\n");
                    process_set_exit_status(process, -1);
                    return PROCESS_RUN_EXITED;
                }
                state->pending_kind = 3;
                state->phase = 2;
                return PROCESS_RUN_YIELDED;
            }
        } else if (state->pending_kind == 3) {
            serial_write("[init] init-smoke spawn ok\n");
            state->pending_kind = 0;
        } else {
            serial_write("[init] sysinit spawn ok\n");
            state->pending_kind = 0;
            state->phase = 4;
            process_block_on_ipc(process);
            return PROCESS_RUN_BLOCKED;
        }

        state->request_id++;
        msg.type = PROC_IPC_SPAWN;
        msg.source = state->reply_endpoint;
        msg.destination = process_manager_endpoint();
        msg.request_id = state->request_id;
        msg.arg0 = state->sysinit_index;
        msg.arg1 = 0;
        msg.arg2 = 0;
        msg.arg3 = 0;
        serial_write("[init] spawn sysinit\n");
        int send_rc = ipc_send_from(process->context_id, msg.destination, &msg);
        if (send_rc != IPC_OK) {
            serial_write("[init] sysinit spawn request failed rc=");
            serial_write_hex64((uint64_t)(uint32_t)send_rc);
            serial_write("[init] sysinit spawn request failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        state->pending_kind = 4;
        state->phase = 3;
    }

    if (state->phase == 2) {
        int recv_rc = ipc_recv_for(process->context_id, state->reply_endpoint, &msg);
        if (recv_rc == IPC_EMPTY) {
            process_block_on_ipc(process);
            return PROCESS_RUN_BLOCKED;
        }
        if (recv_rc != IPC_OK) {
            return PROCESS_RUN_YIELDED;
        }
        if (msg.request_id != state->request_id || msg.type == PROC_IPC_ERROR) {
            serial_write("[init] init-smoke spawn failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        serial_write("[init] init-smoke spawn ok\n");
        state->request_id++;
        msg.type = PROC_IPC_SPAWN;
        msg.source = state->reply_endpoint;
        msg.destination = process_manager_endpoint();
        msg.request_id = state->request_id;
        msg.arg0 = state->sysinit_index;
        msg.arg1 = 0;
        msg.arg2 = 0;
        msg.arg3 = 0;
        serial_write("[init] spawn sysinit\n");
        int send_rc = ipc_send_from(process->context_id, msg.destination, &msg);
        if (send_rc != IPC_OK) {
            serial_write("[init] sysinit spawn request failed rc=");
            serial_write_hex64((uint64_t)(uint32_t)send_rc);
            serial_write("[init] sysinit spawn request failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        state->pending_kind = 4;
        state->phase = 3;
    }

    if (state->phase == 3) {
        int recv_rc = ipc_recv_for(process->context_id, state->reply_endpoint, &msg);
        if (recv_rc == IPC_EMPTY) {
            process_block_on_ipc(process);
            return PROCESS_RUN_BLOCKED;
        }
        if (recv_rc != IPC_OK) {
            return PROCESS_RUN_YIELDED;
        }
        if (msg.request_id != state->request_id || msg.type == PROC_IPC_ERROR) {
            serial_write("[init] sysinit spawn failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        serial_write("[init] sysinit spawn ok\n");
        state->pending_kind = 0;
        state->phase = 4;
    }

    process_block_on_ipc(process);
    return PROCESS_RUN_BLOCKED;
}

static void
run_kernel_loop(void)
{
    for (;;) {
        __asm__ volatile("cli");
        if (process_schedule_once() != 0) {
            __asm__ volatile("pause");
        }
        if (process_should_resched()) {
            process_clear_resched();
        }
        timer_poll();
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
    uint32_t ipc_wait_pid = 0;
    uint32_t ipc_send_pid = 0;
    process_t *ipc_wait_proc = 0;
    process_t *ipc_send_proc = 0;
    uint32_t preempt_busy_pid = 0;
    uint32_t preempt_observer_pid = 0;
    uint32_t idle_pid = 0;
    uint32_t init_pid = 0;
    init_state_t init_state;

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
    g_boot_info = boot_info;
    cpu_init();

    mm_init(boot_info);
    ipc_init();
    process_init();
    wasm_ipc_slots_init();

    serial_write("[kernel] wasm3 init on-demand\n");

    if (process_spawn_idle("idle", idle_entry, 0, &idle_pid) != 0) {
        serial_write("[kernel] idle spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

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

    serial_write("[kernel] chardev pid=");
    serial_write_hex64(chardev_pid);

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

    wasmos_app_set_policy_hooks(wasmos_endpoint_resolve, wasmos_capability_grant);

    g_pf_test_state.addr = 0;
    g_pf_test_state.stage = 0;
    if (process_spawn_as(init_pid, "pagefault-test", page_fault_test_entry, &g_pf_test_state, &pf_test_pid) != 0) {
        serial_write("[kernel] page fault test spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    serial_write("[kernel] page fault test pid=");
    serial_write_hex64(pf_test_pid);

    g_ipc_test_state.endpoint = IPC_ENDPOINT_NONE;
    g_ipc_test_state.sender_endpoint = IPC_ENDPOINT_NONE;
    g_ipc_test_state.sender_ticks = 0;
    g_ipc_test_state.done = 0;
    if (process_spawn_as(init_pid, "ipc-wait-test", ipc_wait_test_entry, &g_ipc_test_state, &ipc_wait_pid) != 0 ||
        process_spawn_as(init_pid, "ipc-send-test", ipc_send_test_entry, &g_ipc_test_state, &ipc_send_pid) != 0) {
        serial_write("[kernel] ipc test spawn failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    ipc_wait_proc = process_get(ipc_wait_pid);
    ipc_send_proc = process_get(ipc_send_pid);
    if (!ipc_wait_proc || !ipc_send_proc) {
        serial_write("[kernel] ipc test lookup failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    if (ipc_endpoint_create(ipc_wait_proc->context_id, &g_ipc_test_state.endpoint) != IPC_OK ||
        ipc_endpoint_create(ipc_send_proc->context_id, &g_ipc_test_state.sender_endpoint) != IPC_OK) {
        serial_write("[kernel] ipc test endpoint create failed\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    if (g_preempt_test_enabled) {
    g_preempt_test_state.observer_runs = 0;
    g_preempt_test_state.done = 0;
    g_preempt_test_state.stop_busy = 0;
    if (process_spawn_as(init_pid, "preempt-busy", preempt_busy_entry, &g_preempt_test_state, &preempt_busy_pid) != 0 ||
        process_spawn_as(init_pid, "preempt-observer", preempt_observer_entry, &g_preempt_test_state,
                         &preempt_observer_pid) != 0) {
            serial_write("[kernel] preempt test spawn failed\n");
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
    }

    timer_init(250);
    serial_write("[kernel] interrupts on\n");
    cpu_enable_interrupts();

    serial_write("[kernel] scheduler loop\n");
    run_kernel_loop();
}
