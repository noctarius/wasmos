#include "process_manager.h"
#include "ipc.h"
#include "serial.h"
#include "wasmos_app.h"
#include "native_driver.h"
#include "wasm_chardev.h"
#include "physmem.h"

/*
 * The process manager is the bridge between filesystem-visible WASMOS-APP
 * artifacts and runnable kernel processes. It validates container metadata,
 * allocates runtime state, resolves endpoint dependencies, and drives entry
 * execution according to the app/service/driver role encoded in the container.
 */

#define PM_MAX_MANAGED_APPS 16u
#define PM_MAX_WAITERS 8u
#define PM_FS_BUFFER_SIZE (256u * 1024u)

typedef struct {
    uint8_t in_use;
    uint32_t pid;
    uint32_t flags;
    const uint8_t *blob;
    uint32_t blob_size;
    /* FS-backed spawns are copied into PM-owned storage so the originating
     * filesystem buffer can be reused immediately after the spawn request. */
    uint8_t blob_storage[PM_FS_BUFFER_SIZE];
    uint8_t started;
    uint32_t entry_argc;
    uint32_t entry_arg0;
    uint32_t entry_arg1;
    uint32_t entry_arg2;
    uint32_t entry_arg3;
    wasmos_app_instance_t app;
    char name[64];
} pm_app_state_t;

typedef struct {
    uint8_t in_use;
    uint32_t pid;
    uint32_t reply_endpoint;
    uint32_t request_id;
} pm_wait_state_t;

typedef struct {
    uint8_t in_use;
    uint32_t reply_endpoint;
    uint32_t request_id;
    uint32_t parent_pid;
    uint32_t fs_request_id;
    char name[32];
} pm_spawn_state_t;

typedef struct {
    uint8_t in_use;
    uint32_t context_id;
    uint64_t buffer_phys;
} pm_fs_buffer_slot_t;

typedef struct {
    const boot_info_t *boot_info;
    uint32_t proc_endpoint;
    uint32_t fs_endpoint;
    uint32_t block_endpoint;
    uint32_t kbd_endpoint;
    uint32_t fb_endpoint;
    uint32_t vt_endpoint;
    uint32_t fs_reply_endpoint;
    uint32_t fs_request_id;
    uint8_t started;
    uint32_t init_module_index;
    uint32_t module_count;
    pm_app_state_t apps[PM_MAX_MANAGED_APPS];
    pm_wait_state_t waits[PM_MAX_WAITERS];
    pm_spawn_state_t spawn;
} pm_state_t;

static pm_state_t g_pm;
static pm_fs_buffer_slot_t g_pm_fs_slots[PROCESS_MAX_COUNT];

static pm_fs_buffer_slot_t *
pm_fs_slot_for_context(uint32_t context_id)
{
    pm_fs_buffer_slot_t *empty = 0;
    const uint64_t page_size = 4096u;
    const uint64_t pages = PM_FS_BUFFER_SIZE / page_size;

    if (context_id == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_pm_fs_slots[i].in_use && g_pm_fs_slots[i].context_id == context_id) {
            return &g_pm_fs_slots[i];
        }
        if (!empty && !g_pm_fs_slots[i].in_use) {
            empty = &g_pm_fs_slots[i];
        }
    }

    if (!empty) {
        return 0;
    }
    empty->in_use = 1;
    empty->context_id = context_id;
    empty->buffer_phys = pfa_alloc_pages(pages);
    if (empty->buffer_phys == 0) {
        empty->in_use = 0;
        empty->context_id = 0;
        return 0;
    }
    return empty;
}

void *
process_manager_fs_buffer_for_context(uint32_t context_id)
{
    pm_fs_buffer_slot_t *slot = pm_fs_slot_for_context(context_id);
    if (!slot) {
        return 0;
    }
    return (void *)(uintptr_t)slot->buffer_phys;
}

uint32_t
process_manager_fs_buffer_size(void)
{
    return PM_FS_BUFFER_SIZE;
}


static int
copy_name(char *dst, uint32_t dst_len, const uint8_t *src, uint32_t src_len)
{
    if (!dst || !src || dst_len == 0 || src_len == 0 || src_len >= dst_len) {
        return -1;
    }
    for (uint32_t i = 0; i < src_len; ++i) {
        dst[i] = (char)src[i];
    }
    dst[src_len] = '\0';
    return 0;
}

static int
name_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}


static void
unpack_name_args(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, char *out, uint32_t out_len)
{
    uint32_t args[4] = { arg0, arg1, arg2, arg3 };
    uint32_t pos = 0;
    if (!out || out_len == 0) {
        return;
    }
    for (uint32_t i = 0; i < 4 && pos + 1 < out_len; ++i) {
        uint32_t v = args[i];
        for (uint32_t b = 0; b < 4 && pos + 1 < out_len; ++b) {
            char c = (char)(v & 0xFF);
            if (c == '\0') {
                out[pos] = '\0';
                return;
            }
            out[pos++] = c;
            v >>= 8;
        }
    }
    out[pos] = '\0';
}

