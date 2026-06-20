#include "boot.h"
#include "arch/x86_64/smp.h"
#include "klog.h"
#include "ipc.h"
#include "io.h"
#include "physmem.h"
#include "process.h"
#include "process_manager.h"
#include "memory.h"
#include "paging.h"
#include "serial.h"
#include "timer.h"
#include "link.h"
#include "wasmos_app.h"
#include "wasmos_driver_abi.h"
#include "framebuffer.h"
#include "irq.h"
#include "policy.h"
#include "capability.h"
#include "system_control.h"
#include "thread.h"
#include "wasm_driver.h"

#ifdef WASMOS_SCHED_THREADABLE
#include "futex.h"
#endif

#include <stdint.h>
#include <string.h>

extern M3Result ResizeMemory(IM3Runtime io_runtime, uint32_t i_numPages);

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

typedef struct {
    uint32_t pid;
    uint32_t shmem_id;
    uint32_t offset;
    uint32_t size;
    uint8_t valid;
} wasm_shmem_linear_map_t;

static wasm_ipc_last_slot_t g_wasm_last_slots[PROCESS_MAX_COUNT];
static wasm_block_slot_t g_wasm_block_slots[PROCESS_MAX_COUNT];
static wasm_fs_peer_slot_t g_wasm_fs_peer_slots[PROCESS_MAX_COUNT];
/* Allow several SHMEM mappings per process (UI + multiple window buffers + aux buffers). */
#define WASM_SHMEM_MAP_SLOTS (PROCESS_MAX_COUNT * 32)
static wasm_shmem_linear_map_t g_wasm_shmem_maps[WASM_SHMEM_MAP_SLOTS];
static const boot_info_t *g_wasm_boot_info;

#define KENV_MAX_ENTRIES 64
#define KENV_KEY_MAX     33
#define KENV_VAL_MAX     129

typedef struct {
    uint8_t in_use;
    char    key[KENV_KEY_MAX];
    char    value[KENV_VAL_MAX];
} kenv_entry_t;

static kenv_entry_t g_kenv[KENV_MAX_ENTRIES];

