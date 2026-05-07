#include "boot.h"
#include "ipc.h"
#include "io.h"
#include "physmem.h"
#include "process.h"
#include "process_manager.h"
#include "memory.h"
#include "paging.h"
#include "serial.h"
#include "timer.h"
#include "wasm3_link.h"
#include "wasmos_app.h"
#include "wasmos_driver_abi.h"
#include "framebuffer.h"
#include "irq.h"
#include "policy.h"

#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t pid;
    uint8_t valid;
    ipc_message_t message;
} wasm_ipc_last_slot_t;

typedef struct {
    uint32_t pid;
    uint64_t buffer_phys;
} wasm_block_slot_t;

typedef struct {
    uint32_t pid;
    uint8_t valid;
    uint32_t peer_context_id;
} wasm_fs_peer_slot_t;

static wasm_ipc_last_slot_t g_wasm_last_slots[PROCESS_MAX_COUNT];
static wasm_block_slot_t g_wasm_block_slots[PROCESS_MAX_COUNT];
static wasm_fs_peer_slot_t g_wasm_fs_peer_slots[PROCESS_MAX_COUNT];
static const boot_info_t *g_wasm_boot_info;

static int
wasm_copy_to_user_bytes(uint32_t context_id,
                        uint64_t user_dst,
                        const void *src,
                        uint32_t len)
{
    if (!src) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (mm_copy_to_user(context_id, user_dst, src, (uint64_t)len) != 0) {
        return -1;
    }
    return 0;
}

static int
wasm_copy_to_user_sync_views(uint32_t context_id,
                             uint64_t user_dst,
                             void *host_dst,
                             const void *src,
                             uint32_t len)
{
    if (!host_dst || !src) {
        return -1;
    }
    if (wasm_copy_to_user_bytes(context_id, user_dst, src, len) != 0) {
        return -1;
    }
    /* Ring3 migration note:
     * wasm3 host pointers can diverge from validated user mappings in some
     * early-output paths; keep both views synchronized until linear-memory
     * ownership is fully unified. */
    memcpy(host_dst, src, (size_t)len);
    return 0;
}

static int
wasm_copy_from_user_sync_views(uint32_t context_id,
                               uint64_t user_src,
                               const void *host_src,
                               void *dst,
                               uint32_t len)
{
    if (!host_src || !dst) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (mm_copy_from_user(context_id, dst, user_src, (uint64_t)len) != 0) {
        return -1;
    }
    if (memcmp(dst, host_src, (size_t)len) != 0) {
        memcpy(dst, host_src, (size_t)len);
        if (mm_copy_to_user(context_id, user_src, host_src, (uint64_t)len) != 0) {
            return -1;
        }
    }
    return 0;
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
        g_wasm_fs_peer_slots[i].pid = 0;
        g_wasm_fs_peer_slots[i].valid = 0;
        g_wasm_fs_peer_slots[i].peer_context_id = 0;
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

static int
wasm_user_va_from_offset(uint32_t context_id,
                         uint32_t offset,
                         uint32_t span,
                         uint64_t *out_user_va)
{
    if (context_id == 0 || span == 0 || !out_user_va) {
        return -1;
    }
    mm_context_t *ctx = mm_context_get(context_id);
    if (!ctx) {
        return -1;
    }
    mem_region_t linear = {0};
    if (mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &linear) != 0) {
        return -1;
    }
    uint64_t off = (uint64_t)offset;
    uint64_t len = (uint64_t)span;
    if (off > linear.size || len > (linear.size - off)) {
        return -1;
    }
    *out_user_va = linear.base + off;
    return 0;
}

static int
wasm_user_va_from_host_ptr(uint32_t context_id,
                           const uint8_t *mem_base,
                           uint64_t mem_size,
                           const void *host_ptr,
                           uint32_t span,
                           uint64_t *out_user_va)
{
    if (!mem_base || !host_ptr || span == 0 || !out_user_va) {
        return -1;
    }
    const uint8_t *ptr = (const uint8_t *)host_ptr;
    if (ptr < mem_base) {
        return -1;
    }
    uint64_t off = (uint64_t)(ptr - mem_base);
    if (off > mem_size || (uint64_t)span > (mem_size - off)) {
        return -1;
    }
    return wasm_user_va_from_offset(context_id, (uint32_t)off, span, out_user_va);
}

static int
require_io_capability(uint32_t context_id)
{
    return policy_authorize(context_id, POLICY_ACTION_IO_PORT, 0);
}

static int
require_mmio_capability(uint32_t context_id)
{
    return policy_authorize(context_id, POLICY_ACTION_MMIO_MAP, 0);
}

static int
require_dma_capability(uint32_t context_id)
{
    return policy_authorize(context_id, POLICY_ACTION_DMA_BUFFER, 0);
}

static int
require_irq_route_capability(uint32_t context_id)
{
    return policy_authorize(context_id, POLICY_ACTION_IRQ_CONTROL, 0);
}

static int
require_system_control_capability(uint32_t context_id)
{
    return policy_authorize(context_id, POLICY_ACTION_SYSTEM_CONTROL, 0);
}

static int
wasm_console_should_mirror_to_vt(void)
{
    process_t *proc = process_get(process_current_pid());
    if (!proc) {
        return 0;
    }

    if (proc->name &&
        memcmp(proc->name, "hello-", 6) == 0 &&
        (proc->name[6] != '\0')) {
        return 1;
    }

    if (proc->parent_pid == 0) {
        return 0;
    }

    process_t *parent = process_get(proc->parent_pid);
    if (!parent || !parent->name) {
        return 0;
    }

    /* Restrict mirrored VT output to shell-launched app workloads for now.
     * FIXME: Replace this parent-name heuristic with explicit per-process
     * console routing policy once PM exposes tty ownership metadata. */
    return strcmp(parent->name, "cli") == 0;
}