static void
pack_name_args(const char *name, uint32_t out[4])
{
    if (!out) {
        return;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        out[i] = 0;
    }
    if (!name) {
        return;
    }
    uint32_t idx = 0;
    for (uint32_t i = 0; name[i] && idx < 16; ++i, ++idx) {
        uint32_t slot = idx / 4;
        uint32_t shift = (idx % 4) * 8;
        out[slot] |= ((uint32_t)(uint8_t)name[i]) << shift;
    }
}

static const boot_module_t *
pm_module_at(uint32_t index)
{
    const boot_info_t *info = g_pm.boot_info;
    if (!info || !(info->flags & BOOT_INFO_FLAG_MODULES_PRESENT)) {
        return 0;
    }
    if (!info->modules || info->module_entry_size < sizeof(boot_module_t)) {
        return 0;
    }
    if (index >= info->module_count) {
        return 0;
    }
    const uint8_t *mods = (const uint8_t *)info->modules;
    return (const boot_module_t *)(mods + index * info->module_entry_size);
}

static pm_app_state_t *
pm_find_app_slot(void)
{
    for (uint32_t i = 0; i < PM_MAX_MANAGED_APPS; ++i) {
        if (!g_pm.apps[i].in_use) {
            return &g_pm.apps[i];
        }
    }
    return 0;
}