static int
kenv_find(const char *key)
{
    for (int i = 0; i < KENV_MAX_ENTRIES; i++) {
        if (g_kenv[i].in_use && strcmp(g_kenv[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static int
wasm_arg_u32_nonneg(int32_t raw, uint32_t *out)
{
    if (!out || raw < 0) {
        return -1;
    }
    *out = (uint32_t)raw;
    return 0;
}

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
    /* TODO: Remove host-view sync once linear-memory ownership converges.
     * Ring3 migration note:
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
    /* TODO: Remove host-view reconciliation once linear-memory ownership
     * converges and user copy helpers are the single source of truth. */
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

static int
initfs_header_get(const wasmos_initfs_header_t **out_hdr, const uint8_t **out_base)
{
    const wasmos_initfs_header_t *hdr = 0;
    const uint8_t *base = 0;
    uint64_t entries_bytes = 0;
    uint64_t entries_end = 0;

    if (!out_hdr || !out_base ||
        !g_wasm_boot_info ||
        !(g_wasm_boot_info->flags & BOOT_INFO_FLAG_INITFS_PRESENT) ||
        !g_wasm_boot_info->initfs ||
        g_wasm_boot_info->initfs_size < sizeof(wasmos_initfs_header_t)) {
        return -1;
    }

    base = (const uint8_t *)g_wasm_boot_info->initfs;
    hdr = (const wasmos_initfs_header_t *)base;
    if (memcmp(hdr->magic, WASMOS_INITFS_MAGIC, sizeof(hdr->magic)) != 0 ||
        hdr->version != WASMOS_INITFS_VERSION ||
        hdr->header_size < sizeof(wasmos_initfs_header_t) ||
        hdr->entry_size != sizeof(wasmos_initfs_entry_t)) {
        return -1;
    }
    entries_bytes = (uint64_t)hdr->entry_count * (uint64_t)hdr->entry_size;
    entries_end = (uint64_t)hdr->header_size + entries_bytes;
    if (entries_end > (uint64_t)g_wasm_boot_info->initfs_size) {
        return -1;
    }
    *out_hdr = hdr;
    *out_base = base;
    return 0;
}

static int
initfs_entry_at(uint32_t index, wasmos_initfs_entry_t *out)
{
    const wasmos_initfs_header_t *hdr = 0;
    const uint8_t *base = 0;
    const uint8_t *entries_base = 0;
    uint64_t payload_end = 0;
    if (!out || initfs_header_get(&hdr, &base) != 0) {
        return -1;
    }
    if (index >= hdr->entry_count) {
        return -1;
    }

    entries_base = base + hdr->header_size;

    const wasmos_initfs_entry_t *entry =
        (const wasmos_initfs_entry_t *)(entries_base + ((uint64_t)index * hdr->entry_size));
    payload_end = (uint64_t)entry->offset + (uint64_t)entry->size;
    if (payload_end > (uint64_t)g_wasm_boot_info->initfs_size) {
        return -1;
    }
    *out = *entry;
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
    for (uint32_t i = 0; i < WASM_SHMEM_MAP_SLOTS; ++i) {
        g_wasm_shmem_maps[i].pid = 0;
        g_wasm_shmem_maps[i].shmem_id = 0;
        g_wasm_shmem_maps[i].offset = 0;
        g_wasm_shmem_maps[i].size = 0;
        g_wasm_shmem_maps[i].valid = 0;
    }
}

static void
wasm_shmem_map_track(uint32_t pid, uint32_t shmem_id, uint32_t offset, uint32_t size)
{
    wasm_shmem_linear_map_t *empty = 0;
    for (uint32_t i = 0; i < WASM_SHMEM_MAP_SLOTS; ++i) {
        wasm_shmem_linear_map_t *slot = &g_wasm_shmem_maps[i];
        if (slot->valid &&
            slot->pid == pid &&
            slot->shmem_id == shmem_id &&
            slot->offset == offset)
        {
            slot->size = size;
            return;
        }
        if (!empty && !slot->valid) {
            empty = slot;
        }
    }
    if (empty) {
        empty->pid = pid;
        empty->shmem_id = shmem_id;
        empty->offset = offset;
        empty->size = size;
        empty->valid = 1;
    }
}

static void
wasm_shmem_map_untrack(uint32_t pid, uint32_t shmem_id)
{
    for (uint32_t i = 0; i < WASM_SHMEM_MAP_SLOTS; ++i) {
        wasm_shmem_linear_map_t *slot = &g_wasm_shmem_maps[i];
        if (!slot->valid) continue;
        if (slot->pid == pid && slot->shmem_id == shmem_id) {
            slot->valid = 0;
        }
    }
}

static uint8_t
wasm_shmem_map_overlaps(uint32_t pid, uint32_t offset, uint32_t size)
{
    uint64_t a0 = (uint64_t)offset;
    uint64_t a1 = a0 + (uint64_t)size;
    for (uint32_t i = 0; i < WASM_SHMEM_MAP_SLOTS; ++i) {
        const wasm_shmem_linear_map_t *slot = &g_wasm_shmem_maps[i];
        if (!slot->valid || slot->pid != pid || slot->size == 0) {
            continue;
        }
        uint64_t b0 = (uint64_t)slot->offset;
        uint64_t b1 = b0 + (uint64_t)slot->size;
        if (a0 < b1 && b0 < a1) {
            return 1;
        }
    }
    return 0;
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

static void
wasm_linear_region_sync_size(mm_context_t *ctx, uint64_t required_size)
{
    if (!ctx || required_size == 0) {
        return;
    }
    list_iter_t it;
    mem_region_t *region = (mem_region_t *)list_first(&ctx->regions, &it);
    while (region) {
        if (region->type != MEM_REGION_WASM_LINEAR) {
            region = (mem_region_t *)list_next(&it);
            continue;
        }
        if (required_size > region->size) {
            region->size = required_size;
        }
        return;
    }
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

/* io.port, io.mmio and irq use allowlist-based policy: a process may have the
 * general capability but be denied a specific port/line.  Denial is an expected
 * normal outcome (probe-and-skip), not a security violation, so policy_authorize
 * is used (returns -1, caller decides how to handle). */
static int
require_io_capability(uint32_t context_id, uint16_t port)
{
    return policy_authorize(context_id, POLICY_ACTION_IO_PORT, port);
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

/* system.control is binary: any denial means the process must not be calling
 * this.  policy_require kills the process instead of returning a silent -1. */
static int
require_system_control_capability(uint32_t context_id)
{
    return policy_require(context_id, POLICY_ACTION_SYSTEM_CONTROL, 0);
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
    process_t *proc = process_get(pid);
    uint8_t is_fs_manager = (proc && proc->name && strcmp(proc->name, "fs-manager") == 0) ? 1u : 0u;
    if (is_fs_manager) {
        uint32_t borrowed_source = process_manager_buffer_borrow_source_context(PM_BUFFER_KIND_FILESYSTEM,
                                                                                 context_id);
        if (borrowed_source != 0) {
            target_context = borrowed_source;
        }
        /* fs-manager relays through explicit buffer borrows; peer-slot
         * redirection can point at backend replies and corrupt relay writes. */
        return process_manager_buffer_for_context(PM_BUFFER_KIND_FILESYSTEM, target_context);
    }
    if (peer &&
        peer->valid &&
        peer->peer_context_id != 0) {
        target_context = peer->peer_context_id;
    }
    return process_manager_buffer_for_context(PM_BUFFER_KIND_FILESYSTEM, target_context);
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


m3ApiRawFunction(wasmos_ipc_endpoint_owner)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, endpoint)
    uint32_t owner_context_id = 0;

    preempt_safepoint();
    if (endpoint < 0) {
        m3ApiReturn(-1);
    }
    if (ipc_endpoint_owner((uint32_t)endpoint, &owner_context_id) != IPC_OK ||
        owner_context_id == 0) {
        m3ApiReturn(-1);
    }
    preempt_safepoint();
    m3ApiReturn((int32_t)owner_context_id);
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

static int
process_name_eq(const char *a, const char *b)
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

static int32_t
wasm_buffer_borrow_impl(int32_t kind, int32_t source_endpoint, int32_t flags)
{
    uint32_t context_id = 0;
    uint32_t source_owner = 0;
    uint32_t pid = process_current_pid();
    process_t *proc = process_get(pid);

    if (kind != (int32_t)PM_BUFFER_KIND_FILESYSTEM ||
        flags <= 0 || (flags & ~0x3) != 0) {
        return IPC_ERR_INVALID;
    }
    if (current_process_context(&context_id) != 0) {
        return IPC_ERR_PERM;
    }
    /* FIXME: Replace mixed role/capability proxy permission with a dedicated
     * borrow capability once non-FS DMA users are fully profiled. */
    if (!proc ||
        (!process_name_eq(proc->name, "fs-manager") &&
         !capability_has(context_id, CAP_DMA_BUFFER))) {
        return IPC_ERR_PERM;
    }
    if (ipc_endpoint_owner((uint32_t)source_endpoint, &source_owner) != IPC_OK ||
        source_owner == 0 || source_owner == context_id) {
        return IPC_ERR_PERM;
    }
    return process_manager_buffer_borrow_context((uint32_t)kind, context_id, source_owner, (uint32_t)flags);
}

static int32_t
wasm_buffer_release_impl(int32_t kind)
{
    uint32_t context_id = 0;
    uint32_t pid = process_current_pid();
    process_t *proc = process_get(pid);

    if (kind != (int32_t)PM_BUFFER_KIND_FILESYSTEM) {
        return IPC_ERR_INVALID;
    }
    if (current_process_context(&context_id) != 0) {
        return IPC_ERR_PERM;
    }
    if (!proc ||
        (!process_name_eq(proc->name, "fs-manager") &&
         !capability_has(context_id, CAP_DMA_BUFFER))) {
        return IPC_ERR_PERM;
    }
    return process_manager_buffer_release_context((uint32_t)kind, context_id);
}

static int
dma_direction_borrow_allowed(uint32_t borrow_flags, uint32_t direction_flags)
{
    if (direction_flags == 0) {
        return 0;
    }
    if ((direction_flags & WASMOS_DMA_DIR_TO_DEVICE) != 0 &&
        (borrow_flags & PM_BUFFER_BORROW_READ) == 0) {
        return 0;
    }
    if ((direction_flags & WASMOS_DMA_DIR_FROM_DEVICE) != 0 &&
        (borrow_flags & PM_BUFFER_BORROW_WRITE) == 0) {
        return 0;
    }
    return 1;
}

m3ApiRawFunction(wasmos_dma_map_borrow)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, kind)
    m3ApiGetArg(int32_t, source_endpoint)
    m3ApiGetArg(int32_t, offset)
    m3ApiGetArg(int32_t, length)
    m3ApiGetArg(int32_t, direction_flags)
    uint32_t context_id = 0;
    uint32_t source_owner = 0;
    uint32_t borrow_flags = 0;
    uint32_t max_bytes = 0;
    uint64_t device_addr = 0;

    if (kind < 0 || offset < 0 ||
        length <= 0 || direction_flags <= 0) {
        m3ApiReturn(WASMOS_DMA_STATUS_INVALID);
    }
    if (current_process_context(&context_id) != 0 ||
        require_dma_capability(context_id) != 0) {
        m3ApiReturn(WASMOS_DMA_STATUS_DENY);
    }
    if (ipc_endpoint_owner((uint32_t)source_endpoint, &source_owner) != IPC_OK ||
        source_owner == 0 || source_owner == context_id) {
        m3ApiReturn(WASMOS_DMA_STATUS_DENY);
    }
    if (process_manager_buffer_borrow_source_context((uint32_t)kind, context_id) != source_owner) {
        m3ApiReturn(WASMOS_DMA_STATUS_DENY);
    }
    borrow_flags = process_manager_buffer_borrow_flags((uint32_t)kind, context_id);
    if (!dma_direction_borrow_allowed(borrow_flags, (uint32_t)direction_flags)) {
        m3ApiReturn(WASMOS_DMA_STATUS_DENY);
    }
    if (!capability_dma_direction_allowed(context_id, (uint32_t)direction_flags)) {
        m3ApiReturn(WASMOS_DMA_STATUS_DENY);
    }
    max_bytes = capability_dma_max_bytes(context_id);
    if (max_bytes == 0 || (uint32_t)length > max_bytes) {
        m3ApiReturn(WASMOS_DMA_STATUS_RANGE);
    }
    if (process_manager_buffer_dma_map((uint32_t)kind,
                                       context_id,
                                       source_owner,
                                       (uint32_t)offset,
                                       (uint32_t)length,
                                       (uint32_t)direction_flags,
                                       &device_addr) != 0) {
        m3ApiReturn(WASMOS_DMA_STATUS_DENY);
    }
    if (!capability_dma_range_allowed(context_id, device_addr, (uint64_t)(uint32_t)length)) {
        (void)process_manager_buffer_dma_unmap((uint32_t)kind, context_id, source_owner);
        m3ApiReturn(WASMOS_DMA_STATUS_RANGE);
    }
    if (device_addr > 0x7FFFFFFFULL) {
        (void)process_manager_buffer_dma_unmap((uint32_t)kind, context_id, source_owner);
        m3ApiReturn(WASMOS_DMA_STATUS_UNAVAILABLE);
    }
    m3ApiReturn((int32_t)device_addr);
}

m3ApiRawFunction(wasmos_dma_sync_borrow)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, kind)
    m3ApiGetArg(int32_t, offset)
    m3ApiGetArg(int32_t, length)
    m3ApiGetArg(int32_t, sync_op)
    uint32_t context_id = 0;

    if (kind < 0 || offset < 0 || length <= 0 ||
        (sync_op != WASMOS_DMA_SYNC_TO_DEVICE &&
         sync_op != WASMOS_DMA_SYNC_FROM_DEVICE &&
         sync_op != WASMOS_DMA_SYNC_BIDIR)) {
        m3ApiReturn(WASMOS_DMA_STATUS_INVALID);
    }
    if (current_process_context(&context_id) != 0 ||
        require_dma_capability(context_id) != 0) {
        m3ApiReturn(WASMOS_DMA_STATUS_DENY);
    }
    if (process_manager_buffer_dma_sync((uint32_t)kind,
                                        context_id,
                                        (uint32_t)offset,
                                        (uint32_t)length,
                                        (uint32_t)sync_op) != 0) {
        m3ApiReturn(WASMOS_DMA_STATUS_DENY);
    }
    m3ApiReturn(WASMOS_DMA_STATUS_OK);
}

m3ApiRawFunction(wasmos_dma_unmap_borrow)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, kind)
    m3ApiGetArg(int32_t, source_endpoint)
    uint32_t context_id = 0;
    uint32_t source_owner = 0;

    if (kind < 0) {
        m3ApiReturn(WASMOS_DMA_STATUS_INVALID);
    }
    if (current_process_context(&context_id) != 0 ||
        require_dma_capability(context_id) != 0) {
        m3ApiReturn(WASMOS_DMA_STATUS_DENY);
    }
    if (ipc_endpoint_owner((uint32_t)source_endpoint, &source_owner) != IPC_OK ||
        source_owner == 0 || source_owner == context_id) {
        m3ApiReturn(WASMOS_DMA_STATUS_DENY);
    }
    if (process_manager_buffer_dma_unmap((uint32_t)kind, context_id, source_owner) != 0) {
        m3ApiReturn(WASMOS_DMA_STATUS_DENY);
    }
    m3ApiReturn(WASMOS_DMA_STATUS_OK);
}

m3ApiRawFunction(wasmos_fs_buffer_borrow)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, source_endpoint)
    m3ApiGetArg(int32_t, flags)
    m3ApiReturn(wasm_buffer_borrow_impl((int32_t)PM_BUFFER_KIND_FILESYSTEM, source_endpoint, flags));
}