static void
wasm_console_write_vt_mirror(const char *ptr, int32_t len)
{
    uint32_t vt_endpoint = process_manager_vt_endpoint();
    if (vt_endpoint == IPC_ENDPOINT_NONE || !ptr || len <= 0 ||
        !wasm_console_should_mirror_to_vt()) {
        return;
    }

    for (int32_t offset = 0; offset < len; ) {
        ipc_message_t msg;
        int32_t chunk[4] = { 0, 0, 0, 0 };

        for (int i = 0; i < 4 && offset < len; ++i, ++offset) {
            chunk[i] = (int32_t)(uint8_t)ptr[offset];
        }

        msg.type = VT_IPC_WRITE_REQ;
        msg.source = IPC_ENDPOINT_NONE;
        msg.destination = vt_endpoint;
        msg.request_id = 0;
        msg.arg0 = (uint32_t)chunk[0];
        msg.arg1 = (uint32_t)chunk[1];
        msg.arg2 = (uint32_t)chunk[2];
        msg.arg3 = (uint32_t)chunk[3];

        if (ipc_send_from(IPC_CONTEXT_KERNEL, vt_endpoint, &msg) != IPC_OK) {
            break;
        }
    }
}

static wasm_fs_peer_slot_t *
wasm_fs_peer_slot_for_pid(uint32_t pid)
{
    wasm_fs_peer_slot_t *empty = 0;

    if (pid == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_wasm_fs_peer_slots[i].pid == pid) {
            return &g_wasm_fs_peer_slots[i];
        }
        if (!empty && g_wasm_fs_peer_slots[i].pid == 0) {
            empty = &g_wasm_fs_peer_slots[i];
        }
    }

    if (empty) {
        empty->pid = pid;
        empty->valid = 0;
        empty->peer_context_id = 0;
    }
    return empty;
}

static void *
wasm_fs_buffer_for_pid(uint32_t pid, uint32_t context_id)
{
    uint32_t target_context = context_id;
    wasm_fs_peer_slot_t *peer = wasm_fs_peer_slot_for_pid(pid);
    if (peer && peer->valid && peer->peer_context_id != 0) {
        target_context = peer->peer_context_id;
    }
    return process_manager_fs_buffer_for_context(target_context);
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
                wasm_fs_peer_slot_t *peer = wasm_fs_peer_slot_for_pid(pid);
                if (peer &&
                    slot->message.type >= FS_IPC_OPEN_REQ &&
                    slot->message.type <= FS_IPC_READ_APP_REQ) {
                    uint32_t owner_context = 0;
                    if (ipc_endpoint_owner(slot->message.source, &owner_context) == IPC_OK &&
                        owner_context != 0) {
                        peer->valid = 1;
                        peer->peer_context_id = owner_context;
                    } else {
                        peer->valid = 0;
                        peer->peer_context_id = 0;
                    }
                }
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
        wasm_fs_peer_slot_t *peer = wasm_fs_peer_slot_for_pid(pid);
        if (peer &&
            slot->message.type >= FS_IPC_OPEN_REQ &&
            slot->message.type <= FS_IPC_READ_APP_REQ) {
            uint32_t owner_context = 0;
            if (ipc_endpoint_owner(slot->message.source, &owner_context) == IPC_OK &&
                owner_context != 0) {
                peer->valid = 1;
                peer->peer_context_id = owner_context;
            } else {
                peer->valid = 0;
                peer->peer_context_id = 0;
            }
        }
        preempt_safepoint();
        m3ApiReturn(1);
    }
}

m3ApiRawFunction(wasmos_ipc_try_recv)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, endpoint)
    uint32_t context_id = 0;
    uint32_t pid = process_current_pid();
    wasm_ipc_last_slot_t *slot;
    int rc;

    if (endpoint < 0 || current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }

    slot = wasm_ipc_slot_for_pid(pid);
    if (!slot) {
        m3ApiReturn(-1);
    }

    preempt_safepoint();
    rc = ipc_recv_for(context_id, (uint32_t)endpoint, &slot->message);
    if (rc == IPC_EMPTY) {
        m3ApiReturn(0); /* no message — return without blocking */
    }
    if (rc != IPC_OK) {
        m3ApiReturn(-1);
    }
    slot->valid = 1;
    preempt_safepoint();
    m3ApiReturn(1);
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
#define WASM_BLOCK_BUFFER_SIZE_BYTES (WASM_BLOCK_BUFFER_PAGES * 4096u)