static uint32_t
pm_find_module_index_by_name(const char *name)
{
    /* Boot modules are still a first-class lookup source for the early storage
     * bootstrap chain even though late startup now prefers FAT-backed loading. */
    const boot_info_t *info = g_pm.boot_info;
    if (!info || !name || !(info->flags & BOOT_INFO_FLAG_MODULES_PRESENT)) {
        return 0xFFFFFFFFu;
    }

    for (uint32_t i = 0; i < info->module_count; ++i) {
        const boot_module_t *mod = pm_module_at(i);
        if (!mod || mod->type != BOOT_MODULE_TYPE_WASMOS_APP ||
            mod->base == 0 || mod->size == 0 || mod->size > 0xFFFFFFFFULL) {
            continue;
        }
        wasmos_app_desc_t desc;
        if (wasmos_app_parse((const uint8_t *)(uintptr_t)mod->base, (uint32_t)mod->size, &desc) != 0) {
            continue;
        }
        char temp[64];
        if (copy_name(temp, sizeof(temp), desc.name, desc.name_len) != 0) {
            continue;
        }
        if (name_eq(temp, name)) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

static process_run_result_t
pm_app_entry(process_t *process, void *arg)
{
    pm_app_state_t *state = (pm_app_state_t *)arg;

    if (!process || !state) {
        return PROCESS_RUN_IDLE;
    }

#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
    preempt_disable();
#endif

    if (!state->started) {
        /* The first run performs the expensive path once: parse metadata, wire
         * endpoints, instantiate the runtime, and invoke the declared entry. */
        wasmos_app_desc_t desc;
        if (wasmos_app_parse(state->blob, state->blob_size, &desc) != 0) {
            serial_write("[pm] app parse failed\n");
            process_set_exit_status(process, -1);
#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
            preempt_enable();
#endif
            return PROCESS_RUN_EXITED;
        }
        state->flags = desc.flags;
        uint32_t init_args[4] = {
            state->entry_arg0,
            state->entry_arg1,
            state->entry_arg2,
            state->entry_arg3
        };
        trace_write_unlocked("[pm] app flags=");
        trace_do(serial_write_hex64((uint64_t)desc.flags));
        if (desc.flags & WASMOS_APP_FLAG_NATIVE) {
            trace_write_unlocked("[pm] app type=native-driver\n");
        } else if (desc.flags & WASMOS_APP_FLAG_DRIVER) {
            trace_write_unlocked("[pm] app type=driver\n");
        } else if (desc.flags & WASMOS_APP_FLAG_SERVICE) {
            trace_write_unlocked("[pm] app type=service\n");
        } else if (desc.flags & WASMOS_APP_FLAG_APP) {
            trace_write_unlocked("[pm] app type=app\n");
        }
        trace_write_unlocked("[pm] app start ");
        trace_write_unlocked(state->name);
        trace_write_unlocked("\n");

        if (desc.flags & WASMOS_APP_FLAG_NATIVE) {
            /* Native ELF driver: load segments and call initialize() in one
             * shot.  The wasm instance is never created, so wasmos_app_stop
             * on the (inactive) app slot below is a safe no-op. */
            int native_rc = native_driver_start(process->context_id,
                                                desc.wasm_bytes,
                                                desc.wasm_size,
                                                state->name,
                                                init_args,
                                                state->entry_argc);
            process_set_exit_status(process, native_rc == 0 ? 0 : -1);
            wasmos_app_stop(&state->app);
            state->in_use = 0;
#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
            preempt_enable();
#endif
            return PROCESS_RUN_EXITED;
        }

        if (wasmos_app_start(&state->app,
                             &desc,
                             process->context_id,
                             init_args,
                             state->entry_argc) != 0) {
            serial_write("[pm] app start failed\n");
            process_set_exit_status(process, -1);
#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
            preempt_enable();
#endif
            return PROCESS_RUN_EXITED;
        }
        state->started = 1;
    }

    trace_write_unlocked("[pm] entry start ");
    trace_write_unlocked(state->name);
    trace_write_unlocked("\n");
    trace_write_unlocked("[pm] entry args count=");
    trace_do(serial_write_hex64((uint64_t)(uint32_t)state->entry_argc));
    trace_write_unlocked("[pm] entry args a0=");
    trace_do(serial_write_hex64((uint64_t)(uint32_t)state->entry_arg0));
    trace_write_unlocked("[pm] entry args a1=");
    trace_do(serial_write_hex64((uint64_t)(uint32_t)state->entry_arg1));
    trace_write_unlocked("[pm] entry args a2=");
    trace_do(serial_write_hex64((uint64_t)(uint32_t)state->entry_arg2));
    trace_write_unlocked("[pm] entry args a3=");
    trace_do(serial_write_hex64((uint64_t)(uint32_t)state->entry_arg3));

    {
        volatile uint64_t stack_canary_a = 0xA5A5A5A5DEADBEEFULL;
        volatile uint64_t stack_canary_b = 0x5A5A5A5AF00DFACEULL;
        uint64_t rsp = 0;
        uint64_t rip = 0;
        __asm__ volatile("mov %%rsp, %0\n" : "=r"(rsp));
        __asm__ volatile("leaq 1f(%%rip), %0\n1:" : "=r"(rip));
        trace_write_unlocked("[pm] entry rsp=");
        trace_do(serial_write_hex64(rsp));
        trace_write_unlocked("[pm] entry rip=");
        trace_do(serial_write_hex64(rip));
        trace_write_unlocked("[pm] entry call begin\n");
        int entry_rc = wasmos_app_call_entry(&state->app);
        trace_write_unlocked("[pm] entry call rc=");
        trace_do(serial_write_hex64((uint64_t)(uint32_t)entry_rc));
        if (entry_rc != 0) {
            serial_write("[pm] app entry failed\n");
            process_set_exit_status(process, -1);
        } else {
            process_set_exit_status(process, 0);
        }
        trace_write_unlocked("[pm] entry returned ");
        trace_write_unlocked(state->name);
        trace_write_unlocked(" flags=");
        trace_do(serial_write_hex64((uint64_t)state->flags));
        if (state->flags & (WASMOS_APP_FLAG_DRIVER | WASMOS_APP_FLAG_SERVICE)) {
            trace_write_unlocked("[pm] service/driver returned\n");
        } else {
            trace_write_unlocked("[pm] app returned\n");
        }
        if (stack_canary_a != 0xA5A5A5A5DEADBEEFULL || stack_canary_b != 0x5A5A5A5AF00DFACEULL) {
            serial_write("[pm] stack canary corrupted around entry\n");
            trace_write_unlocked("[pm] canary a=");
            trace_do(serial_write_hex64(stack_canary_a));
            trace_write_unlocked("[pm] canary b=");
            trace_do(serial_write_hex64(stack_canary_b));
            for (;;) {
                __asm__ volatile("hlt");
            }
        }
        uint64_t rsp_after = 0;
        uint64_t rip_after = 0;
        __asm__ volatile("mov %%rsp, %0\n" : "=r"(rsp_after));
        __asm__ volatile("leaq 1f(%%rip), %0\n1:" : "=r"(rip_after));
        trace_write_unlocked("[pm] entry done rsp=");
        trace_do(serial_write_hex64(rsp_after));
        trace_write_unlocked("[pm] entry done rip=");
        trace_do(serial_write_hex64(rip_after));
    }

    wasmos_app_stop(&state->app);
    state->in_use = 0;
    state->pid = 0;
#if defined(WASMOS_ENABLE_PREEMPT_GUARD)
    preempt_enable();
#endif
    return PROCESS_RUN_EXITED;
}

static int
pm_spawn_module(uint32_t parent_pid, uint32_t module_index, uint32_t *out_pid)
{
    const boot_module_t *mod = pm_module_at(module_index);
    if (!mod || mod->type != BOOT_MODULE_TYPE_WASMOS_APP || mod->base == 0 ||
        mod->size == 0 || mod->size > 0xFFFFFFFFULL) {
        return -1;
    }

    pm_app_state_t *slot = pm_find_app_slot();
    if (!slot) {
        return -1;
    }

    wasmos_app_desc_t desc;
    if (wasmos_app_parse((const uint8_t *)(uintptr_t)mod->base, (uint32_t)mod->size, &desc) != 0) {
        return -1;
    }

    if (copy_name(slot->name, sizeof(slot->name), desc.name, desc.name_len) != 0) {
        return -1;
    }
    trace_write("[pm] spawn module ");
    trace_write(slot->name);
    trace_write("\n");

    slot->blob = (const uint8_t *)(uintptr_t)mod->base;
    slot->blob_size = (uint32_t)mod->size;
    slot->started = 0;
    slot->entry_argc = 4;
    slot->entry_arg0 = 0;
    slot->entry_arg1 = 0;
    slot->entry_arg2 = 0;
    slot->entry_arg3 = 0;
    slot->in_use = 1;

    if (name_eq(slot->name, "sysinit")) {
        slot->entry_arg0 = g_pm.proc_endpoint;
        slot->entry_arg1 = g_pm.module_count;
        slot->entry_arg2 = g_pm.init_module_index;
    } else if (name_eq(slot->name, "chardev-client")) {
        uint32_t chardev_endpoint = IPC_ENDPOINT_NONE;
        if (wasm_chardev_endpoint(&chardev_endpoint) != 0) {
            slot->in_use = 0;
            return -1;
        }
        slot->entry_arg0 = chardev_endpoint;
    } else if (name_eq(slot->name, "cli")) {
        uint32_t fs_endpoint = g_pm.fs_endpoint;
        if (fs_endpoint == IPC_ENDPOINT_NONE) {
            slot->in_use = 0;
            return -1;
        }
        slot->entry_arg0 = g_pm.proc_endpoint;
        slot->entry_arg1 = fs_endpoint;
        slot->entry_arg2 = (g_pm.vt_endpoint != IPC_ENDPOINT_NONE)
                               ? g_pm.vt_endpoint
                               : (uint32_t)-1;
    } else if (name_eq(slot->name, "fs-fat")) {
        uint32_t block_endpoint = g_pm.block_endpoint;
        if (block_endpoint == IPC_ENDPOINT_NONE) {
            slot->in_use = 0;
            return -1;
        }
        slot->entry_arg0 = block_endpoint;
        slot->entry_arg1 = IPC_ENDPOINT_NONE;
    } else if (name_eq(slot->name, "hw-discovery")) {
        if (g_pm.proc_endpoint == IPC_ENDPOINT_NONE) {
            slot->in_use = 0;
            return -1;
        }
        slot->entry_arg0 = g_pm.proc_endpoint;
        slot->entry_arg1 = g_pm.module_count;
    } else if (name_eq(slot->name, "ata")) {
        slot->entry_arg0 = IPC_ENDPOINT_NONE;
    } else if (name_eq(slot->name, "keyboard")) {
        slot->entry_arg1 = IPC_ENDPOINT_NONE;
    } else if (name_eq(slot->name, "vt")) {
        slot->entry_arg0 = (g_pm.fb_endpoint != IPC_ENDPOINT_NONE)
                               ? g_pm.fb_endpoint
                               : (uint32_t)-1;
        slot->entry_arg1 = g_pm.kbd_endpoint;
        slot->entry_arg2 = (g_pm.vt_endpoint != IPC_ENDPOINT_NONE)
                               ? g_pm.vt_endpoint
                               : (uint32_t)-1;
    }

    preempt_disable();
    if (process_spawn_as(parent_pid, slot->name, pm_app_entry, slot, out_pid) != 0) {
        preempt_enable();
        slot->in_use = 0;
        return -1;
    }

    slot->pid = *out_pid;
    trace_write("[pm] spawn pid ");
    trace_do(serial_write_hex64(*out_pid));
    if (name_eq(slot->name, "ata") && g_pm.block_endpoint == IPC_ENDPOINT_NONE) {
        process_t *proc = process_get(*out_pid);
        if (!proc || ipc_endpoint_create(proc->context_id, &g_pm.block_endpoint) != IPC_OK) {
            preempt_enable();
            slot->in_use = 0;
            return -1;
        }
        slot->entry_arg0 = g_pm.block_endpoint;
    }
    if (name_eq(slot->name, "keyboard") && g_pm.kbd_endpoint == IPC_ENDPOINT_NONE) {
        process_t *proc = process_get(*out_pid);
        if (!proc || ipc_endpoint_create(proc->context_id, &g_pm.kbd_endpoint) != IPC_OK) {
            preempt_enable();
            slot->in_use = 0;
            return -1;
        }
        slot->entry_arg1 = g_pm.kbd_endpoint;
    }
    if (name_eq(slot->name, "vt") && g_pm.vt_endpoint == IPC_ENDPOINT_NONE) {
        process_t *proc = process_get(*out_pid);
        if (!proc || ipc_endpoint_create(proc->context_id, &g_pm.vt_endpoint) != IPC_OK) {
            preempt_enable();
            slot->in_use = 0;
            return -1;
        }
        slot->entry_arg2 = g_pm.vt_endpoint;
    }
    if (name_eq(slot->name, "cli")) {
        slot->entry_arg2 = (g_pm.vt_endpoint != IPC_ENDPOINT_NONE)
                               ? g_pm.vt_endpoint
                               : (uint32_t)-1;
    }
    if (name_eq(slot->name, "fs-fat") && g_pm.fs_endpoint == IPC_ENDPOINT_NONE) {
        process_t *proc = process_get(*out_pid);
        if (!proc || ipc_endpoint_create(proc->context_id, &g_pm.fs_endpoint) != IPC_OK) {
            preempt_enable();
            slot->in_use = 0;
            return -1;
        }
        slot->entry_arg1 = g_pm.fs_endpoint;
    }
    preempt_enable();
    return 0;
}

static int
pm_spawn_from_buffer(uint32_t parent_pid, const uint8_t *blob, uint32_t blob_size, uint32_t *out_pid)
{
    if (!blob || blob_size == 0 || !out_pid) {
        return -1;
    }
    pm_app_state_t *slot = pm_find_app_slot();
    if (!slot) {
        return -1;
    }
    if (blob_size > sizeof(slot->blob_storage)) {
        return -1;
    }

    for (uint32_t i = 0; i < blob_size; ++i) {
        slot->blob_storage[i] = blob[i];
    }

    wasmos_app_desc_t desc;
    if (wasmos_app_parse(slot->blob_storage, blob_size, &desc) != 0) {
        return -1;
    }
    if ((desc.flags & WASMOS_APP_FLAG_DRIVER) != 0 ||
        (desc.flags & (WASMOS_APP_FLAG_APP | WASMOS_APP_FLAG_SERVICE)) == 0) {
        return -1;
    }
    if (copy_name(slot->name, sizeof(slot->name), desc.name, desc.name_len) != 0) {
        return -1;
    }

    slot->blob = slot->blob_storage;
    slot->blob_size = blob_size;
    slot->started = 0;
    slot->entry_argc = 4;
    slot->entry_arg0 = 0;
    slot->entry_arg1 = 0;
    slot->entry_arg2 = 0;
    slot->entry_arg3 = 0;
    slot->in_use = 1;

    if (name_eq(slot->name, "chardev-client") || name_eq(slot->name, "chardev-preempt")) {
        uint32_t chardev_endpoint = IPC_ENDPOINT_NONE;
        if (wasm_chardev_endpoint(&chardev_endpoint) != 0) {
            slot->in_use = 0;
            return -1;
        }
        slot->entry_argc = 4;
        slot->entry_arg0 = chardev_endpoint;
    } else if (name_eq(slot->name, "sysinit")) {
        if (g_pm.proc_endpoint == IPC_ENDPOINT_NONE) {
            slot->in_use = 0;
            return -1;
        }
        slot->entry_arg0 = g_pm.proc_endpoint;
    } else if (name_eq(slot->name, "cli")) {
        if (g_pm.proc_endpoint == IPC_ENDPOINT_NONE || g_pm.fs_endpoint == IPC_ENDPOINT_NONE) {
            slot->in_use = 0;
            return -1;
        }
        slot->entry_arg0 = g_pm.proc_endpoint;
        slot->entry_arg1 = g_pm.fs_endpoint;
        slot->entry_arg2 = (g_pm.vt_endpoint != IPC_ENDPOINT_NONE)
                               ? g_pm.vt_endpoint
                               : (uint32_t)-1;
    } else if (name_eq(slot->name, "vt")) {
        slot->entry_arg0 = (g_pm.fb_endpoint != IPC_ENDPOINT_NONE)
                               ? g_pm.fb_endpoint
                               : (uint32_t)-1;
        slot->entry_arg1 = g_pm.kbd_endpoint;
        slot->entry_arg2 = (g_pm.vt_endpoint != IPC_ENDPOINT_NONE)
                               ? g_pm.vt_endpoint
                               : (uint32_t)-1;
    }

    if (process_spawn_as(parent_pid, slot->name, pm_app_entry, slot, out_pid) != 0) {
        slot->in_use = 0;
        return -1;
    }
    slot->pid = *out_pid;
    if (name_eq(slot->name, "vt") && g_pm.vt_endpoint == IPC_ENDPOINT_NONE) {
        process_t *proc = process_get(*out_pid);
        if (!proc || ipc_endpoint_create(proc->context_id, &g_pm.vt_endpoint) != IPC_OK) {
            slot->in_use = 0;
            return -1;
        }
        slot->entry_arg2 = g_pm.vt_endpoint;
    }
    return 0;
}

static int
pm_send_fs_read(uint32_t pm_context_id, const char *name, uint32_t *out_req_id)
{
    if (!name || !out_req_id || g_pm.fs_endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }
    uint32_t args[4];
    pack_name_args(name, args);

    ipc_message_t req;
    req.type = FS_IPC_READ_APP_REQ;
    req.source = g_pm.fs_reply_endpoint;
    req.destination = g_pm.fs_endpoint;
    req.request_id = g_pm.fs_request_id++;
    req.arg0 = args[0];
    req.arg1 = args[1];
    req.arg2 = args[2];
    req.arg3 = args[3];
    if (ipc_send_from(pm_context_id, g_pm.fs_endpoint, &req) != IPC_OK) {
        return -1;
    }
    *out_req_id = req.request_id;
    return 0;
}

static void
pm_poll_spawn(uint32_t pm_context_id)
{
    if (!g_pm.spawn.in_use || g_pm.fs_reply_endpoint == IPC_ENDPOINT_NONE) {
        return;
    }

    ipc_message_t msg;
    int recv_rc = ipc_recv_for(pm_context_id, g_pm.fs_reply_endpoint, &msg);
    if (recv_rc == IPC_EMPTY) {
        return;
    }
    g_pm.spawn.in_use = 0;
    if (recv_rc != IPC_OK ||
        msg.request_id != g_pm.spawn.fs_request_id ||
        msg.type != FS_IPC_RESP) {
        ipc_message_t resp;
        resp.type = PROC_IPC_ERROR;
        resp.source = g_pm.proc_endpoint;
        resp.destination = g_pm.spawn.reply_endpoint;
        resp.request_id = g_pm.spawn.request_id;
        resp.arg0 = PROC_IPC_SPAWN_NAME;
        resp.arg1 = 0;
        resp.arg2 = 0;
        resp.arg3 = 0;
        ipc_send_from(pm_context_id, g_pm.spawn.reply_endpoint, &resp);
        return;
    }

    uint32_t pid = 0;
    uint32_t size = (uint32_t)msg.arg0;
    const uint8_t *fs_blob = (const uint8_t *)process_manager_fs_buffer_for_context(pm_context_id);
    if (size == 0 || size > process_manager_fs_buffer_size() || !fs_blob ||
        pm_spawn_from_buffer(g_pm.spawn.parent_pid,
                             fs_blob,
                             size,
                             &pid) != 0) {
        ipc_message_t resp;
        resp.type = PROC_IPC_ERROR;
        resp.source = g_pm.proc_endpoint;
        resp.destination = g_pm.spawn.reply_endpoint;
        resp.request_id = g_pm.spawn.request_id;
        resp.arg0 = PROC_IPC_SPAWN_NAME;
        resp.arg1 = 0;
        resp.arg2 = 0;
        resp.arg3 = 0;
        ipc_send_from(pm_context_id, g_pm.spawn.reply_endpoint, &resp);
        return;
    }

    ipc_message_t resp;
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = g_pm.spawn.reply_endpoint;
    resp.request_id = g_pm.spawn.request_id;
    resp.arg0 = pid;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    ipc_send_from(pm_context_id, g_pm.spawn.reply_endpoint, &resp);
}

static void
pm_check_waits(uint32_t pm_context_id)
{
    for (uint32_t i = 0; i < PM_MAX_WAITERS; ++i) {
        pm_wait_state_t *waiter = &g_pm.waits[i];
        if (!waiter->in_use) {
            continue;
        }
        int32_t exit_status = 0;
        int rc = process_get_exit_status(waiter->pid, &exit_status);
        if (rc != 0) {
            continue;
        }

        ipc_message_t resp;
        resp.type = PROC_IPC_RESP;
        resp.source = g_pm.proc_endpoint;
        resp.destination = waiter->reply_endpoint;
        resp.request_id = waiter->request_id;
        resp.arg0 = waiter->pid;
        resp.arg1 = (uint32_t)exit_status;
        resp.arg2 = 0;
        resp.arg3 = 0;
        ipc_send_from(pm_context_id, waiter->reply_endpoint, &resp);
        waiter->in_use = 0;
    }
}

static void
pm_reap_apps(process_t *owner)
{
    if (!owner) {
        return;
    }
    for (uint32_t i = 0; i < PM_MAX_MANAGED_APPS; ++i) {
        pm_app_state_t *app = &g_pm.apps[i];
        if (!app->in_use || app->pid == 0) {
            continue;
        }
        int32_t exit_status = 0;
        if (process_get_exit_status(app->pid, &exit_status) != 0) {
            continue;
        }
        if (process_wait(owner, app->pid, &exit_status) != 0) {
            continue;
        }
        wasmos_app_stop(&app->app);
        app->in_use = 0;
        app->pid = 0;
    }
}

static int
pm_handle_spawn(uint32_t pm_context_id, const ipc_message_t *msg)
{
    uint32_t owner_context = 0;
    process_t *caller = 0;
    uint32_t parent_pid = 0;
    uint32_t pid = 0;
    trace_write("[pm] spawn index=");
    trace_do(serial_write_hex64(msg->arg0));
    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return -1;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return -1;
    }
    parent_pid = caller->pid;

    if (pm_spawn_module(parent_pid, msg->arg0, &pid) != 0) {
        return -1;
    }

    ipc_message_t resp;
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = pid;
    resp.arg1 = 0;
    resp.arg2 = 0;
    resp.arg3 = 0;
    int rc = ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
    trace_write_unlocked("[pm] spawn resp rc=");
    trace_do(serial_write_hex64((uint64_t)(uint32_t)rc));
    return rc;
}

static int
pm_handle_spawn_name(uint32_t pm_context_id, const ipc_message_t *msg)
{
    char name[32];
    uint32_t owner_context = 0;
    process_t *caller = 0;
    uint32_t parent_pid = 0;

    unpack_name_args((uint32_t)msg->arg0,
                     (uint32_t)msg->arg1,
                     (uint32_t)msg->arg2,
                     (uint32_t)msg->arg3,
                     name,
                     sizeof(name));
    if (name[0] == '\0') {
        return -1;
    }
    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return -1;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return -1;
    }
    parent_pid = caller->pid;

    if (g_pm.fs_endpoint == IPC_ENDPOINT_NONE || g_pm.fs_reply_endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }
    if (g_pm.spawn.in_use) {
        return -1;
    }
    uint32_t fs_req_id = 0;
    if (pm_send_fs_read(pm_context_id, name, &fs_req_id) != 0) {
        return -1;
    }

    g_pm.spawn.in_use = 1;
    g_pm.spawn.reply_endpoint = msg->source;
    g_pm.spawn.request_id = msg->request_id;
    g_pm.spawn.parent_pid = parent_pid;
    g_pm.spawn.fs_request_id = fs_req_id;
    for (uint32_t i = 0; i < sizeof(g_pm.spawn.name); ++i) {
        g_pm.spawn.name[i] = name[i];
        if (!name[i]) {
            break;
        }
    }
    return 0;
}