m3ApiRawFunction(wasmos_fs_buffer_release)
{
    m3ApiReturnType(int32_t)
    m3ApiReturn(wasm_buffer_release_impl((int32_t)PM_BUFFER_KIND_FILESYSTEM));
}

m3ApiRawFunction(wasmos_buffer_borrow)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, kind)
    m3ApiGetArg(int32_t, source_endpoint)
    m3ApiGetArg(int32_t, flags)
    m3ApiReturn(wasm_buffer_borrow_impl(kind, source_endpoint, flags));
}

m3ApiRawFunction(wasmos_buffer_release)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, kind)
    m3ApiReturn(wasm_buffer_release_impl(kind));
}

m3ApiRawFunction(wasmos_ipc_select_one)
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
        /* Preserve the legacy sync-spawn contract for WASM children: the
         * first blocking IPC wait marks the process ready unless it requires
         * an explicit PROC_IPC_NOTIFY_READY handshake. */
        if (!process->ready && !process->require_explicit_ready) {
            process->ready = 1;
        }
        /* Use the blocking variant: sleeps in sched_event_wait until a message
         * arrives, then dequeues and returns IPC_OK.  Returns IPC_EMPTY only
         * on a spurious wake, in which case we retry immediately. */
        rc = ipc_recv_blocking_for(context_id, (uint32_t)endpoint, &slot->message);
        if (rc == IPC_EMPTY) {
            /* Spurious wake — re-block immediately, but honour a pending
             * reschedule first so the timer tick doesn't go unserviced. */
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
            int owner_rc = ipc_endpoint_owner(slot->message.source, &owner_context);
            if (owner_rc == IPC_OK &&
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

m3ApiRawFunction(wasmos_ipc_drain)
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


m3ApiRawFunction(wasmos_sys_select_create)
{
    m3ApiReturnType(int32_t)
    uint32_t context_id = 0;
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }
    uint32_t select_id = 0;
    int rc = ipc_select_create(context_id, &select_id);
    if (rc != IPC_OK) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)select_id);
}

m3ApiRawFunction(wasmos_sys_select_add)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, select_id)
    m3ApiGetArg(int32_t, endpoint_id)
    uint32_t context_id = 0;
    if (select_id <= 0 || endpoint_id < 0 || current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }
    int rc = ipc_select_add((uint32_t)select_id, (uint32_t)endpoint_id, context_id);
    m3ApiReturn(rc == IPC_OK ? 0 : -1);
}

m3ApiRawFunction(wasmos_sys_select_wait)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, select_id)
    uint32_t context_id = 0;
    if (select_id <= 0 || current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }
    uint32_t ready_ep = IPC_ENDPOINT_NONE;
    for (;;) {
        int rc = ipc_select_wait((uint32_t)select_id, context_id, &ready_ep);
        if (rc == IPC_OK) {
            m3ApiReturn((int32_t)ready_ep);
        }
        if (rc == IPC_EMPTY) {
            /* Spurious wake — re-block, but honour pending reschedule first. */
            preempt_safepoint();
            continue;
        }
        m3ApiReturn(-1);
    }
}

m3ApiRawFunction(wasmos_sys_select_destroy)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, select_id)
    uint32_t context_id = 0;
    if (select_id <= 0 || current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }
    ipc_select_destroy((uint32_t)select_id, context_id);
    m3ApiReturn(0);
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
    m3ApiReturn((int32_t)process_manager_buffer_size(PM_BUFFER_KIND_FILESYSTEM));
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
    uint32_t borrow_flags = 0;

    if (len <= 0 || offset < 0) {
        m3ApiReturn(-1);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }
    uint32_t max_len = process_manager_buffer_size(PM_BUFFER_KIND_FILESYSTEM);
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
    borrow_flags = process_manager_buffer_borrow_flags(PM_BUFFER_KIND_FILESYSTEM, context_id);
    if (borrow_flags != 0 && (borrow_flags & 0x1u) == 0) {
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
    uint32_t borrow_flags = 0;

    if (len <= 0 || offset < 0) {
        m3ApiReturn(-1);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }
    uint32_t max_len = process_manager_buffer_size(PM_BUFFER_KIND_FILESYSTEM);
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
    borrow_flags = process_manager_buffer_borrow_flags(PM_BUFFER_KIND_FILESYSTEM, context_id);
    if (borrow_flags != 0 && (borrow_flags & 0x2u) == 0) {
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

m3ApiRawFunction(wasmos_env_get)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(const char *, name_ptr)
    m3ApiGetArg(int32_t, name_len)
    m3ApiGetArgMem(char *, buf_ptr)
    m3ApiGetArg(int32_t, buf_len)

    if (name_len <= 0 || buf_len <= 0) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(name_ptr, (uint32_t)name_len);
    m3ApiCheckMem(buf_ptr, (uint32_t)buf_len);
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t name_user = 0;
    uint64_t buf_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   name_ptr,
                                   (uint32_t)name_len,
                                   &name_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                name_user,
                                (uint64_t)(uint32_t)name_len,
                                MEM_REGION_FLAG_READ) != 0 ||
        wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   buf_ptr,
                                   (uint32_t)buf_len,
                                   &buf_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                buf_user,
                                (uint64_t)(uint32_t)buf_len,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(-1);
    }
    char local_name[KENV_KEY_MAX];
    uint32_t copy_len = (uint32_t)name_len;
    if (copy_len >= KENV_KEY_MAX) {
        copy_len = KENV_KEY_MAX - 1u;
    }
    if (mm_copy_from_user(proc->context_id, local_name, name_user, (uint64_t)copy_len) != 0) {
        m3ApiReturn(-1);
    }
    local_name[copy_len] = '\0';
    int idx = kenv_find(local_name);
    if (idx < 0) {
        m3ApiReturn(-1);
    }
    uint32_t val_len = 0;
    while (g_kenv[idx].value[val_len]) {
        val_len++;
    }
    uint32_t write_len = val_len;
    if (write_len >= (uint32_t)buf_len) {
        write_len = (uint32_t)buf_len - 1u;
    }
    if (wasm_copy_to_user_bytes(proc->context_id, buf_user, g_kenv[idx].value, write_len) != 0) {
        m3ApiReturn(-1);
    }
    char nul = '\0';
    if (wasm_copy_to_user_bytes(proc->context_id, buf_user + (uint64_t)write_len, &nul, 1) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)write_len);
}