static int
wasm_block_buffer_validate_args(wasm_block_slot_t *slot,
                                int32_t phys,
                                int32_t len,
                                int32_t offset)
{
    (void)slot;
    uint64_t start = 0;
    uint64_t end = 0;

    if (phys <= 0 || len <= 0 || offset < 0) {
        return -1;
    }
    start = (uint64_t)(uint32_t)phys + (uint64_t)(uint32_t)offset;
    end = start + (uint64_t)(uint32_t)len;
    if (end < start) {
        return -1;
    }
    if (end > 0x100000000ULL) {
        return -1;
    }
    return 0;
}

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
    uint32_t pid = process_current_pid();
    wasm_block_slot_t *slot = wasm_block_slot_for_pid(pid);

    if (wasm_block_buffer_validate_args(slot, phys, len, offset) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(ptr, (uint32_t)len);
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   ptr,
                                   (uint32_t)len,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                ptr_user,
                                (uint64_t)(uint32_t)len,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(-1);
    }

    const uint8_t *src = (const uint8_t *)(uintptr_t)((uint32_t)phys + (uint32_t)offset);
    if (wasm_copy_to_user_sync_views(proc->context_id,
                                     ptr_user,
                                     ptr,
                                     src,
                                     (uint32_t)len) != 0) {
        m3ApiReturn(-1);
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
    uint32_t pid = process_current_pid();
    wasm_block_slot_t *slot = wasm_block_slot_for_pid(pid);

    if (wasm_block_buffer_validate_args(slot, phys, len, offset) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(ptr, (uint32_t)len);
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   ptr,
                                   (uint32_t)len,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                ptr_user,
                                (uint64_t)(uint32_t)len,
                                MEM_REGION_FLAG_READ) != 0) {
        m3ApiReturn(-1);
    }

    uint8_t *dst = (uint8_t *)(uintptr_t)((uint32_t)phys + (uint32_t)offset);
    uint32_t copied = 0;
    uint8_t bounce[256];
    while (copied < (uint32_t)len) {
        uint32_t chunk = (uint32_t)len - copied;
        if (chunk > (uint32_t)sizeof(bounce)) {
            chunk = (uint32_t)sizeof(bounce);
        }
        if (wasm_copy_from_user_sync_views(proc->context_id,
                                           ptr_user + (uint64_t)copied,
                                           ptr + copied,
                                           bounce,
                                           chunk) != 0) {
            m3ApiReturn(-1);
        }
        for (uint32_t i = 0; i < chunk; ++i) {
            dst[copied + i] = bounce[i];
        }
        copied += chunk;
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_fs_buffer_size)
{
    m3ApiReturnType(int32_t)
    m3ApiReturn((int32_t)process_manager_fs_buffer_size());
}

m3ApiRawFunction(wasmos_fs_endpoint)
{
    m3ApiReturnType(int32_t)

    uint32_t endpoint = process_manager_fs_endpoint();
    if (endpoint == IPC_ENDPOINT_NONE) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)endpoint);
}

m3ApiRawFunction(wasmos_fs_buffer_copy)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(uint8_t *, ptr)
    m3ApiGetArg(int32_t, len)
    m3ApiGetArg(int32_t, offset)
    uint32_t context_id = 0;
    uint32_t pid = process_current_pid();

    if (len <= 0 || offset < 0) {
        m3ApiReturn(-1);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }
    uint32_t max_len = process_manager_fs_buffer_size();
    if ((uint32_t)offset + (uint32_t)len > max_len) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(ptr, (uint32_t)len);
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   ptr,
                                   (uint32_t)len,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(context_id,
                                ptr_user,
                                (uint64_t)(uint32_t)len,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(-1);
    }

    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    const uint8_t *src = (const uint8_t *)wasm_fs_buffer_for_pid(pid, context_id);
    if (!src) {
        m3ApiReturn(-1);
    }
    if (wasm_copy_to_user_sync_views(proc->context_id,
                                     ptr_user,
                                     ptr,
                                     src + offset,
                                     (uint32_t)len) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_fs_buffer_write)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(const uint8_t *, ptr)
    m3ApiGetArg(int32_t, len)
    m3ApiGetArg(int32_t, offset)
    uint32_t context_id = 0;
    uint32_t pid = process_current_pid();

    if (len <= 0 || offset < 0) {
        m3ApiReturn(-1);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }
    uint32_t max_len = process_manager_fs_buffer_size();
    if ((uint32_t)offset + (uint32_t)len > max_len) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(ptr, (uint32_t)len);
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   ptr,
                                   (uint32_t)len,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(context_id,
                                ptr_user,
                                (uint64_t)(uint32_t)len,
                                MEM_REGION_FLAG_READ) != 0) {
        m3ApiReturn(-1);
    }

    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint8_t *dst = (uint8_t *)wasm_fs_buffer_for_pid(pid, context_id);
    if (!dst) {
        m3ApiReturn(-1);
    }
    uint32_t copied = 0;
    uint8_t bounce[256];
    while (copied < (uint32_t)len) {
        uint32_t chunk = (uint32_t)len - copied;
        if (chunk > (uint32_t)sizeof(bounce)) {
            chunk = (uint32_t)sizeof(bounce);
        }
        if (wasm_copy_from_user_sync_views(proc->context_id,
                                           ptr_user + (uint64_t)copied,
                                           ptr + copied,
                                           bounce,
                                           chunk) != 0) {
            m3ApiReturn(-1);
        }
        for (uint32_t i = 0; i < chunk; ++i) {
            dst[(uint32_t)offset + copied + i] = bounce[i];
        }
        copied += chunk;
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_early_log_size)
{
    m3ApiReturnType(int32_t)
    m3ApiReturn((int32_t)serial_early_log_size());
}