static int
pm_handle_kill(uint32_t pm_context_id, const ipc_message_t *msg)
{
    uint32_t owner_context = 0;
    process_t *caller = 0;
    process_t *target = 0;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return -1;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return -1;
    }

    target = process_get(msg->arg0);
    if (!target || target->parent_pid != caller->pid) {
        return -1;
    }

    if (process_kill(msg->arg0, (int32_t)msg->arg1) != 0) {
        return -1;
    }

    ipc_message_t resp;
    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = msg->arg0;
    resp.arg1 = msg->arg1;
    resp.arg2 = 0;
    resp.arg3 = 0;
    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
}

static int
pm_handle_status(uint32_t pm_context_id, const ipc_message_t *msg)
{
    process_t *target = process_get(msg->arg0);
    ipc_message_t resp;

    resp.type = PROC_IPC_RESP;
    resp.source = g_pm.proc_endpoint;
    resp.destination = msg->source;
    resp.request_id = msg->request_id;
    resp.arg0 = msg->arg0;
    resp.arg1 = PROC_STATUS_UNKNOWN;
    resp.arg2 = 0;
    resp.arg3 = 0;

    if (target) {
        if (target->state == PROCESS_STATE_ZOMBIE) {
            resp.arg1 = PROC_STATUS_ZOMBIE;
            resp.arg2 = (uint32_t)target->exit_status;
        } else if (target->state != PROCESS_STATE_UNUSED) {
            resp.arg1 = PROC_STATUS_RUNNING;
        }
    }

    return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
}