m3ApiRawFunction(wasmos_env_set)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(const char *, name_ptr)
    m3ApiGetArg(int32_t, name_len)
    m3ApiGetArgMem(const char *, val_ptr)
    m3ApiGetArg(int32_t, val_len)

    if (name_len <= 0 || val_len < 0) {
        m3ApiReturn(-1);
    }
    if (name_len >= KENV_KEY_MAX || val_len >= KENV_VAL_MAX) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(name_ptr, (uint32_t)name_len);
    if (val_len > 0) {
        m3ApiCheckMem(val_ptr, (uint32_t)val_len);
    }
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t name_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   name_ptr,
                                   (uint32_t)name_len,
                                   &name_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                name_user,
                                (uint64_t)(uint32_t)name_len,
                                MEM_REGION_FLAG_READ) != 0) {
        m3ApiReturn(-1);
    }
    char local_name[KENV_KEY_MAX];
    if (mm_copy_from_user(proc->context_id, local_name, name_user, (uint64_t)(uint32_t)name_len) != 0) {
        m3ApiReturn(-1);
    }
    local_name[name_len] = '\0';
    char local_val[KENV_VAL_MAX];
    local_val[0] = '\0';
    if (val_len > 0) {
        uint64_t val_user = 0;
        if (wasm_user_va_from_host_ptr(proc->context_id,
                                       (const uint8_t *)_mem,
                                       (uint64_t)m3_GetMemorySize(runtime),
                                       val_ptr,
                                       (uint32_t)val_len,
                                       &val_user) != 0 ||
            mm_user_range_permitted(proc->context_id,
                                    val_user,
                                    (uint64_t)(uint32_t)val_len,
                                    MEM_REGION_FLAG_READ) != 0) {
            m3ApiReturn(-1);
        }
        if (mm_copy_from_user(proc->context_id, local_val, val_user, (uint64_t)(uint32_t)val_len) != 0) {
            m3ApiReturn(-1);
        }
        local_val[val_len] = '\0';
    }
    int idx = kenv_find(local_name);
    if (idx < 0) {
        for (int i = 0; i < KENV_MAX_ENTRIES; i++) {
            if (!g_kenv[i].in_use) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            m3ApiReturn(-1);
        }
        g_kenv[idx].in_use = 1;
        memcpy(g_kenv[idx].key, local_name, (uint32_t)name_len + 1u);
    }
    memcpy(g_kenv[idx].value, local_val, (uint32_t)val_len + 1u);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_env_unset)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(const char *, name_ptr)
    m3ApiGetArg(int32_t, name_len)

    if (name_len <= 0 || name_len >= KENV_KEY_MAX) {
        m3ApiReturn(0);
    }
    m3ApiCheckMem(name_ptr, (uint32_t)name_len);
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(0);
    }
    uint64_t name_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   name_ptr,
                                   (uint32_t)name_len,
                                   &name_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                name_user,
                                (uint64_t)(uint32_t)name_len,
                                MEM_REGION_FLAG_READ) != 0) {
        m3ApiReturn(0);
    }
    char local_name[KENV_KEY_MAX];
    if (mm_copy_from_user(proc->context_id, local_name, name_user, (uint64_t)(uint32_t)name_len) != 0) {
        m3ApiReturn(0);
    }
    local_name[name_len] = '\0';
    int idx = kenv_find(local_name);
    if (idx >= 0) {
        g_kenv[idx].in_use = 0;
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
    if (current_process_context(&context_id) != 0 ||
        require_io_capability(context_id, (uint16_t)port) != 0) {
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
    if (current_process_context(&context_id) != 0 ||
        require_io_capability(context_id, (uint16_t)port) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)inw((uint16_t)port));
}