m3ApiRawFunction(wasmos_early_log_copy)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(uint8_t *, ptr)
    m3ApiGetArg(int32_t, len)
    m3ApiGetArg(int32_t, offset)

    if (len < 0 || offset < 0) {
        m3ApiReturn(-1);
    }
    uint32_t total = serial_early_log_size();
    uint32_t start = (uint32_t)offset;
    uint32_t count = (uint32_t)len;
    if (start > total || count > total - start) {
        m3ApiReturn(-1);
    }
    if (count == 0) {
        m3ApiReturn(0);
    }
    m3ApiCheckMem(ptr, count);
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   ptr,
                                   count,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                ptr_user,
                                count,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(-1);
    }
    uint32_t copied = 0;
    uint8_t bounce[256];
    while (copied < count) {
        uint32_t chunk = count - copied;
        if (chunk > (uint32_t)sizeof(bounce)) {
            chunk = (uint32_t)sizeof(bounce);
        }
        serial_early_log_copy(bounce, start + copied, chunk);
        if (mm_copy_to_user(proc->context_id,
                            ptr_user + (uint64_t)copied,
                            bounce,
                            (uint64_t)chunk) != 0) {
            m3ApiReturn(-1);
        }
        copied += chunk;
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_boot_config_size)
{
    m3ApiReturnType(int32_t)
    if (!g_wasm_boot_info || !g_wasm_boot_info->boot_config || g_wasm_boot_info->boot_config_size == 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)g_wasm_boot_info->boot_config_size);
}

m3ApiRawFunction(wasmos_boot_config_copy)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(uint8_t *, ptr)
    m3ApiGetArg(int32_t, len)
    m3ApiGetArg(int32_t, offset)

    if (len < 0 || offset < 0) {
        m3ApiReturn(-1);
    }
    if (!g_wasm_boot_info || !g_wasm_boot_info->boot_config || g_wasm_boot_info->boot_config_size == 0) {
        m3ApiReturn(-1);
    }

    uint32_t total = g_wasm_boot_info->boot_config_size;
    uint32_t start = (uint32_t)offset;
    uint32_t count = (uint32_t)len;
    if (start > total || count > total - start) {
        m3ApiReturn(-1);
    }
    if (count == 0) {
        m3ApiReturn(0);
    }

    m3ApiCheckMem(ptr, count);
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   ptr,
                                   count,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                ptr_user,
                                count,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(-1);
    }
    const uint8_t *src = (const uint8_t *)(uintptr_t)g_wasm_boot_info->boot_config;
    if (wasm_copy_to_user_bytes(proc->context_id,
                                ptr_user,
                                src + start,
                                count) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_io_in8)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, port)
    uint32_t context_id = 0;
    if (port < 0 || port > 0xFFFF) {
        m3ApiReturn(-1);
    }
    if (current_process_context(&context_id) != 0 || require_io_capability(context_id) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)inb((uint16_t)port));
}

m3ApiRawFunction(wasmos_io_in16)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, port)
    uint32_t context_id = 0;
    if (port < 0 || port > 0xFFFF) {
        m3ApiReturn(-1);
    }
    if (current_process_context(&context_id) != 0 || require_io_capability(context_id) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)inw((uint16_t)port));
}

m3ApiRawFunction(wasmos_io_out8)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, port)
    m3ApiGetArg(int32_t, value)
    uint32_t context_id = 0;
    if (port < 0 || port > 0xFFFF || value < 0 || value > 0xFF) {
        m3ApiReturn(-1);
    }
    if (current_process_context(&context_id) != 0 || require_io_capability(context_id) != 0) {
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
    uint32_t context_id = 0;
    if (port < 0 || port > 0xFFFF || value < 0 || value > 0xFFFF) {
        m3ApiReturn(-1);
    }
    if (current_process_context(&context_id) != 0 || require_io_capability(context_id) != 0) {
        m3ApiReturn(-1);
    }
    outw((uint16_t)port, (uint16_t)value);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_io_wait)
{
    m3ApiReturnType(int32_t)
    uint32_t context_id = 0;
    if (current_process_context(&context_id) != 0 || require_io_capability(context_id) != 0) {
        m3ApiReturn(-1);
    }
    io_wait();
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_framebuffer_pixel)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, x)
    m3ApiGetArg(int32_t, y)
    m3ApiGetArg(int32_t, color)
    if (framebuffer_put_pixel((uint32_t)x, (uint32_t)y, (uint32_t)color) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_framebuffer_info)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(uint8_t *, out_ptr)
    m3ApiGetArg(int32_t, len)

    if (len < (int32_t)sizeof(framebuffer_info_t) || len <= 0) {
        m3ApiReturn(-1);
    }
    if (!out_ptr) {
        m3ApiReturn(-1);
    }
    framebuffer_info_t info = {0};
    if (framebuffer_get_info(&info) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(out_ptr, (uint32_t)len);
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t out_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   out_ptr,
                                   (uint32_t)len,
                                   &out_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                out_user,
                                (uint64_t)(uint32_t)len,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(-1);
    }
    if (mm_copy_to_user(proc->context_id,
                        out_user,
                        &info,
                        sizeof(info)) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_framebuffer_map)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, ptr)
    m3ApiGetArg(int32_t, size)

    if (ptr < 0 || size <= 0) {
        m3ApiReturn(-1);
    }
    if ((size & 0xFFF) != 0) {
        m3ApiReturn(-1);
    }

    framebuffer_info_t info = {0};
    if (framebuffer_get_info(&info) != 0) {
        m3ApiReturn(-1);
    }
    if ((uint32_t)size < info.framebuffer_size) {
        m3ApiReturn(-1);
    }

    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0 ||
        require_mmio_capability(proc->context_id) != 0) {
        m3ApiReturn(-1);
    }

    mm_context_t *ctx = mm_context_get(proc->context_id);
    if (!ctx || ctx->root_table == 0) {
        m3ApiReturn(-1);
    }

    /* Map the physical framebuffer over caller-provided linear-memory pages.
     * Resolve the WASM offset into the process-owned user VA explicitly. */
    uint32_t off32 = (uint32_t)ptr;
    uint32_t map_size32 = (uint32_t)size;
    if ((uint64_t)off32 + (uint64_t)map_size32 > (uint64_t)m3_GetMemorySize(runtime)) {
        m3ApiReturn(-1);
    }
    uint64_t virt = 0;
    if (wasm_user_va_from_offset(proc->context_id, off32, map_size32, &virt) != 0 ||
        mm_user_range_permitted(proc->context_id, virt, (uint64_t)map_size32, MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(-1);
    }
    if ((virt & 0xFFFULL) != 0) {
        m3ApiReturn(-1);
    }

    uint64_t pages = (uint64_t)map_size32 / 0x1000ULL;
    if (pages == 0) {
        serial_write("[framebuffer-map] zero pages\n");
        m3ApiReturn(-1);
    }
    uint64_t cur_virt = virt;
    uint64_t cur_phys = info.framebuffer_base;
    for (uint64_t i = 0; i < pages; ++i) {
        (void)paging_unmap_4k_in_root(ctx->root_table, cur_virt);
        if (paging_map_4k_in_root(ctx->root_table,
                                 cur_virt,
                                 cur_phys,
                                 MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE) < 0) {
            m3ApiReturn(-1);
        }
        cur_virt += 0x1000ULL;
        cur_phys += 0x1000ULL;
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_shmem_create)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, pages)
    m3ApiGetArg(int32_t, flags)

    if (pages <= 0) {
        m3ApiReturn(-1);
    }

    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0 ||
        require_dma_capability(proc->context_id) != 0) {
        m3ApiReturn(-1);
    }

    uint32_t id = 0;
    uint64_t phys = 0;
    uint32_t create_flags = (flags > 0)
                                ? (uint32_t)flags
                                : (MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE);
    if (mm_shared_create(proc->context_id, (uint64_t)(uint32_t)pages, create_flags, &id, &phys) != 0) {
        m3ApiReturn(-1);
    }
    (void)phys;
    m3ApiReturn((int32_t)id);
}