static int
pm_handle_wait(uint32_t pm_context_id, const ipc_message_t *msg)
{
    uint32_t owner_context = 0;
    process_t *caller = 0;
    process_t *target = 0;
    int32_t exit_status = 0;

    if (ipc_endpoint_owner(msg->source, &owner_context) != IPC_OK) {
        return -1;
    }
    caller = process_find_by_context(owner_context);
    if (!caller) {
        return -1;
    }

    target = process_get(msg->arg0);
    if (!target || target->parent_pid != caller->pid) {
        return -1;
    }

    if (process_get_exit_status(msg->arg0, &exit_status) == 0) {
        ipc_message_t resp;
        resp.type = PROC_IPC_RESP;
        resp.source = g_pm.proc_endpoint;
        resp.destination = msg->source;
        resp.request_id = msg->request_id;
        resp.arg0 = msg->arg0;
        resp.arg1 = (uint32_t)exit_status;
        resp.arg2 = 0;
        resp.arg3 = 0;
        return ipc_send_from(pm_context_id, msg->source, &resp) == IPC_OK ? 0 : -1;
    }

    for (uint32_t i = 0; i < PM_MAX_WAITERS; ++i) {
        pm_wait_state_t *waiter = &g_pm.waits[i];
        if (waiter->in_use) {
            continue;
        }
        waiter->in_use = 1;
        waiter->pid = msg->arg0;
        waiter->reply_endpoint = msg->source;
        waiter->request_id = msg->request_id;
        return 0;
    }

    return -1;
}