m3ApiRawFunction(wasmos_io_in32)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, port)
    uint32_t context_id = 0;
    if (port < 0 || port > 0xFFFF) {
        m3ApiReturn(-1);
    }
    if (current_process_context(&context_id) != 0 ||
        require_io_capability(context_id, (uint16_t)port) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)inl((uint16_t)port));
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
    if (current_process_context(&context_id) != 0 ||
        require_io_capability(context_id, (uint16_t)port) != 0) {
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
    if (current_process_context(&context_id) != 0 ||
        require_io_capability(context_id, (uint16_t)port) != 0) {
        m3ApiReturn(-1);
    }
    outw((uint16_t)port, (uint16_t)value);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_io_out32)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, port)
    m3ApiGetArg(int32_t, value)
    uint32_t context_id = 0;
    if (port < 0 || port > 0xFFFF) {
        m3ApiReturn(-1);
    }
    if (current_process_context(&context_id) != 0 ||
        require_io_capability(context_id, (uint16_t)port) != 0) {
        m3ApiReturn(-1);
    }
    outl((uint16_t)port, (uint32_t)value);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_io_wait)
{
    m3ApiReturnType(int32_t)
    uint32_t context_id = 0;
    if (current_process_context(&context_id) != 0 ||
        require_io_capability(context_id, 0x80) != 0) {
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
    int va_rc = wasm_user_va_from_offset(proc->context_id, off32, map_size32, &virt);
    int perm_rc = 0;
    if (va_rc == 0) {
        perm_rc = mm_user_range_permitted(proc->context_id, virt, (uint64_t)map_size32, MEM_REGION_FLAG_WRITE);
    }
    if (va_rc != 0 || perm_rc != 0) {
        m3ApiReturn(-1);
    }
    if ((virt & 0xFFFULL) != 0) {
        m3ApiReturn(-1);
    }

    uint64_t pages = (uint64_t)map_size32 / 0x1000ULL;
    if (pages == 0) {
        klog_write("[framebuffer-map] zero pages\n");
        m3ApiReturn(-1);
    }
    uint64_t cur_virt = virt;
    uint64_t cur_phys = info.framebuffer_base;
    for (uint64_t i = 0; i < pages; ++i) {
        (void)paging_unmap_4k_in_root(ctx->root_table, cur_virt);
        if (paging_map_4k_in_root(ctx->root_table,
                                 cur_virt,
                                 cur_phys,
                                 MEM_REGION_FLAG_READ |
                                     MEM_REGION_FLAG_WRITE |
                                     MEM_REGION_FLAG_USER) < 0) {
            m3ApiReturn(-1);
        }
        cur_virt += 0x1000ULL;
        cur_phys += 0x1000ULL;
    }
    m3ApiReturn(0);
}

/* Map an arbitrary physical address range into a WASM linear-memory window.
 * phys_lo/phys_hi form a 64-bit physical address (hi=0 for 32-bit addresses).
 * wasm_offset must be page-aligned; size must be a multiple of 4096.
 * Requires the mmio.map capability. */
m3ApiRawFunction(wasmos_phys_map)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, phys_lo)
    m3ApiGetArg(int32_t, phys_hi)
    m3ApiGetArg(int32_t, size)
    m3ApiGetArg(int32_t, wasm_offset)

    if (size <= 0 || (size & 0xFFF) != 0) {
        m3ApiReturn(-1);
    }
    if (wasm_offset < 0 || (wasm_offset & 0xFFF) != 0) {
        m3ApiReturn(-1);
    }

    uint64_t phys = ((uint64_t)(uint32_t)phys_hi << 32) | (uint64_t)(uint32_t)phys_lo;
    if (phys == 0) {
        m3ApiReturn(-1);
    }

    uint32_t off32 = (uint32_t)wasm_offset;
    uint32_t size32 = (uint32_t)size;

    uint32_t mem_size = 0;
    uint8_t *mem_base = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem_base || mem_size == 0) {
        m3ApiReturn(-1);
    }
    if ((uint64_t)off32 + (uint64_t)size32 > (uint64_t)mem_size) {
        m3ApiReturn(-1);
    }

    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0 ||
        require_mmio_capability(proc->context_id) != 0) {
        m3ApiReturn(-1);
    }

    /* Copy physical memory into wasm3 linear-memory host buffer via the
     * kernel higher-half mapping.  wasm3 accesses linear memory exclusively
     * through this host pointer; the user-VA page table is irrelevant here. */
    memcpy(mem_base + off32,
           (const void *)(uintptr_t)(phys | KERNEL_HIGHER_HALF_BASE),
           (size_t)size32);
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

    if (mm_context_map_physical(proc->context_id,
                                virt,
                                phys_base,
                                needed_size,
                                MEM_REGION_FLAG_READ |
                                    MEM_REGION_FLAG_WRITE |
                                    MEM_REGION_FLAG_USER) != 0) {
        m3ApiReturn(-1);
    }

    if (mm_shared_retain(proc->context_id, (uint32_t)id) != 0) {
        m3ApiReturn(-1);
    }
    wasm_shmem_map_track(proc->pid, (uint32_t)id, off32, map_size32);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_shmem_map_auto)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, id)
    m3ApiGetArg(int32_t, size)

    if (id <= 0 || size <= 0 || (size & 0xFFF) != 0) {
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
    int get_phys_rc = mm_shared_get_phys(proc->context_id, (uint32_t)id, &phys_base, &shared_pages);
    if (get_phys_rc != 0 || shared_pages == 0) {
        m3ApiReturn(-1);
    }

    uint64_t map_size = (uint64_t)(uint32_t)size;
    uint64_t needed_size = shared_pages * 0x1000ULL;
    if (map_size < needed_size) {
        m3ApiReturn(-1);
    }

    uint64_t mem_size = (uint64_t)m3_GetMemorySize(runtime);
    wasm_linear_region_sync_size(ctx, mem_size);
    uint64_t off64 = 0;
    uint8_t found = 0;
    /* Keep auto-mapped shared pages away from low linear-memory where
     * module data/rodata/heap metadata commonly live. */
    const uint64_t map_auto_min_off = 0x200000ULL;
    uint64_t scan_off = map_auto_min_off;
    if (scan_off < 0x4000ULL) {
        scan_off = 0x4000ULL;
    }
    scan_off = (scan_off + 0xFFFULL) & ~0xFFFULL;

    for (off64 = scan_off; off64 + map_size <= mem_size; off64 += 0x1000ULL) {
        if (wasm_shmem_map_overlaps(proc->pid, (uint32_t)off64, (uint32_t)map_size)) {
            continue;
        }
        uint64_t probe_virt = 0;
        if (wasm_user_va_from_offset(proc->context_id, (uint32_t)off64, (uint32_t)map_size, &probe_virt) != 0) {
            continue;
        }
        if (mm_user_range_permitted(proc->context_id, probe_virt, (uint64_t)(uint32_t)map_size, MEM_REGION_FLAG_WRITE) != 0) {
            continue;
        }
    if ((probe_virt & 0xFFFULL) != 0) {
            continue;
        }
        found = 1;
        break;
    }
    if (!found) {
        off64 = (mem_size + 0xFFFULL) & ~0xFFFULL;
        uint64_t required = off64 + map_size;
        if (required > mem_size) {
            uint32_t pages = (uint32_t)((required + 0xFFFFULL) >> 16);
            if (ResizeMemory(runtime, pages) != m3Err_none) {
                m3ApiReturn(-1);
            }
            mem_size = (uint64_t)m3_GetMemorySize(runtime);
            wasm_linear_region_sync_size(ctx, mem_size);
            if (required > mem_size) {
                m3ApiReturn(-1);
            }
        }
    }

    uint32_t off32 = (uint32_t)off64;
    uint32_t map_size32 = (uint32_t)map_size;
    uint64_t virt = 0;
    if (wasm_user_va_from_offset(proc->context_id, off32, map_size32, &virt) != 0 ||
        mm_user_range_permitted(proc->context_id, virt, (uint64_t)map_size32, MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(-1);
    }
    if ((virt & 0xFFFULL) != 0) {
        m3ApiReturn(-1);
    }

    if (mm_context_map_physical(proc->context_id,
                                virt,
                                phys_base,
                                needed_size,
                                MEM_REGION_FLAG_READ |
                                    MEM_REGION_FLAG_WRITE |
                                    MEM_REGION_FLAG_USER) != 0) {
        m3ApiReturn(-1);
    }
    if (mm_shared_retain(proc->context_id, (uint32_t)id) != 0) {
        m3ApiReturn(-1);
    }
    wasm_shmem_map_track(proc->pid, (uint32_t)id, off32, map_size32);

    /* FIXME(shmem-map-auto): unmap currently only drops shared refs and does
     * not reclaim or reuse grown linear-memory pages for future map_auto
     * allocations. */
    m3ApiReturn((int32_t)off32);
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

m3ApiRawFunction(wasmos_shmem_revoke)
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
    m3ApiReturn(mm_shared_revoke(proc->context_id, (uint32_t)id, target->context_id));
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
    wasm_shmem_map_untrack(proc->pid, (uint32_t)id);
    m3ApiReturn(mm_shared_release(proc->context_id, (uint32_t)id));
}

m3ApiRawFunction(wasmos_shmem_flush)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, id)
    m3ApiGetArg(int32_t, ptr)
    m3ApiGetArg(int32_t, size)

    if (id <= 0 || ptr < 0 || size <= 0) {
        m3ApiReturn(-1);
    }

    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0 ||
        require_dma_capability(proc->context_id) != 0) {
        m3ApiReturn(-1);
    }

    uint64_t phys_base = 0;
    uint64_t shared_pages = 0;
    if (mm_shared_get_phys(proc->context_id, (uint32_t)id, &phys_base, &shared_pages) != 0 ||
        shared_pages == 0 || phys_base == 0) {
        m3ApiReturn(-1);
    }

    uint32_t mem_size = 0;
    uint8_t *mem_base = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem_base || mem_size == 0) {
        m3ApiReturn(-1);
    }

    uint32_t off32 = (uint32_t)ptr;
    uint32_t len32 = (uint32_t)size;
    if ((uint64_t)off32 + (uint64_t)len32 > (uint64_t)mem_size) {
        m3ApiReturn(-1);
    }

    uint64_t max_len = shared_pages * 0x1000ULL;
    if ((uint64_t)len32 > max_len) {
        m3ApiReturn(-1);
    }

    memcpy((void *)(uintptr_t)(phys_base | KERNEL_HIGHER_HALF_BASE), mem_base + off32, (size_t)len32);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_shmem_refresh)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, id)
    m3ApiGetArg(int32_t, ptr)
    m3ApiGetArg(int32_t, size)

    if (id <= 0 || ptr < 0 || size <= 0) {
        m3ApiReturn(-1);
    }

    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0 ||
        require_dma_capability(proc->context_id) != 0) {
        m3ApiReturn(-1);
    }

    uint64_t phys_base = 0;
    uint64_t shared_pages = 0;
    if (mm_shared_get_phys(proc->context_id, (uint32_t)id, &phys_base, &shared_pages) != 0 ||
        shared_pages == 0 || phys_base == 0) {
        m3ApiReturn(-1);
    }

    uint32_t mem_size = 0;
    uint8_t *mem_base = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem_base || mem_size == 0) {
        m3ApiReturn(-1);
    }

    uint32_t off32 = (uint32_t)ptr;
    uint32_t len32 = (uint32_t)size;
    if ((uint64_t)off32 + (uint64_t)len32 > (uint64_t)mem_size) {
        m3ApiReturn(-1);
    }

    uint64_t max_len = shared_pages * 0x1000ULL;
    if ((uint64_t)len32 > max_len) {
        m3ApiReturn(-1);
    }

    memcpy(mem_base + off32, (const void *)(uintptr_t)(phys_base | KERNEL_HIGHER_HALF_BASE), (size_t)len32);
    m3ApiReturn(0);
}


m3ApiRawFunction(wasmos_irq_route_ipc)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, irq_line)
    m3ApiGetArg(int32_t, msg_endpoint)

    uint32_t context_id = 0;
    if (irq_line < 0 || msg_endpoint < 0) {
        m3ApiReturn(-1);
    }
    if (current_process_context(&context_id) != 0 ||
        require_irq_route_capability(context_id) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(irq_register(context_id, (uint32_t)irq_line, (uint32_t)msg_endpoint));
}

m3ApiRawFunction(wasmos_irq_ack)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, irq_line)

    uint32_t context_id = 0;
    if (irq_line < 0) {
        m3ApiReturn(-1);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(irq_ack(context_id, (uint32_t)irq_line));
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
    kernel_system_poweroff();
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
    kernel_system_reboot();
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

m3ApiRawFunction(wasmos_initfs_entry_count)
{
    m3ApiReturnType(int32_t)
    const wasmos_initfs_header_t *hdr = 0;
    const uint8_t *base = 0;
    if (initfs_header_get(&hdr, &base) != 0) {
        m3ApiReturn(-1);
    }
    if (hdr->entry_count > 0x7FFFFFFFu) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)hdr->entry_count);
}