m3ApiRawFunction(wasmos_shmem_map)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, id)
    m3ApiGetArg(int32_t, ptr)
    m3ApiGetArg(int32_t, size)

    if (id <= 0 || ptr < 0 || size <= 0 || (size & 0xFFF) != 0) {
        m3ApiReturn(-1);
    }

    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0 ||
        require_dma_capability(proc->context_id) != 0) {
        m3ApiReturn(-1);
    }
    mm_context_t *ctx = mm_context_get(proc->context_id);
    if (!ctx || ctx->root_table == 0) {
        m3ApiReturn(-1);
    }

    uint64_t phys_base = 0;
    uint64_t shared_pages = 0;
    if (mm_shared_get_phys(proc->context_id, (uint32_t)id, &phys_base, &shared_pages) != 0 ||
        shared_pages == 0) {
        m3ApiReturn(-1);
    }
    uint64_t map_size = (uint64_t)(uint32_t)size;
    uint64_t needed_size = shared_pages * 0x1000ULL;
    if (map_size < needed_size) {
        m3ApiReturn(-1);
    }

    uint32_t off32 = (uint32_t)ptr;
    uint32_t map_size32 = (uint32_t)size;
    if ((uint64_t)off32 + (uint64_t)map_size32 > (uint64_t)m3_GetMemorySize(runtime)) {
        m3ApiReturn(-1);
    }
    uint64_t virt = 0;
    if (wasm_user_va_from_offset(proc->context_id, off32, map_size32, &virt) != 0 ||
        mm_user_range_permitted(proc->context_id, virt, (uint64_t)map_size32, MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(-1);
    }
    if ((virt & 0xFFFULL) != 0) {
        m3ApiReturn(-1);
    }

    for (uint64_t i = 0; i < shared_pages; ++i) {
        uint64_t cur_virt = virt + (i * 0x1000ULL);
        uint64_t cur_phys = phys_base + (i * 0x1000ULL);
        (void)paging_unmap_4k_in_root(ctx->root_table, cur_virt);
        if (paging_map_4k_in_root(ctx->root_table,
                                  cur_virt,
                                  cur_phys,
                                  MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE) < 0) {
            m3ApiReturn(-1);
        }
    }

    if (mm_shared_retain(proc->context_id, (uint32_t)id) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_shmem_grant)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, id)
    m3ApiGetArg(int32_t, target_pid)

    process_t *proc = process_get(process_current_pid());
    process_t *target = 0;
    if (id <= 0 || target_pid <= 0) {
        m3ApiReturn(-1);
    }
    if (!proc || proc->context_id == 0 ||
        require_dma_capability(proc->context_id) != 0) {
        m3ApiReturn(-1);
    }
    target = process_get((uint32_t)target_pid);
    if (!target || target->context_id == 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(mm_shared_grant(proc->context_id, (uint32_t)id, target->context_id));
}

m3ApiRawFunction(wasmos_shmem_unmap)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, id)

    if (id <= 0) {
        m3ApiReturn(-1);
    }
    /* FIXME: This currently only releases shared-region ownership/refcount.
     * It does not restore the previous linear-memory page mappings. */
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(mm_shared_release(proc->context_id, (uint32_t)id));
}

m3ApiRawFunction(wasmos_irq_route)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, irq_line)
    m3ApiGetArg(int32_t, endpoint)

    uint32_t context_id = 0;
    if (irq_line < 0 || endpoint < 0) {
        m3ApiReturn(-1);
    }
    if (current_process_context(&context_id) != 0 ||
        require_irq_route_capability(context_id) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(irq_register(context_id, (uint32_t)irq_line, (uint32_t)endpoint));
}