int
process_manager_init(const boot_info_t *boot_info)
{
    g_pm.init_module_index = 0xFFFFFFFFu;
    g_pm.module_count = 0;
    g_pm.boot_info = boot_info;
    g_pm.proc_endpoint = IPC_ENDPOINT_NONE;
    g_pm.fs_endpoint = IPC_ENDPOINT_NONE;
    g_pm.block_endpoint = IPC_ENDPOINT_NONE;
    g_pm.kbd_endpoint = IPC_ENDPOINT_NONE;
    g_pm.fb_endpoint = IPC_ENDPOINT_NONE;
    g_pm.vt_endpoint = IPC_ENDPOINT_NONE;
    g_pm.fs_reply_endpoint = IPC_ENDPOINT_NONE;
    g_pm.fs_request_id = 1;
    g_pm.started = 0;
    if (boot_info && (boot_info->flags & BOOT_INFO_FLAG_MODULES_PRESENT)) {
        g_pm.module_count = boot_info->module_count;
        g_pm.init_module_index = pm_find_module_index_by_name("sysinit");
    }
    for (uint32_t i = 0; i < PM_MAX_MANAGED_APPS; ++i) {
        g_pm.apps[i].in_use = 0;
        g_pm.apps[i].pid = 0;
        g_pm.apps[i].blob = 0;
        g_pm.apps[i].blob_size = 0;
        g_pm.apps[i].started = 0;
        g_pm.apps[i].name[0] = '\0';
    }
    for (uint32_t i = 0; i < PM_MAX_WAITERS; ++i) {
        g_pm.waits[i].in_use = 0;
        g_pm.waits[i].pid = 0;
        g_pm.waits[i].reply_endpoint = IPC_ENDPOINT_NONE;
        g_pm.waits[i].request_id = 0;
    }
    g_pm.spawn.in_use = 0;
    return 0;
}