m3ApiRawFunction(wasmos_initfs_entry_name)
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

    wasmos_initfs_entry_t entry;
    if (initfs_entry_at((uint32_t)index, &entry) != 0) {
        m3ApiReturn(-1);
    }
    uint32_t name_len = 0;
    while (name_len < sizeof(entry.path) && entry.path[name_len] != '\0') {
        name_len++;
    }
    uint32_t copy_len = name_len;
    if (copy_len >= (uint32_t)out_len) {
        copy_len = (uint32_t)out_len - 1U;
    }
    if (copy_len > 0 &&
        wasm_copy_to_user_bytes(proc->context_id, out_user, entry.path, copy_len) != 0) {
        m3ApiReturn(-1);
    }
    {
        char nul = '\0';
        if (wasm_copy_to_user_bytes(proc->context_id, out_user + (uint64_t)copy_len, &nul, 1) != 0) {
            m3ApiReturn(-1);
        }
    }
    m3ApiReturn((int32_t)name_len);
}

m3ApiRawFunction(wasmos_initfs_entry_size)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, index)
    wasmos_initfs_entry_t entry;
    if (index < 0 || initfs_entry_at((uint32_t)index, &entry) != 0) {
        m3ApiReturn(-1);
    }
    if (entry.size > 0x7FFFFFFFu) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)entry.size);
}

m3ApiRawFunction(wasmos_initfs_entry_copy)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, index)
    m3ApiGetArgMem(uint8_t *, out_ptr)
    m3ApiGetArg(int32_t, len)
    m3ApiGetArg(int32_t, offset)

    if (index < 0 || len <= 0 || offset < 0) {
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

    wasmos_initfs_entry_t entry;
    if (initfs_entry_at((uint32_t)index, &entry) != 0) {
        m3ApiReturn(-1);
    }
    if ((uint32_t)offset >= entry.size) {
        m3ApiReturn(0);
    }
    uint32_t copy_len = (uint32_t)len;
    uint32_t available = entry.size - (uint32_t)offset;
    if (copy_len > available) {
        copy_len = available;
    }
    const uint8_t *src = (const uint8_t *)g_wasm_boot_info->initfs + entry.offset + (uint32_t)offset;
    if (wasm_copy_to_user_sync_views(proc->context_id, out_user, out_ptr, src, copy_len) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)copy_len);
}

m3ApiRawFunction(wasmos_initfs_find_path)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(const char *, path_ptr)
    m3ApiGetArg(int32_t, path_len)

    if (path_len <= 0 || path_len >= 112) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(path_ptr, (uint32_t)path_len);
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t path_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   path_ptr,
                                   (uint32_t)path_len,
                                   &path_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                path_user,
                                (uint64_t)(uint32_t)path_len,
                                MEM_REGION_FLAG_READ) != 0) {
        m3ApiReturn(-1);
    }

    char local_path[112];
    if (mm_copy_from_user(proc->context_id, local_path, path_user, (uint64_t)(uint32_t)path_len) != 0) {
        m3ApiReturn(-1);
    }
    local_path[path_len] = '\0';

    uint32_t ri = 0;
    while (local_path[ri] == '/') {
        ri++;
    }
    if ((local_path[ri] == 'i' || local_path[ri] == 'I') &&
        (local_path[ri + 1] == 'n' || local_path[ri + 1] == 'N') &&
        (local_path[ri + 2] == 'i' || local_path[ri + 2] == 'I') &&
        (local_path[ri + 3] == 't' || local_path[ri + 3] == 'T') &&
        local_path[ri + 4] == '/') {
        ri += 5;
    }
    if (local_path[ri] == '\0') {
        m3ApiReturn(-1);
    }
    const wasmos_initfs_header_t *hdr = 0;
    const uint8_t *base = 0;
    if (initfs_header_get(&hdr, &base) != 0) {
        m3ApiReturn(-1);
    }
    for (uint32_t i = 0; i < hdr->entry_count; ++i) {
        wasmos_initfs_entry_t entry;
        if (initfs_entry_at(i, &entry) != 0) {
            continue;
        }
        if (strcasecmp(entry.path, &local_path[ri]) == 0) {
            m3ApiReturn((int32_t)i);
        }
        const char *base_name = entry.path;
        for (uint32_t j = 0; entry.path[j] != '\0'; ++j) {
            if (entry.path[j] == '/') {
                base_name = &entry.path[j + 1];
            }
        }
        if (strcasecmp(base_name, &local_path[ri]) == 0) {
            m3ApiReturn((int32_t)i);
        }
    }
    m3ApiReturn(-1);
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
    /* ptr is validated by m3ApiGetArgMem/m3ApiCheckMem to be within wasm3
     * linear memory bounds. Use the host pointer directly — the user-VA
     * reconciliation path fails for apps whose heap/stack extends past the
     * initial 8-page user VA region, silencing all console output. */
    preempt_disable();
    char buf[128];
    uint32_t copied = 0;
    while (copied < (uint32_t)len) {
        uint32_t chunk = (uint32_t)len - copied;
        if (chunk > (uint32_t)(sizeof(buf) - 1U)) {
            chunk = (uint32_t)(sizeof(buf) - 1U);
        }
        __builtin_memcpy(buf, ptr + copied, chunk);
        buf[chunk] = '\0';
        klog_write(buf);
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

    trace_do(klog_write("[kmap] contexts begin\n"));
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

        trace_do(klog_write("[kmap] pid="));
        trace_do(serial_write_hex64((uint64_t)pid));
        trace_do(klog_write(" parent="));
        trace_do(serial_write_hex64((uint64_t)parent_pid));
        trace_do(klog_write(" ctx="));
        trace_do(serial_write_hex64((uint64_t)proc->context_id));
        trace_do(klog_write(" name="));
        trace_do(klog_write(name ? name : "(unknown)"));
        trace_do(klog_write("\n"));

        paging_dump_user_root_kernel_mappings(root);
        if ((proc->ctx.cs & 0x3u) == 0x3u) {
            if (paging_verify_user_root_no_low_slot(root, 1) != 0) {
                failures++;
            }
        }
    }
    trace_do(klog_write("[kmap] contexts end\n"));
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
    uint32_t endpoint_u32 = 0;
    if (wasm_arg_u32_nonneg(endpoint, &endpoint_u32) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(serial_register_remote_driver(endpoint_u32));
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

m3ApiRawFunction(wasmos_proc_notify_ready)
{
    m3ApiReturnType(int32_t)
    process_t *proc = process_get(process_current_pid());
    if (proc) {
        process_notify_ready(proc);
    }
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

m3ApiRawFunction(wasmos_sched_cpu_count)
{
    m3ApiReturnType(int32_t)
    m3ApiReturn((int32_t)g_cpu_count);
}

m3ApiRawFunction(wasmos_physmem_stats)
{
    typedef struct { uint64_t total_bytes; uint64_t free_bytes; } physmem_stats_t;
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(physmem_stats_t *, out)
    m3ApiCheckMem(out, sizeof(physmem_stats_t));
    out->total_bytes = pfa_total_bytes();
    out->free_bytes  = pfa_free_bytes();
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_kernel_runtime)
{
    m3ApiReturnType(int32_t)
    m3ApiReturn(0); /* wasm3 */
}

m3ApiRawFunction(wasmos_sched_cpu_stats)
{
    typedef struct { uint32_t ready_count; uint32_t running_pid; uint32_t steal_count; uint32_t dispatch_count; uint32_t last_pid; } cpu_stats_t;
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, cpu_id)
    m3ApiGetArgMem(cpu_stats_t *, out)
    m3ApiCheckMem(out, sizeof(cpu_stats_t));
    if (cpu_id < 0 || (uint32_t)cpu_id >= g_cpu_count) {
        m3ApiReturn(-1);
    }
    cpu_sched_t *cs = &g_cpus[(uint32_t)cpu_id].sched;
    uint32_t ready = 0;
    for (int p = 0; p < SCHED_PRIO_MAX; p++) {
        ready += cs->thread_count[p];
    }
    out->ready_count  = ready;
    out->running_pid  = g_cpus[(uint32_t)cpu_id].current_process
                      ? g_cpus[(uint32_t)cpu_id].current_process->pid : 0;
    out->steal_count    = g_cpus[(uint32_t)cpu_id].steal_count;
    out->dispatch_count = g_cpus[(uint32_t)cpu_id].dispatch_count;
    out->last_pid       = g_cpus[(uint32_t)cpu_id].last_dispatched_pid;
    m3ApiReturn(0);
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

m3ApiRawFunction(wasmos_thread_gettid)
{
    m3ApiReturnType(int32_t)
    m3ApiReturn((int32_t)thread_current_tid());
}

m3ApiRawFunction(wasmos_thread_create)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, entry_token)
    m3ApiGetArg(int32_t, arg0)
    m3ApiGetArg(int32_t, arg1)
    m3ApiGetArg(int32_t, flags)
    process_t *proc = process_get(process_current_pid());
    uint64_t mem_size = (uint64_t)m3_GetMemorySize(runtime);
    const char *entry_name = 0;
    uint32_t argv[2];
    uint32_t tid = 0;
    uint32_t argc = 2u;
    if (!proc || proc->pid == 0 || entry_token <= 0) {
        m3ApiReturn(-1);
    }
    if ((uint64_t)(uint32_t)entry_token >= mem_size) {
        m3ApiReturn(-1);
    }
    entry_name = (const char *)((const uint8_t *)_mem + (uint32_t)entry_token);
    /* Require NUL-terminated export names in-bounds to avoid host pointer
     * leaks outside linear memory. */
    {
        uint64_t i = 0;
        uint64_t max = mem_size - (uint64_t)(uint32_t)entry_token;
        uint8_t terminated = 0;
        for (; i < max && i < 64u; ++i) {
            if (entry_name[i] == '\0') {
                terminated = 1;
                break;
            }
        }
        if (!terminated) {
            m3ApiReturn(-1);
        }
    }
    if ((flags & 0x1) == 0) {
        argc = 0u;
    }
    argv[0] = (uint32_t)arg0;
    argv[1] = (uint32_t)arg1;
    if (wasm_driver_spawn_vm_thread(proc->pid,
                                    entry_name,
                                    argc,
                                    argv,
                                    &tid) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn((int32_t)tid);
}

m3ApiRawFunction(wasmos_thread_yield)
{
    m3ApiReturnType(int32_t)
    process_yield(PROCESS_RUN_YIELDED);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_thread_exit)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, status)
    process_t *proc = process_get(process_current_pid());
    if (!proc) {
        m3ApiReturn(-1);
    }
    process_set_exit_status(proc, status);
    process_yield(PROCESS_RUN_THREAD_EXITED);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_thread_join)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, tid)
    process_t *proc = process_get(process_current_pid());
    uint32_t target_tid = 0;
    int32_t exit_status = 0;
    int rc = 0;
    if (!proc || wasm_arg_u32_nonneg(tid, &target_tid) != 0) {
        m3ApiReturn(-1);
    }
    rc = process_thread_join(proc, target_tid, &exit_status);
    if (rc > 0) {
        process_yield(PROCESS_RUN_BLOCKED);
        m3ApiReturn(0);
    }
    if (rc < 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(exit_status);
}