m3ApiRawFunction(wasmos_irq_unroute)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, irq_line)

    uint32_t context_id = 0;
    if (irq_line < 0) {
        m3ApiReturn(-1);
    }
    if (current_process_context(&context_id) != 0 ||
        require_irq_route_capability(context_id) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(irq_unregister(context_id, (uint32_t)irq_line));
}

m3ApiRawFunction(wasmos_system_halt)
{
    m3ApiReturnType(int32_t)
    uint32_t context_id = 0;
    if (current_process_context(&context_id) != 0 ||
        require_system_control_capability(context_id) != 0) {
        m3ApiReturn(-1);
    }
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
    uint32_t context_id = 0;
    if (current_process_context(&context_id) != 0 ||
        require_system_control_capability(context_id) != 0) {
        m3ApiReturn(-1);
    }
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
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t out_user = 0;
    uint64_t out_len_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   out_ptr,
                                   len,
                                   &out_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                out_user,
                                len,
                                MEM_REGION_FLAG_WRITE) != 0 ||
        wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   out_len_ptr,
                                   sizeof(uint32_t),
                                   &out_len_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                out_len_user,
                                sizeof(uint32_t),
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(-1);
    }

    const uint8_t *src = (const uint8_t *)(uintptr_t)g_wasm_boot_info->rsdp;
    if (wasm_copy_to_user_bytes(proc->context_id,
                                out_user,
                                src,
                                len) != 0) {
        m3ApiReturn(-1);
    }
    if (wasm_copy_to_user_bytes(proc->context_id,
                                out_len_user,
                                &len,
                                sizeof(len)) != 0) {
        m3ApiReturn(-1);
    }
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
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t out_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   out_ptr,
                                   (uint32_t)out_len,
                                   &out_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                out_user,
                                (uint64_t)(uint32_t)out_len,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(-1);
    }

    char local_name[64];
    uint32_t name_len = 0;
    if (boot_module_name_at((uint32_t)index, local_name, sizeof(local_name), &name_len) != 0) {
        m3ApiReturn(-1);
    }
    uint32_t copy_len = name_len;
    if (copy_len >= (uint32_t)out_len) {
        copy_len = (uint32_t)out_len - 1U;
    }
    if (wasm_copy_to_user_bytes(proc->context_id,
                                out_user,
                                local_name,
                                copy_len) != 0) {
        m3ApiReturn(-1);
    }
    char nul = '\0';
    if (wasm_copy_to_user_bytes(proc->context_id,
                                out_user + (uint64_t)copy_len,
                                &nul,
                                1) != 0) {
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
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   ptr,
                                   (uint32_t)len,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                ptr_user,
                                (uint64_t)(uint32_t)len,
                                MEM_REGION_FLAG_READ) != 0) {
        m3ApiReturn(-1);
    }

    preempt_disable();
    char buf[128];
    uint32_t copied = 0;
    while (copied < (uint32_t)len) {
        uint32_t chunk = (uint32_t)len - copied;
        if (chunk > (uint32_t)(sizeof(buf) - 1U)) {
            chunk = (uint32_t)(sizeof(buf) - 1U);
        }
        if (wasm_copy_from_user_sync_views(proc->context_id,
                                           ptr_user + (uint64_t)copied,
                                           ptr + copied,
                                           buf,
                                           chunk) != 0) {
            preempt_enable();
            m3ApiReturn(-1);
        }
        buf[chunk] = '\0';
        serial_write(buf);
        wasm_console_write_vt_mirror(buf, (int32_t)chunk);
        copied += chunk;
    }
    preempt_enable();
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_debug_mark)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, tag)
    trace_write_unlocked("[wasm] debug_mark tag=");
    trace_do(serial_write_hex64_unlocked((uint64_t)(uint32_t)tag));
    trace_write_unlocked("[wasm] debug_mark pid=");
    trace_do(serial_write_hex64_unlocked((uint64_t)process_current_pid()));
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_kmap_dump)
{
    m3ApiReturnType(int32_t)
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t root = mm_context_root_table(proc->context_id);
    if (!root) {
        m3ApiReturn(-1);
    }
    paging_dump_user_root_kernel_mappings(root);
    if ((proc->ctx.cs & 0x3u) == 0x3u) {
        m3ApiReturn(paging_verify_user_root_no_low_slot(root, 1) == 0 ? 0 : -1);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_kmap_dump_all)
{
    m3ApiReturnType(int32_t)
    uint32_t count = process_count_active();
    int failures = 0;

    trace_do(serial_write("[kmap] contexts begin\n"));
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t pid = 0;
        uint32_t parent_pid = 0;
        const char *name = 0;
        if (process_info_at_ex(i, &pid, &parent_pid, &name) != 0) {
            continue;
        }
        process_t *proc = process_get(pid);
        if (!proc || proc->context_id == 0) {
            continue;
        }
        uint64_t root = mm_context_root_table(proc->context_id);
        if (!root) {
            failures++;
            continue;
        }

        trace_do(serial_write("[kmap] pid="));
        trace_do(serial_write_hex64((uint64_t)pid));
        trace_do(serial_write(" parent="));
        trace_do(serial_write_hex64((uint64_t)parent_pid));
        trace_do(serial_write(" ctx="));
        trace_do(serial_write_hex64((uint64_t)proc->context_id));
        trace_do(serial_write(" name="));
        trace_do(serial_write(name ? name : "(unknown)"));
        trace_do(serial_write("\n"));

        paging_dump_user_root_kernel_mappings(root);
        if ((proc->ctx.cs & 0x3u) == 0x3u) {
            if (paging_verify_user_root_no_low_slot(root, 1) != 0) {
                failures++;
            }
        }
    }
    trace_do(serial_write("[kmap] contexts end\n"));
    m3ApiReturn(failures == 0 ? 0 : -1);
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
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   ptr,
                                   1,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                ptr_user,
                                1,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(-1);
    }
    uint8_t ch = 0;
    int rc = serial_read_char(&ch);
    if (rc <= 0) {
        m3ApiReturn(rc);
    }
    char out = (char)ch;
    if (wasm_copy_to_user_sync_views(proc->context_id,
                                     ptr_user,
                                     ptr,
                                     &out,
                                     1) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(1);
}