uint32_t
process_manager_endpoint(void)
{
    return g_pm.proc_endpoint;
}

uint32_t
process_manager_fs_endpoint(void)
{
    return g_pm.fs_endpoint;
}

uint32_t
process_manager_block_endpoint(void)
{
    return g_pm.block_endpoint;
}

uint32_t
process_manager_framebuffer_endpoint(void)
{
    return g_pm.fb_endpoint;
}

void
process_manager_set_framebuffer_endpoint(uint32_t endpoint)
{
    g_pm.fb_endpoint = endpoint;
}

process_run_result_t
process_manager_entry(process_t *process, void *arg)
{
    ipc_message_t msg;
    static uint8_t logged_pending = 0;

    (void)arg;

    if (!process) {
        return PROCESS_RUN_IDLE;
    }

    if (!g_pm.started) {
        if (ipc_endpoint_create(process->context_id, &g_pm.proc_endpoint) != IPC_OK) {
            serial_write("[pm] endpoint create failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        if (ipc_endpoint_create(process->context_id, &g_pm.fs_reply_endpoint) != IPC_OK) {
            serial_write("[pm] fs reply endpoint create failed\n");
            process_set_exit_status(process, -1);
            return PROCESS_RUN_EXITED;
        }
        g_pm.started = 1;
        trace_write("[pm] proc endpoint=");
        trace_do(serial_write_hex64(g_pm.proc_endpoint));
        trace_write("[pm] context id=");
        trace_do(serial_write_hex64(process->context_id));
    }

    pm_check_waits(process->context_id);
    pm_reap_apps(process);
    pm_poll_spawn(process->context_id);
    if (!logged_pending) {
        uint32_t pending = 0;
        if (ipc_endpoint_count(g_pm.proc_endpoint, &pending) == IPC_OK && pending > 0) {
            logged_pending = 1;
            trace_write_unlocked("[pm] pending queued\n");
        }
    }

    int recv_rc = ipc_recv_for(process->context_id, g_pm.proc_endpoint, &msg);
    if (recv_rc == IPC_EMPTY) {
        return PROCESS_RUN_YIELDED;
    }
    if (recv_rc != IPC_OK) {
        static uint8_t logged_recv_fail = 0;
        if (!logged_recv_fail) {
            logged_recv_fail = 1;
            uint32_t owner = 0;
            int has_owner = (ipc_endpoint_owner(g_pm.proc_endpoint, &owner) == IPC_OK);
            serial_printf("[pm] recv failed rc=%016llx\n", (unsigned long long)(uint32_t)recv_rc);
            if (has_owner) {
                serial_printf("[pm] proc owner=%016llx\n", (unsigned long long)owner);
            }
            serial_printf("[pm] ctx=%016llx\n", (unsigned long long)process->context_id);
        }
        return PROCESS_RUN_YIELDED;
    }
    trace_write_unlocked("[pm] recv type=");
    trace_do(serial_write_hex64_unlocked(msg.type));
    trace_write_unlocked("[pm] recv req=");
    trace_do(serial_write_hex64_unlocked(msg.request_id));

    int rc = -1;
    switch (msg.type) {
        case PROC_IPC_SPAWN:
            rc = pm_handle_spawn(process->context_id, &msg);
            break;
        case PROC_IPC_SPAWN_NAME:
            rc = pm_handle_spawn_name(process->context_id, &msg);
            break;
        case PROC_IPC_KILL:
            rc = pm_handle_kill(process->context_id, &msg);
            break;
        case PROC_IPC_STATUS:
            rc = pm_handle_status(process->context_id, &msg);
            break;
        case PROC_IPC_WAIT:
            rc = pm_handle_wait(process->context_id, &msg);
            break;
        default:
            rc = -1;
            break;
    }

    if (rc != 0) {
        ipc_message_t resp;
        resp.type = PROC_IPC_ERROR;
        resp.source = g_pm.proc_endpoint;
        resp.destination = msg.source;
        resp.request_id = msg.request_id;
        resp.arg0 = msg.type;
        resp.arg1 = 0;
        resp.arg2 = 0;
        resp.arg3 = 0;
        ipc_send_from(process->context_id, msg.source, &resp);
    }

    return PROCESS_RUN_YIELDED;
}