m3ApiRawFunction(wasmos_thread_detach)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, tid)
    process_t *proc = process_get(process_current_pid());
    uint32_t target_tid = 0;
    if (!proc || wasm_arg_u32_nonneg(tid, &target_tid) != 0) {
        m3ApiReturn(-1);
    }
    m3ApiReturn(process_thread_detach(proc, target_tid));
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

m3ApiRawFunction(wasmos_proc_info_stats)
{
    typedef struct {
        uint32_t state;
        uint32_t block_reason;
        uint32_t is_wasm;
        uint32_t thread_count;
        uint32_t live_thread_count;
        uint32_t current_tid;
        uint32_t context_id;
        uint64_t cpu_ticks;
        uint64_t vm_total_bytes;
        uint64_t thread_kstack_total_bytes;
        uint64_t heap_committed_bytes;
        uint64_t rss_est_bytes;
        uint32_t last_cpu;
    } wasm_proc_stats_t;

    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, index)
    m3ApiGetArgMem(char *, buf)
    m3ApiGetArg(int32_t, buf_len)
    m3ApiGetArgMem(uint32_t *, parent_ptr)
    m3ApiGetArgMem(wasm_proc_stats_t *, stats_ptr)

    if (index < 0 || buf_len <= 0) {
        m3ApiReturn(-1);
    }
    m3ApiCheckMem(buf, (uint32_t)buf_len);
    m3ApiCheckMem(parent_ptr, sizeof(uint32_t));
    m3ApiCheckMem(stats_ptr, sizeof(wasm_proc_stats_t));
    process_t *proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(-1);
    }
    uint64_t buf_user = 0;
    uint64_t parent_user = 0;
    uint64_t stats_user = 0;
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
                                MEM_REGION_FLAG_WRITE) != 0 ||
        wasm_user_va_from_host_ptr(proc->context_id,
                                   (const uint8_t *)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime),
                                   stats_ptr,
                                   sizeof(wasm_proc_stats_t),
                                   &stats_user) != 0 ||
        mm_user_range_permitted(proc->context_id,
                                stats_user,
                                sizeof(wasm_proc_stats_t),
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(-1);
    }

    uint32_t pid = 0;
    uint32_t parent_pid = 0;
    const char *name = 0;
    process_stats_t stats = {0};
    if (process_info_at_stats((uint32_t)index, &pid, &parent_pid, &name, &stats) != 0) {
        m3ApiReturn(-1);
    }
    wasm_proc_stats_t out_stats = {
        .state = stats.state,
        .block_reason = stats.block_reason,
        .is_wasm = stats.is_wasm,
        .thread_count = stats.thread_count,
        .live_thread_count = stats.live_thread_count,
        .current_tid = stats.current_tid,
        .context_id = stats.context_id,
        .cpu_ticks = stats.cpu_ticks,
        .vm_total_bytes = stats.vm_total_bytes,
        .thread_kstack_total_bytes = stats.thread_kstack_total_bytes,
        .heap_committed_bytes = stats.heap_committed_bytes,
        .rss_est_bytes = stats.rss_est_bytes,
        .last_cpu = stats.last_cpu
    };
    if (mm_copy_to_user(proc->context_id,
                        parent_user,
                        &parent_pid,
                        sizeof(parent_pid)) != 0 ||
        mm_copy_to_user(proc->context_id,
                        stats_user,
                        &out_stats,
                        sizeof(out_stats)) != 0) {
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
    uint32_t out_len = copied + 1U;
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

#ifdef WASMOS_SCHED_THREADABLE
m3ApiRawFunction(wasmos_futex_wait)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, addr)
    m3ApiGetArg(int32_t, expected)
    m3ApiGetArg(int32_t, timeout_ms)
    uint32_t context_id = 0;
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(-1);
    }
    int result = futex_wait((uint32_t)addr, (uint32_t)expected,
                            (uint32_t)timeout_ms, context_id);
    m3ApiReturn((int32_t)result);
}