m3ApiRawFunction(wasmos_sync_user_read)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(uint8_t *, ptr)
    m3ApiGetArg(int32_t, len)

    if (len < 0) {
        m3ApiReturn(-1);
    }
    if (len == 0) {
        m3ApiReturn(0);
    }
    m3ApiCheckMem(ptr, (uint32_t)len);
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   ptr,
                                   (uint32_t)len,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                ptr_user,
                                (uint64_t)(uint32_t)len,
                                MEM_REGION_FLAG_READ) != 0) {
        m3ApiReturn(-1);
    }
    if (mm_copy_from_user(proc->context_id,
                          ptr,
                          ptr_user,
                          (uint64_t)(uint32_t)len) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_input_push)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, ch)
    serial_input_push((uint8_t)(ch & 0xFF));
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_input_read)
{
    m3ApiReturnType(int32_t)
    uint8_t ch = 0;
    if (serial_input_read(&ch)) {
        m3ApiReturn((int32_t)ch);
    }
    m3ApiReturn(-1);
}

m3ApiRawFunction(wasmos_serial_register)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, endpoint)
    m3ApiReturn(serial_register_remote_driver((uint32_t)endpoint));
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
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t buf_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   buf,
                                   (uint32_t)buf_len,
                                   &buf_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                buf_user,
                                (uint64_t)(uint32_t)buf_len,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(-1);
    }

    uint32_t pid = 0;
    const char *name = 0;
    if (process_info_at((uint32_t)index, &pid, &name) != 0) {
        m3ApiReturn(-1);
    }
    uint32_t out_cap = (uint32_t)buf_len;
    uint32_t copied = 0;
    char bounce[256];
    if (name) {
        while (name[copied] && copied + 1U < out_cap) {
            copied++;
        }
    }
    uint32_t out_len = copied + 1U; /* NUL-terminated */
    for (uint32_t i = 0; i < copied; ++i) {
        bounce[i % sizeof(bounce)] = name[i];
        if ((i % sizeof(bounce)) == (sizeof(bounce) - 1U)) {
            uint32_t chunk_base = i + 1U - (uint32_t)sizeof(bounce);
            if (mm_copy_to_user(proc->context_id,
                                buf_user + (uint64_t)chunk_base,
                                bounce,
                                (uint64_t)sizeof(bounce)) != 0) {
                m3ApiReturn(-1);
            }
        }
    }
    uint32_t tail = copied % (uint32_t)sizeof(bounce);
    if (tail > 0) {
        if (mm_copy_to_user(proc->context_id,
                            buf_user + (uint64_t)(copied - tail),
                            bounce,
                            (uint64_t)tail) != 0) {
            m3ApiReturn(-1);
        }
    }
    bounce[0] = '\0';
    if (mm_copy_to_user(proc->context_id,
                        buf_user + (uint64_t)(out_len - 1U),
                        bounce,
                        1) != 0) {
        m3ApiReturn(-1);
    }
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
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t buf_user = 0;
    uint64_t parent_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   buf,
                                   (uint32_t)buf_len,
                                   &buf_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                buf_user,
                                (uint64_t)(uint32_t)buf_len,
                                MEM_REGION_FLAG_WRITE) != 0 ||
        wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   parent_ptr,
                                   sizeof(uint32_t),
                                   &parent_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                parent_user,
                                sizeof(uint32_t),
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(-1);
    }

    uint32_t pid = 0;
    uint32_t parent_pid = 0;
    const char *name = 0;
    if (process_info_at_ex((uint32_t)index, &pid, &parent_pid, &name) != 0) {
        m3ApiReturn(-1);
    }
    if (mm_copy_to_user(proc->context_id,
                        parent_user,
                        &parent_pid,
                        sizeof(parent_pid)) != 0) {
        m3ApiReturn(-1);
    }

    uint32_t out_cap = (uint32_t)buf_len;
    uint32_t copied = 0;
    char bounce[256];
    if (name) {
        while (name[copied] && copied + 1U < out_cap) {
            copied++;
        }
    }
    uint32_t out_len = copied + 1U; /* NUL-terminated */
    for (uint32_t i = 0; i < copied; ++i) {
        bounce[i % sizeof(bounce)] = name[i];
        if ((i % sizeof(bounce)) == (sizeof(bounce) - 1U)) {
            uint32_t chunk_base = i + 1U - (uint32_t)sizeof(bounce);
            if (mm_copy_to_user(proc->context_id,
                                buf_user + (uint64_t)chunk_base,
                                bounce,
                                (uint64_t)sizeof(bounce)) != 0) {
                m3ApiReturn(-1);
            }
        }
    }
    uint32_t tail = copied % (uint32_t)sizeof(bounce);
    if (tail > 0) {
        if (mm_copy_to_user(proc->context_id,
                            buf_user + (uint64_t)(copied - tail),
                            bounce,
                            (uint64_t)tail) != 0) {
            m3ApiReturn(-1);
        }
    }
    bounce[0] = '\0';
    if (mm_copy_to_user(proc->context_id,
                        buf_user + (uint64_t)(out_len - 1U),
                        bounce,
                        1) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)pid);
}

m3ApiRawFunction(wasmos_strlen)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(const char *, ptr)

    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(0);
    }
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   ptr,
                                   1,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                ptr_user,
                                1,
                                MEM_REGION_FLAG_READ) != 0) {
        m3ApiReturn(0);
    }

    const uint8_t *start = (const uint8_t *)ptr;
    const uint8_t *end = (const uint8_t *)_mem + m3_GetMemorySize(runtime);
    if ((const uint8_t *)ptr < (const uint8_t *)_mem || start >= end) {
        m3ApiReturn(0);
    }
    int32_t len = 0;
    uint64_t max_len = (uint64_t)(end - start);
    for (uint64_t i = 0; i < max_len; ++i) {
        char ch = 0;
        if (wasm_copy_from_user_sync_views(proc->context_id,
                                           ptr_user + i,
                                           start + i,
                                           &ch,
                                           1) != 0) {
            m3ApiReturn(0);
        }
        if (ch == '\0') {
            break;
        }
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
    rc |= wasm3_link_raw(module, "wasmos", "ipc_try_recv", "i(i)", wasmos_ipc_try_recv);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_wait", "i(i)", wasmos_ipc_wait);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_notify", "i(i)", wasmos_ipc_notify);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_last_field", "i(i)", wasmos_ipc_last_field);
    rc |= wasm3_link_raw(module, "wasmos", "console_write", "i(*i)", wasmos_console_write);
    rc |= wasm3_link_raw(module, "wasmos", "debug_mark", "i(i)", wasmos_debug_mark);
    rc |= wasm3_link_raw(module, "wasmos", "kmap_dump", "i()", wasmos_kmap_dump);
    rc |= wasm3_link_raw(module, "wasmos", "kmap_dump_all", "i()", wasmos_kmap_dump_all);
    rc |= wasm3_link_raw(module, "wasmos", "console_read", "i(*i)", wasmos_console_read);
    rc |= wasm3_link_raw(module, "wasmos", "sync_user_read", "i(*i)", wasmos_sync_user_read);
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
    rc |= wasm3_link_raw(module, "wasmos", "fs_endpoint", "i()", wasmos_fs_endpoint);
    rc |= wasm3_link_raw(module, "wasmos", "fs_buffer_copy", "i(*ii)", wasmos_fs_buffer_copy);
    rc |= wasm3_link_raw(module, "wasmos", "fs_buffer_write", "i(*ii)", wasmos_fs_buffer_write);
    rc |= wasm3_link_raw(module, "wasmos", "early_log_size", "i()", wasmos_early_log_size);
    rc |= wasm3_link_raw(module, "wasmos", "early_log_copy", "i(*ii)", wasmos_early_log_copy);
    rc |= wasm3_link_raw(module, "wasmos", "boot_config_size", "i()", wasmos_boot_config_size);
    rc |= wasm3_link_raw(module, "wasmos", "boot_config_copy", "i(*ii)", wasmos_boot_config_copy);
    rc |= wasm3_link_raw(module, "wasmos", "system_halt", "i()", wasmos_system_halt);
    rc |= wasm3_link_raw(module, "wasmos", "system_reboot", "i()", wasmos_system_reboot);
    rc |= wasm3_link_raw(module, "wasmos", "acpi_rsdp_info", "i(**i)", wasmos_acpi_rsdp_info);
    rc |= wasm3_link_raw(module, "wasmos", "boot_module_name", "i(i*i)", wasmos_boot_module_name);
    rc |= wasm3_link_raw(module, "wasmos", "io_in8", "i(i)", wasmos_io_in8);
    rc |= wasm3_link_raw(module, "wasmos", "io_in16", "i(i)", wasmos_io_in16);
    rc |= wasm3_link_raw(module, "wasmos", "io_out8", "i(ii)", wasmos_io_out8);
    rc |= wasm3_link_raw(module, "wasmos", "io_out16", "i(ii)", wasmos_io_out16);
    rc |= wasm3_link_raw(module, "wasmos", "io_wait", "i()", wasmos_io_wait);
    rc |= wasm3_link_raw(module, "wasmos", "framebuffer_info", "i(ii)", wasmos_framebuffer_info);
    rc |= wasm3_link_raw(module, "wasmos", "framebuffer_map", "i(ii)", wasmos_framebuffer_map);
    rc |= wasm3_link_raw(module, "wasmos", "framebuffer_pixel", "i(iii)", wasmos_framebuffer_pixel);
    rc |= wasm3_link_raw(module, "wasmos", "shmem_create", "i(ii)", wasmos_shmem_create);
    rc |= wasm3_link_raw(module, "wasmos", "shmem_grant", "i(ii)", wasmos_shmem_grant);
    rc |= wasm3_link_raw(module, "wasmos", "shmem_map", "i(iii)", wasmos_shmem_map);
    rc |= wasm3_link_raw(module, "wasmos", "shmem_unmap", "i(i)", wasmos_shmem_unmap);
    rc |= wasm3_link_raw(module, "wasmos", "irq_route", "i(ii)", wasmos_irq_route);
    rc |= wasm3_link_raw(module, "wasmos", "irq_unroute", "i(i)", wasmos_irq_unroute);
    rc |= wasm3_link_raw(module, "wasmos", "serial_register", "i(i)", wasmos_serial_register);
    rc |= wasm3_link_raw(module, "wasmos", "input_push", "i(i)", wasmos_input_push);
    rc |= wasm3_link_raw(module, "wasmos", "input_read", "i()", wasmos_input_read);
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