m3ApiRawFunction(wasmos_futex_wake)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, addr)
    m3ApiGetArg(int32_t, count)
    uint32_t context_id = 0;
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(0);
    }
    int woken = futex_wake((uint32_t)addr, (uint32_t)count, context_id);
    m3ApiReturn((int32_t)woken);
}
#endif /* WASMOS_SCHED_THREADABLE */

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
    klog_write("[wasm3] link failed ");
    klog_write(name);
    klog_write(": ");
    klog_write(res ? res : "unknown");
    klog_write("\n");
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
    rc |= wasm3_link_raw(module, "wasmos", "ipc_endpoint_owner", "i(i)", wasmos_ipc_endpoint_owner);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_send", "i(iiiiiiii)", wasmos_ipc_send);
    rc |= wasm3_link_raw(module, "wasmos", "fs_buffer_borrow", "i(ii)", wasmos_fs_buffer_borrow);
    rc |= wasm3_link_raw(module, "wasmos", "fs_buffer_release", "i()", wasmos_fs_buffer_release);
    rc |= wasm3_link_raw(module, "wasmos", "buffer_borrow", "i(iii)", wasmos_buffer_borrow);
    rc |= wasm3_link_raw(module, "wasmos", "buffer_release", "i(i)", wasmos_buffer_release);
    rc |= wasm3_link_raw(module, "wasmos", "dma_map_borrow", "i(iiiii)", wasmos_dma_map_borrow);
    rc |= wasm3_link_raw(module, "wasmos", "dma_sync_borrow", "i(iiii)", wasmos_dma_sync_borrow);
    rc |= wasm3_link_raw(module, "wasmos", "dma_unmap_borrow", "i(ii)", wasmos_dma_unmap_borrow);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_select_one", "i(i)", wasmos_ipc_select_one);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_recv", "i(i)", wasmos_ipc_select_one); /* legacy alias */
    /* Both naming conventions: sys_select_* (new) and ipc_select_* (legacy ESP binaries). */
    rc |= wasm3_link_raw(module, "wasmos", "sys_select_create",  "i()",   wasmos_sys_select_create);
    rc |= wasm3_link_raw(module, "wasmos", "sys_select_add",     "i(ii)", wasmos_sys_select_add);
    rc |= wasm3_link_raw(module, "wasmos", "sys_select_wait",    "i(i)",  wasmos_sys_select_wait);
    rc |= wasm3_link_raw(module, "wasmos", "sys_select_destroy", "i(i)",  wasmos_sys_select_destroy);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_select_create",  "i()",   wasmos_sys_select_create);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_select_add",     "i(ii)", wasmos_sys_select_add);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_select_wait",    "i(i)",  wasmos_sys_select_wait);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_select_destroy", "i(i)",  wasmos_sys_select_destroy);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_drain", "i(i)", wasmos_ipc_drain);
    rc |= wasm3_link_raw(module, "wasmos", "ipc_try_recv", "i(i)", wasmos_ipc_drain); /* legacy alias */
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
    rc |= wasm3_link_raw(module, "wasmos", "proc_notify_ready", "i()", wasmos_proc_notify_ready);
    rc |= wasm3_link_raw(module, "wasmos", "sched_ticks", "i()", wasmos_sched_ticks);
    rc |= wasm3_link_raw(module, "wasmos", "sched_ready_count", "i()", wasmos_sched_ready_count);
    rc |= wasm3_link_raw(module, "wasmos", "sched_current_pid", "i()", wasmos_sched_current_pid);
    rc |= wasm3_link_raw(module, "wasmos", "sched_cpu_count",   "i()", wasmos_sched_cpu_count);
    rc |= wasm3_link_raw(module, "wasmos", "sched_cpu_stats",   "i(i*)", wasmos_sched_cpu_stats);
    rc |= wasm3_link_raw(module, "wasmos", "physmem_stats",     "i(*)", wasmos_physmem_stats);
    rc |= wasm3_link_raw(module, "wasmos", "kernel_runtime",    "i()", wasmos_kernel_runtime);
    rc |= wasm3_link_raw(module, "wasmos", "sched_yield", "i()", wasmos_sched_yield);
    rc |= wasm3_link_raw(module, "wasmos", "thread_gettid", "i()", wasmos_thread_gettid);
    rc |= wasm3_link_raw(module, "wasmos", "thread_create", "i(iiii)", wasmos_thread_create);
    rc |= wasm3_link_raw(module, "wasmos", "thread_yield", "i()", wasmos_thread_yield);
    rc |= wasm3_link_raw(module, "wasmos", "thread_exit", "i(i)", wasmos_thread_exit);
    rc |= wasm3_link_raw(module, "wasmos", "thread_join", "i(i)", wasmos_thread_join);
    rc |= wasm3_link_raw(module, "wasmos", "thread_detach", "i(i)", wasmos_thread_detach);
    rc |= wasm3_link_raw(module, "wasmos", "proc_info", "i(i*i)", wasmos_proc_info);
    rc |= wasm3_link_raw(module, "wasmos", "proc_info_ex", "i(i*i*)", wasmos_proc_info_ex);
    rc |= wasm3_link_raw(module, "wasmos", "proc_info_stats", "i(i*i**)", wasmos_proc_info_stats);
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
    rc |= wasm3_link_raw(module, "wasmos", "env_get",   "i(*i*i)", wasmos_env_get);
    rc |= wasm3_link_raw(module, "wasmos", "env_set",   "i(*i*i)", wasmos_env_set);
    rc |= wasm3_link_raw(module, "wasmos", "env_unset", "i(*i)",   wasmos_env_unset);
    rc |= wasm3_link_raw(module, "wasmos", "system_halt", "i()", wasmos_system_halt);
    rc |= wasm3_link_raw(module, "wasmos", "system_reboot", "i()", wasmos_system_reboot);
    rc |= wasm3_link_raw(module, "wasmos", "acpi_rsdp_info", "i(**i)", wasmos_acpi_rsdp_info);
    rc |= wasm3_link_raw(module, "wasmos", "boot_module_name", "i(i*i)", wasmos_boot_module_name);
    rc |= wasm3_link_raw(module, "wasmos", "initfs_entry_count", "i()", wasmos_initfs_entry_count);
    rc |= wasm3_link_raw(module, "wasmos", "initfs_entry_name", "i(i*i)", wasmos_initfs_entry_name);
    rc |= wasm3_link_raw(module, "wasmos", "initfs_entry_size", "i(i)", wasmos_initfs_entry_size);
    rc |= wasm3_link_raw(module, "wasmos", "initfs_entry_copy", "i(i*ii)", wasmos_initfs_entry_copy);
    rc |= wasm3_link_raw(module, "wasmos", "initfs_find_path", "i(*i)", wasmos_initfs_find_path);
    rc |= wasm3_link_raw(module, "wasmos", "io_in8", "i(i)", wasmos_io_in8);
    rc |= wasm3_link_raw(module, "wasmos", "io_in16", "i(i)", wasmos_io_in16);
    rc |= wasm3_link_raw(module, "wasmos", "io_in32", "i(i)", wasmos_io_in32);
    rc |= wasm3_link_raw(module, "wasmos", "io_out8", "i(ii)", wasmos_io_out8);
    rc |= wasm3_link_raw(module, "wasmos", "io_out16", "i(ii)", wasmos_io_out16);
    rc |= wasm3_link_raw(module, "wasmos", "io_out32", "i(ii)", wasmos_io_out32);
    rc |= wasm3_link_raw(module, "wasmos", "io_wait", "i()", wasmos_io_wait);
    rc |= wasm3_link_raw(module, "wasmos", "framebuffer_info", "i(ii)", wasmos_framebuffer_info);
    rc |= wasm3_link_raw(module, "wasmos", "framebuffer_map", "i(ii)", wasmos_framebuffer_map);
    rc |= wasm3_link_raw(module, "wasmos", "phys_map", "i(iiii)", wasmos_phys_map);
    rc |= wasm3_link_raw(module, "wasmos", "framebuffer_pixel", "i(iii)", wasmos_framebuffer_pixel);
    rc |= wasm3_link_raw(module, "wasmos", "shmem_create", "i(ii)", wasmos_shmem_create);
    rc |= wasm3_link_raw(module, "wasmos", "shmem_grant", "i(ii)", wasmos_shmem_grant);
    rc |= wasm3_link_raw(module, "wasmos", "shmem_revoke", "i(ii)", wasmos_shmem_revoke);
    rc |= wasm3_link_raw(module, "wasmos", "shmem_map", "i(iii)", wasmos_shmem_map);
    rc |= wasm3_link_raw(module, "wasmos", "shmem_map_auto", "i(ii)", wasmos_shmem_map_auto);
    rc |= wasm3_link_raw(module, "wasmos", "shmem_flush", "i(iii)", wasmos_shmem_flush);
    rc |= wasm3_link_raw(module, "wasmos", "shmem_refresh", "i(iii)", wasmos_shmem_refresh);
    rc |= wasm3_link_raw(module, "wasmos", "shmem_unmap", "i(i)", wasmos_shmem_unmap);
    rc |= wasm3_link_raw(module, "wasmos", "irq_route_ipc", "i(ii)", wasmos_irq_route_ipc);
    rc |= wasm3_link_raw(module, "wasmos", "irq_ack", "i(i)", wasmos_irq_ack);
    rc |= wasm3_link_raw(module, "wasmos", "irq_unroute", "i(i)", wasmos_irq_unroute);
    rc |= wasm3_link_raw(module, "wasmos", "serial_register", "i(i)", wasmos_serial_register);
    rc |= wasm3_link_raw(module, "wasmos", "input_push", "i(i)", wasmos_input_push);
    rc |= wasm3_link_raw(module, "wasmos", "input_read", "i()", wasmos_input_read);
#ifdef WASMOS_SCHED_THREADABLE
    rc |= wasm3_link_raw(module, "wasmos", "futex_wait", "i(iii)", wasmos_futex_wait);
    rc |= wasm3_link_raw(module, "wasmos", "futex_wake", "i(ii)",  wasmos_futex_wake);
#endif
    if (rc != 0) {
        klog_write("[kernel] wasm3 link errors\n");
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
        klog_write("[kernel] wasm3 env link errors\n");
        return -1;
    }
    return 0;
}
