#include "boot.h"
#include "arch/x86_64/smp.h"
#include "klog.h"
#include "block_buffer.h"
#include "hostcall_value.h"
#include "kenv.h"
#include "hostcall_buffer.h"
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
#include "wasmos_status.h"
#include "framebuffer.h"
#include "irq.h"
#include "msi.h"
#include "mmio.h"
#include "policy.h"
#include "capability.h"
#include "system_control.h"
#include "thread.h"
#include "wasm_driver.h"
#include "wasm3/shim.h"
#include "wasm3/link_ipc.h"
#include "sync/spinlock.h"

#include "futex.h"

/* Generated wasm3 host-call link table (WASMOS_WASM3_LINKS); see
 * abi/hostcalls.yaml + scripts/gen_abi_hostcalls.py. */
#include "wasmos_link_wasm3.inc"

#include <stdint.h>
#include <string.h>

extern M3Result ResizeMemory(IM3Runtime io_runtime, uint32_t i_numPages);

typedef struct {
    uint32_t pid;
    uint64_t buffer_phys;
    uint32_t map_offset; /* linmem offset of the zero-copy overlay, 0 if unmapped */
} wasm_block_slot_t;

#define WASM_BLOCK_BUFFER_PAGES 2u
#define WASM_BLOCK_BUFFER_SIZE_BYTES (WASM_BLOCK_BUFFER_PAGES * 4096u)

typedef struct {
    uint32_t pid;
    uint32_t shmem_id;
    uint32_t offset;
    uint32_t size;
    uint8_t valid;
} wasm_shmem_linear_map_t;

typedef struct {
    uint32_t pid;
    uint32_t offset;
    uint32_t size;
    uint32_t pages;
    uint64_t phys_base;
    uint8_t valid;
} wasm_dma_region_map_t;

static wasm_ipc_last_slot_t g_wasm_last_slots[PROCESS_MAX_COUNT];
static wasm_block_slot_t g_wasm_block_slots[PROCESS_MAX_COUNT];
static wasm_fs_peer_slot_t g_wasm_fs_peer_slots[PROCESS_MAX_COUNT];
static ksync_spinlock_t g_wasm_side_table_lock;
/* Allow several SHMEM mappings per process (UI + multiple window buffers + aux buffers). */
#define WASM_SHMEM_MAP_SLOTS (PROCESS_MAX_COUNT * 32)
static wasm_shmem_linear_map_t g_wasm_shmem_maps[WASM_SHMEM_MAP_SLOTS];
/* Driver-owned DMA regions (virtqueues, packet pools) also occupy linear-memory
 * windows and therefore must participate in overlap tracking / teardown. */
#define WASM_DMA_REGION_MAP_SLOTS (PROCESS_MAX_COUNT * 16)
static wasm_dma_region_map_t g_wasm_dma_region_maps[WASM_DMA_REGION_MAP_SLOTS];
static const boot_info_t* g_wasm_boot_info;

static int wasm_arg_u32_nonneg(int32_t raw, uint32_t* out) {
    if (!out || raw < 0) {
        return -1;
    }
    *out = (uint32_t)raw;
    return 0;
}

static int wasm_copy_to_user_bytes(uint32_t context_id, uint64_t user_dst, const void* src,
                                   uint32_t len) {
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

static int wasm_copy_from_user_bytes(uint32_t context_id, uint64_t user_src, void* dst,
                                     uint32_t len) {
    if (!dst) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (mm_copy_from_user(context_id, dst, user_src, (uint64_t)len) != 0) {
        return -1;
    }
    return 0;
}

static int boot_module_name_at(uint32_t index, char* out, uint32_t out_len,
                               uint32_t* out_name_len) {
    if (!g_wasm_boot_info || !out || out_len == 0 ||
        !(g_wasm_boot_info->flags & BOOT_INFO_FLAG_MODULES_PRESENT) || !g_wasm_boot_info->modules ||
        g_wasm_boot_info->module_entry_size < sizeof(boot_module_t)) {
        return -1;
    }
    if (index >= g_wasm_boot_info->module_count) {
        return -1;
    }

    const uint8_t* mods = (const uint8_t*)g_wasm_boot_info->modules;
    const boot_module_t* mod =
        (const boot_module_t*)(mods + index * g_wasm_boot_info->module_entry_size);
    if (!mod || mod->type != BOOT_MODULE_TYPE_WASMOS_APP || mod->base == 0 || mod->size == 0 ||
        mod->size > 0xFFFFFFFFULL) {
        return -1;
    }

    wasmos_app_desc_t desc;
    if (wasmos_app_parse(ptr_cast(uint8_t, mod->base), (uint32_t)mod->size, &desc) != 0) {
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

static int initfs_header_get(const wasmos_initfs_header_t** out_hdr, const uint8_t** out_base) {
    const wasmos_initfs_header_t* hdr = 0;
    const uint8_t* base = 0;
    uint64_t entries_bytes = 0;
    uint64_t entries_end = 0;

    if (!out_hdr || !out_base || !g_wasm_boot_info ||
        !(g_wasm_boot_info->flags & BOOT_INFO_FLAG_INITFS_PRESENT) || !g_wasm_boot_info->initfs ||
        g_wasm_boot_info->initfs_size < sizeof(wasmos_initfs_header_t)) {
        return -1;
    }

    base = (const uint8_t*)g_wasm_boot_info->initfs;
    hdr = (const wasmos_initfs_header_t*)base;
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

static int initfs_entry_at(uint32_t index, wasmos_initfs_entry_t* out) {
    const wasmos_initfs_header_t* hdr = 0;
    const uint8_t* base = 0;
    const uint8_t* entries_base = 0;
    uint64_t payload_end = 0;
    if (!out || initfs_header_get(&hdr, &base) != 0) {
        return -1;
    }
    if (index >= hdr->entry_count) {
        return -1;
    }

    entries_base = base + hdr->header_size;

    const wasmos_initfs_entry_t* entry =
        (const wasmos_initfs_entry_t*)(entries_base + ((uint64_t)index * hdr->entry_size));
    payload_end = (uint64_t)entry->offset + (uint64_t)entry->size;
    if (payload_end > (uint64_t)g_wasm_boot_info->initfs_size) {
        return -1;
    }
    *out = *entry;
    return 0;
}

static void wasm_ipc_slots_init(void) {
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        g_wasm_last_slots[i].pid = 0;
        g_wasm_last_slots[i].valid = 0;
        g_wasm_block_slots[i].pid = 0;
        g_wasm_block_slots[i].buffer_phys = 0;
        g_wasm_block_slots[i].map_offset = 0;
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
    for (uint32_t i = 0; i < WASM_DMA_REGION_MAP_SLOTS; ++i) {
        g_wasm_dma_region_maps[i].pid = 0;
        g_wasm_dma_region_maps[i].offset = 0;
        g_wasm_dma_region_maps[i].size = 0;
        g_wasm_dma_region_maps[i].pages = 0;
        g_wasm_dma_region_maps[i].phys_base = 0;
        g_wasm_dma_region_maps[i].valid = 0;
    }
}

static void wasm_shmem_map_track(uint32_t pid, uint32_t shmem_id, uint32_t offset, uint32_t size) {
    wasm_shmem_linear_map_t* empty = 0;
    for (uint32_t i = 0; i < WASM_SHMEM_MAP_SLOTS; ++i) {
        wasm_shmem_linear_map_t* slot = &g_wasm_shmem_maps[i];
        if (slot->valid && slot->pid == pid && slot->shmem_id == shmem_id &&
            slot->offset == offset) {
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

static void wasm_shmem_map_untrack(uint32_t pid, uint32_t shmem_id) {
    for (uint32_t i = 0; i < WASM_SHMEM_MAP_SLOTS; ++i) {
        wasm_shmem_linear_map_t* slot = &g_wasm_shmem_maps[i];
        if (!slot->valid)
            continue;
        if (slot->pid == pid && slot->shmem_id == shmem_id) {
            slot->valid = 0;
        }
    }
}

static uint8_t wasm_shmem_map_overlaps(uint32_t pid, uint32_t offset, uint32_t size) {
    uint64_t a0 = (uint64_t)offset;
    uint64_t a1 = a0 + (uint64_t)size;
    for (uint32_t i = 0; i < WASM_SHMEM_MAP_SLOTS; ++i) {
        const wasm_shmem_linear_map_t* slot = &g_wasm_shmem_maps[i];
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

static void wasm_dma_region_map_track(uint32_t pid, uint32_t offset, uint32_t size,
                                      uint64_t phys_base, uint32_t pages) {
    wasm_dma_region_map_t* empty = 0;
    for (uint32_t i = 0; i < WASM_DMA_REGION_MAP_SLOTS; ++i) {
        wasm_dma_region_map_t* slot = &g_wasm_dma_region_maps[i];
        if (slot->valid && slot->pid == pid && slot->offset == offset) {
            slot->size = size;
            slot->pages = pages;
            slot->phys_base = phys_base;
            return;
        }
        if (!empty && !slot->valid) {
            empty = slot;
        }
    }
    if (empty) {
        empty->pid = pid;
        empty->offset = offset;
        empty->size = size;
        empty->pages = pages;
        empty->phys_base = phys_base;
        empty->valid = 1;
    }
}

static uint8_t wasm_dma_region_map_overlaps(uint32_t pid, uint32_t offset, uint32_t size) {
    uint64_t a0 = (uint64_t)offset;
    uint64_t a1 = a0 + (uint64_t)size;
    for (uint32_t i = 0; i < WASM_DMA_REGION_MAP_SLOTS; ++i) {
        const wasm_dma_region_map_t* slot = &g_wasm_dma_region_maps[i];
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

static uint8_t wasm_linear_window_overlaps(uint32_t pid, uint32_t offset, uint32_t size) {
    return wasm_shmem_map_overlaps(pid, offset, size) ||
           wasm_dma_region_map_overlaps(pid, offset, size);
}

void wasm3_release_pid(uint32_t pid) {
    if (pid == 0) {
        return;
    }
    for (uint32_t i = 0; i < WASM_DMA_REGION_MAP_SLOTS; ++i) {
        wasm_dma_region_map_t* slot = &g_wasm_dma_region_maps[i];
        if (!slot->valid || slot->pid != pid) {
            continue;
        }
        if (slot->phys_base != 0 && slot->pages != 0) {
            pfa_free_pages(slot->phys_base, (uint64_t)slot->pages);
        }
        slot->pid = 0;
        slot->offset = 0;
        slot->size = 0;
        slot->pages = 0;
        slot->phys_base = 0;
        slot->valid = 0;
    }
    for (uint32_t i = 0; i < WASM_SHMEM_MAP_SLOTS; ++i) {
        if (g_wasm_shmem_maps[i].pid == pid) {
            g_wasm_shmem_maps[i].pid = 0;
            g_wasm_shmem_maps[i].shmem_id = 0;
            g_wasm_shmem_maps[i].offset = 0;
            g_wasm_shmem_maps[i].size = 0;
            g_wasm_shmem_maps[i].valid = 0;
        }
    }
    /* Reclaim the per-pid block buffer.  Capture the phys under the side-table
     * lock and free outside it so the PFA lock never nests under it. */
    uint64_t block_phys = 0;
    ksync_spinlock_lock(&g_wasm_side_table_lock);
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_wasm_block_slots[i].pid == pid) {
            block_phys = g_wasm_block_slots[i].buffer_phys;
            g_wasm_block_slots[i].pid = 0;
            g_wasm_block_slots[i].buffer_phys = 0;
            g_wasm_block_slots[i].map_offset = 0;
            break;
        }
    }
    ksync_spinlock_unlock(&g_wasm_side_table_lock);
    if (block_phys != 0) {
        pfa_free_pages(block_phys, WASM_BLOCK_BUFFER_PAGES);
    }
}

wasm_ipc_last_slot_t* wasm_ipc_slot_for_pid(uint32_t pid) {
    wasm_ipc_last_slot_t* empty = 0;
    wasm_ipc_last_slot_t* slot = 0;

    if (pid == 0) {
        return 0;
    }

    ksync_spinlock_lock(&g_wasm_side_table_lock);
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_wasm_last_slots[i].pid == pid) {
            slot = &g_wasm_last_slots[i];
            break;
        }
        if (!empty && g_wasm_last_slots[i].pid == 0) {
            empty = &g_wasm_last_slots[i];
        }
    }

    if (!slot && empty) {
        empty->pid = pid;
        empty->valid = 0;
        slot = empty;
    }
    ksync_spinlock_unlock(&g_wasm_side_table_lock);
    return slot;
}

static wasm_block_slot_t* wasm_block_slot_for_pid(uint32_t pid) {
    wasm_block_slot_t* empty = 0;
    wasm_block_slot_t* slot = 0;

    if (pid == 0) {
        return 0;
    }

    ksync_spinlock_lock(&g_wasm_side_table_lock);
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_wasm_block_slots[i].pid == pid) {
            slot = &g_wasm_block_slots[i];
            break;
        }
        if (!empty && g_wasm_block_slots[i].pid == 0) {
            empty = &g_wasm_block_slots[i];
        }
    }

    if (!slot && empty) {
        empty->pid = pid;
        empty->buffer_phys = 0;
        empty->map_offset = 0;
        slot = empty;
    }
    ksync_spinlock_unlock(&g_wasm_side_table_lock);
    return slot;
}

/* Report whether phys names the base of a live block buffer owned by ANY pid.
 * copy/write are called cross-process: a block server accesses a client's
 * buffer by the physical address the client handed over via IPC, so the owning
 * slot is usually not the caller's.  Matching phys against a live slot is what
 * bounds the hostcall to real 8 KiB block buffers instead of arbitrary physical
 * memory.  Returns 1 if a live slot matches, 0 otherwise. */
static int wasm_block_slot_phys_is_live(uint64_t phys) {
    int found = 0;
    if (phys == 0) {
        return 0;
    }
    ksync_spinlock_lock(&g_wasm_side_table_lock);
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_wasm_block_slots[i].pid != 0 && g_wasm_block_slots[i].buffer_phys == phys) {
            found = 1;
            break;
        }
    }
    ksync_spinlock_unlock(&g_wasm_side_table_lock);
    return found;
}

int current_process_context(uint32_t* out_context_id) {
    uint32_t pid = process_current_pid();
    process_t* proc = process_get(pid);

    if (!proc || !out_context_id) {
        return -1;
    }

    *out_context_id = proc->context_id;
    return 0;
}

static int wasm_user_va_from_offset(uint32_t context_id, uint32_t offset, uint32_t span,
                                    uint64_t* out_user_va) {
    if (context_id == 0 || span == 0 || !out_user_va) {
        return -1;
    }
    mm_context_t* ctx = mm_context_get(context_id);
    if (!ctx) {
        return -1;
    }
    mem_region_t linear = {0};
    if (mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &linear) != 0) {
        return -1;
    }
    uint64_t off = (uint64_t)offset;
    uint64_t len = (uint64_t)span;
    /* Guest offset 0 lives at region page 1: page 0 holds the M3MemoryHeader
     * (the interpreter's `mallocated`).  Shift the user VA by one page and bound
     * against the data area (region size minus the header page) so overlays,
     * copy helpers and the interpreter all resolve the same address. */
    if (linear.size < 0x1000ULL) {
        return -1;
    }
    uint64_t data_size = linear.size - 0x1000ULL;
    if (off > data_size || len > (data_size - off)) {
        return -1;
    }
    *out_user_va = linear.base + 0x1000ULL + off;
    return 0;
}

static int wasm_user_va_from_host_ptr(uint32_t context_id, const uint8_t* mem_base,
                                      uint64_t mem_size, const void* host_ptr, uint32_t span,
                                      uint64_t* out_user_va) {
    if (!mem_base || !host_ptr || span == 0 || !out_user_va) {
        return -1;
    }
    const uint8_t* ptr = (const uint8_t*)host_ptr;
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
static int require_io_capability(uint32_t context_id, uint16_t port) {
    return policy_authorize(context_id, POLICY_ACTION_IO_PORT, port);
}

static int require_mmio_capability(uint32_t context_id) {
    return policy_authorize(context_id, POLICY_ACTION_MMIO_MAP, 0);
}

static int require_dma_capability(uint32_t context_id) {
    return policy_authorize(context_id, POLICY_ACTION_DMA_BUFFER, 0);
}

static int require_irq_route_capability(uint32_t context_id) {
    return policy_authorize(context_id, POLICY_ACTION_IRQ_CONTROL, 0);
}

/* system.control is binary: any denial means the process must not be calling
 * this.  policy_require kills the process instead of returning a silent -1. */
static int require_system_control_capability(uint32_t context_id) {
    return policy_require(context_id, POLICY_ACTION_SYSTEM_CONTROL, 0);
}

static int wasm_console_should_mirror_to_vt(void) {
    process_t* proc = process_get(process_current_pid());
    if (!proc) {
        return 0;
    }

    if (proc->name && memcmp(proc->name, "hello-", 6) == 0 && (proc->name[6] != '\0')) {
        return 1;
    }

    if (proc->parent_pid == 0) {
        return 0;
    }

    process_t* parent = process_get(proc->parent_pid);
    if (!parent || !parent->name) {
        return 0;
    }

    /* Restrict mirrored VT output to shell-launched app workloads for now.
     * FIXME: Replace this parent-name heuristic with explicit per-process
     * console routing policy once PM exposes tty ownership metadata. */
    return strcmp(parent->name, "cli") == 0;
}

static void wasm_console_write_vt_mirror(const char* ptr, int32_t len) {
    uint32_t vt_endpoint = process_manager_vt_endpoint();
    if (vt_endpoint == IPC_ENDPOINT_NONE || !ptr || len <= 0 ||
        !wasm_console_should_mirror_to_vt()) {
        return;
    }

    for (int32_t offset = 0; offset < len;) {
        ipc_message_t msg;
        int32_t chunk[4] = {0, 0, 0, 0};

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

wasm_fs_peer_slot_t* wasm_fs_peer_slot_for_pid(uint32_t pid) {
    wasm_fs_peer_slot_t* empty = 0;
    wasm_fs_peer_slot_t* slot = 0;

    if (pid == 0) {
        return 0;
    }

    ksync_spinlock_lock(&g_wasm_side_table_lock);
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_wasm_fs_peer_slots[i].pid == pid) {
            slot = &g_wasm_fs_peer_slots[i];
            break;
        }
        if (!empty && g_wasm_fs_peer_slots[i].pid == 0) {
            empty = &g_wasm_fs_peer_slots[i];
        }
    }

    if (!slot && empty) {
        empty->pid = pid;
        empty->valid = 0;
        empty->peer_context_id = 0;
        slot = empty;
    }
    ksync_spinlock_unlock(&g_wasm_side_table_lock);
    return slot;
}

/* Whether the calling context may own/lend transfer buffers. Owning a transfer
 * buffer is like opening a file descriptor: any real process may acquire, borrow
 * and release one so it can move IPC payloads. The DMA capability is enforced
 * separately at dma_map_borrow (require_dma_capability), so this no longer gates
 * on CAP_DMA_BUFFER or the fs-manager name. */
static int wasm_buffer_role_allowed(uint32_t context_id, const process_t* proc) {
    (void)context_id;
    return proc != NULL;
}

/* acquire: create a buffer object owned by the caller; returns the buffer_id
 * userspace carries back (like a file descriptor), or a negative object status. */
static int32_t wasm_buffer_acquire_impl(int32_t kind, int32_t minimum_size) {
    uint32_t context_id = 0;
    process_t* proc = process_get(process_current_pid());
    xfer_buffer_owner_t owner;
    int rc = 0;

    if (kind != (int32_t)BUFFER_KIND_TRANSFER) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_KIND;
    }
    if (minimum_size <= 0) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_SIZE;
    }
    if (current_process_context(&context_id) != 0) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    }
    if (!wasm_buffer_role_allowed(context_id, proc)) {
        return WASMOS_ERR_XFER_BUFFER_NO_ACCESS;
    }
    rc = xfer_buffer_acquire((uint32_t)kind, context_id, (uint32_t)minimum_size, &owner);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    return (int32_t)owner.buffer.buffer_id;
}

/* spawn_info_buffer: return the calling process's spawn-info buffer_id (0 if
 * none). The child reads its wasmos_spawn_info_t header + args blob from it. */
static int32_t wasm_spawn_info_buffer_impl(void) {
    process_t* proc = process_get(process_current_pid());
    if (!proc) {
        return 0;
    }
    return (int32_t)proc->spawn_info_buffer_id;
}

/* borrow: caller borrows object buffer_id owned by the context that owns
 * source_endpoint; returns the borrow_id, or a negative object status. */
static int32_t wasm_buffer_borrow_impl(int32_t kind, int32_t grantee_endpoint, int32_t buffer_id,
                                       int32_t flags) {
    uint32_t context_id = 0; /* caller == the OWNER assigning the grant */
    uint32_t grantee_context = 0;
    process_t* proc = process_get(process_current_pid());
    xfer_buffer_t key;
    xfer_buffer_owner_t owner;
    xfer_buffer_borrow_t out;
    int rc = 0;

    if (kind != (int32_t)BUFFER_KIND_TRANSFER) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_KIND;
    }
    if (buffer_id <= 0) {
        return WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    }
    if (flags <= 0 || (flags & ~0x3) != 0) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_FLAGS;
    }
    if (current_process_context(&context_id) != 0) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    }
    if (!wasm_buffer_role_allowed(context_id, proc)) {
        return WASMOS_ERR_XFER_BUFFER_NO_ACCESS;
    }
    /* Owner-driven grant: the caller must own buffer_id and names the grantee by
     * an endpoint it owns. This is where access rights are assigned. */
    if (ipc_endpoint_owner((uint32_t)grantee_endpoint, &grantee_context) != IPC_OK ||
        grantee_context == 0) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    }
    key.kind = (uint32_t)kind;
    key.buffer_id = (uint32_t)buffer_id;
    key.size_bytes = 0u;
    rc = xfer_buffer_get_owned(&key, context_id, &owner); /* caller must be owner */
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    rc = xfer_buffer_borrow(&owner, grantee_context, (uint32_t)flags, &out);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    return (int32_t)out.borrow_id;
}

/* reborrow: a current borrower extends a (rights-narrowed) sub-grant of its own
 * borrow to the context that owns grantee_endpoint. The caller must hold the
 * borrow named by borrow_id; requested flags must be a subset of that borrow's
 * rights. Returns the new borrow_id (the downstream grantee's handle). */
static int32_t wasm_buffer_reborrow_impl(int32_t kind, int32_t grantee_endpoint, int32_t borrow_id,
                                         int32_t flags) {
    uint32_t context_id = 0; /* caller == an existing borrower */
    uint32_t grantee_context = 0;
    xfer_buffer_borrow_t upstream;
    xfer_buffer_borrow_t out;
    int rc = 0;

    if (kind != (int32_t)BUFFER_KIND_TRANSFER) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_KIND;
    }
    if (borrow_id <= 0) {
        return WASMOS_ERR_XFER_BUFFER_INACTIVE_BORROW;
    }
    if (flags <= 0 || (flags & ~0x3) != 0) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_FLAGS;
    }
    if (current_process_context(&context_id) != 0) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    }
    if (ipc_endpoint_owner((uint32_t)grantee_endpoint, &grantee_context) != IPC_OK ||
        grantee_context == 0) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    }
    /* Resolve the caller's own upstream borrow, then sub-grant it. */
    rc = xfer_buffer_get_borrowed((uint32_t)borrow_id, context_id, &upstream, 0);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    rc = xfer_buffer_reborrow(&upstream, grantee_context, (uint32_t)flags, &out);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    return (int32_t)out.borrow_id;
}

/* release: the owner destroys its own object named by buffer_id. */
static int32_t wasm_buffer_release_impl(int32_t kind, int32_t buffer_id) {
    uint32_t context_id = 0;
    process_t* proc = process_get(process_current_pid());
    xfer_buffer_t key;
    xfer_buffer_owner_t owner;
    int rc = 0;

    if (kind != (int32_t)BUFFER_KIND_TRANSFER) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_KIND;
    }
    if (buffer_id <= 0) {
        return WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    }
    if (current_process_context(&context_id) != 0) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    }
    if (!wasm_buffer_role_allowed(context_id, proc)) {
        return WASMOS_ERR_XFER_BUFFER_NO_ACCESS;
    }
    key.kind = (uint32_t)kind;
    key.buffer_id = (uint32_t)buffer_id;
    key.size_bytes = 0u;
    rc = xfer_buffer_get_owned(&key, context_id, &owner);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    return xfer_buffer_release_owned(&owner);
}

/* unborrow: the GRANTOR (lender) of a (re)borrow drops it, cascading downstream.
 * Owner-push: only whoever created the borrow (its lender) may unborrow it —
 * resolved via get_lent, not get_borrowed. */
static int32_t wasm_buffer_unborrow_impl(int32_t borrow_id) {
    uint32_t context_id = 0;
    xfer_buffer_borrow_t borrow;
    int rc = 0;

    if (borrow_id <= 0) {
        return WASMOS_ERR_XFER_BUFFER_INACTIVE_BORROW;
    }
    if (current_process_context(&context_id) != 0) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    }
    rc = xfer_buffer_get_lent((uint32_t)borrow_id, context_id, &borrow);
    if (rc != WASMOS_ERR_NONE) {
        return rc;
    }
    return xfer_buffer_unborrow(&borrow);
}

m3ApiRawFunction(wasmos_dma_map_borrow) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, borrow_id) m3ApiGetArg(int32_t, offset)
        m3ApiGetArg(int32_t, length) m3ApiGetArg(int32_t, direction_flags) uint32_t context_id = 0;
    uint32_t max_bytes = 0;
    xfer_buffer_borrow_t borrow;
    xfer_buffer_dma_mapping_t mapping;

    if (borrow_id <= 0 || offset < 0 || length <= 0 || direction_flags <= 0) {
        m3ApiReturn(WASMOS_ERR_DMA_INVALID);
    }
    if (current_process_context(&context_id) != 0 || require_dma_capability(context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_DMA_DENY);
    }
    /* Resolve the caller's borrow handle; get_borrowed enforces the caller is
     * the borrower, and dma_map_borrow enforces direction ⊆ borrow rights. */
    if (xfer_buffer_get_borrowed((uint32_t)borrow_id, context_id, &borrow, 0) != WASMOS_ERR_NONE) {
        m3ApiReturn(WASMOS_ERR_DMA_DENY);
    }
    if (!capability_dma_direction_allowed(context_id, (uint32_t)direction_flags)) {
        m3ApiReturn(WASMOS_ERR_DMA_DENY);
    }
    max_bytes = capability_dma_max_bytes(context_id);
    if (max_bytes == 0 || (uint32_t)length > max_bytes) {
        m3ApiReturn(WASMOS_ERR_DMA_RANGE);
    }
    if (xfer_buffer_dma_map_borrow(&borrow, (uint32_t)offset, (uint32_t)length,
                                   (uint32_t)direction_flags, &mapping) != WASMOS_ERR_NONE) {
        m3ApiReturn(WASMOS_ERR_DMA_DENY);
    }
    if (!capability_dma_range_allowed(context_id, mapping.device_addr,
                                      (uint64_t)(uint32_t)length)) {
        (void)xfer_buffer_dma_unmap(&mapping);
        m3ApiReturn(WASMOS_ERR_DMA_RANGE);
    }
    if (hostcall_value_check(mapping.device_addr) != WASMOS_OK) {
        (void)xfer_buffer_dma_unmap(&mapping);
        m3ApiReturn(WASMOS_ERR_DMA_UNAVAILABLE);
    }
    m3ApiReturn((int32_t)mapping.device_addr);
}

m3ApiRawFunction(wasmos_dma_sync_borrow) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, borrow_id) m3ApiGetArg(int32_t, offset)
        m3ApiGetArg(int32_t, length) m3ApiGetArg(int32_t, sync_op) uint32_t context_id = 0;
    xfer_buffer_borrow_t borrow;
    xfer_buffer_dma_mapping_t mapping;

    if (borrow_id <= 0 || offset < 0 || length <= 0 ||
        (sync_op != WASMOS_DMA_SYNC_TO_DEVICE && sync_op != WASMOS_DMA_SYNC_FROM_DEVICE &&
         sync_op != WASMOS_DMA_SYNC_BIDIR)) {
        m3ApiReturn(WASMOS_ERR_DMA_INVALID);
    }
    if (current_process_context(&context_id) != 0 || require_dma_capability(context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_DMA_DENY);
    }
    if (xfer_buffer_get_borrowed((uint32_t)borrow_id, context_id, &borrow, &mapping) !=
        WASMOS_ERR_NONE) {
        m3ApiReturn(WASMOS_ERR_DMA_DENY);
    }
    if (xfer_buffer_dma_sync(&mapping, (uint32_t)offset, (uint32_t)length) != WASMOS_ERR_NONE) {
        m3ApiReturn(WASMOS_ERR_DMA_DENY);
    }
    m3ApiReturn(WASMOS_ERR_NONE);
}

m3ApiRawFunction(wasmos_dma_unmap_borrow) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, borrow_id) uint32_t context_id = 0;
    xfer_buffer_borrow_t borrow;
    xfer_buffer_dma_mapping_t mapping;

    if (borrow_id <= 0) {
        m3ApiReturn(WASMOS_ERR_DMA_INVALID);
    }
    if (current_process_context(&context_id) != 0 || require_dma_capability(context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_DMA_DENY);
    }
    if (xfer_buffer_get_borrowed((uint32_t)borrow_id, context_id, &borrow, &mapping) !=
        WASMOS_ERR_NONE) {
        m3ApiReturn(WASMOS_ERR_DMA_DENY);
    }
    if (xfer_buffer_dma_unmap(&mapping) != WASMOS_ERR_NONE) {
        m3ApiReturn(WASMOS_ERR_DMA_DENY);
    }
    m3ApiReturn(WASMOS_ERR_NONE);
}

m3ApiRawFunction(wasmos_xfer_buffer_acquire) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, minimum_size)
        m3ApiReturn(wasm_buffer_acquire_impl((int32_t)BUFFER_KIND_TRANSFER, minimum_size));
}

m3ApiRawFunction(wasmos_spawn_info_buffer) {
    m3ApiReturnType(int32_t) m3ApiReturn(wasm_spawn_info_buffer_impl());
}

m3ApiRawFunction(wasmos_xfer_buffer_borrow) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, grantee_endpoint) m3ApiGetArg(int32_t, buffer_id)
        m3ApiGetArg(int32_t, flags) m3ApiReturn(wasm_buffer_borrow_impl(
            (int32_t)BUFFER_KIND_TRANSFER, grantee_endpoint, buffer_id, flags));
}

m3ApiRawFunction(wasmos_xfer_buffer_reborrow) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, grantee_endpoint) m3ApiGetArg(int32_t, borrow_id)
        m3ApiGetArg(int32_t, flags) m3ApiReturn(wasm_buffer_reborrow_impl(
            (int32_t)BUFFER_KIND_TRANSFER, grantee_endpoint, borrow_id, flags));
}

m3ApiRawFunction(wasmos_xfer_buffer_release) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, buffer_id)
        m3ApiReturn(wasm_buffer_release_impl((int32_t)BUFFER_KIND_TRANSFER, buffer_id));
}

m3ApiRawFunction(wasmos_xfer_buffer_unborrow) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, borrow_id)
        m3ApiReturn(wasm_buffer_unborrow_impl(borrow_id));
}

m3ApiRawFunction(wasmos_buffer_acquire) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, kind) m3ApiGetArg(int32_t, minimum_size)
        m3ApiReturn(wasm_buffer_acquire_impl(kind, minimum_size));
}

m3ApiRawFunction(wasmos_buffer_borrow) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, kind) m3ApiGetArg(int32_t, grantee_endpoint)
        m3ApiGetArg(int32_t, buffer_id) m3ApiGetArg(int32_t, flags)
            m3ApiReturn(wasm_buffer_borrow_impl(kind, grantee_endpoint, buffer_id, flags));
}

m3ApiRawFunction(wasmos_buffer_reborrow) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, kind) m3ApiGetArg(int32_t, grantee_endpoint)
        m3ApiGetArg(int32_t, borrow_id) m3ApiGetArg(int32_t, flags)
            m3ApiReturn(wasm_buffer_reborrow_impl(kind, grantee_endpoint, borrow_id, flags));
}

m3ApiRawFunction(wasmos_buffer_release) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, kind) m3ApiGetArg(int32_t, buffer_id)
        m3ApiReturn(wasm_buffer_release_impl(kind, buffer_id));
}

m3ApiRawFunction(wasmos_buffer_unborrow) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, borrow_id)
        m3ApiReturn(wasm_buffer_unborrow_impl(borrow_id));
}

static wasmos_error_code_t wasm_block_buffer_validate_args(int32_t phys, int32_t len,
                                                           int32_t offset) {
    /* phys must name a live block buffer, and the access must stay within that
     * buffer's fixed 8 KiB window.  Without this a guest could pass an arbitrary
     * phys and read/write any physical address below 4 GiB. */
    if (!wasm_block_slot_phys_is_live((uint64_t)(uint32_t)phys)) {
        return WASMOS_ERR_BLOCK_NO_SLOT;
    }
    /* The guest's int32 arguments are zero-extended, so a negative length
     * arrives as a large positive one and the range test refuses it. */
    return block_buffer_check_range((uint64_t)(uint32_t)offset, (uint64_t)(uint32_t)len,
                                    (uint64_t)WASM_BLOCK_BUFFER_SIZE_BYTES);
}

m3ApiRawFunction(wasmos_block_buffer_phys) {
    m3ApiReturnType(int32_t) uint32_t pid = process_current_pid();
    wasm_block_slot_t* slot = wasm_block_slot_for_pid(pid);

    if (!slot) {
        m3ApiReturn(WASMOS_ERR_BLOCK_NO_SLOT);
    }
    if (slot->buffer_phys == 0) {
        /* Below 2 GiB, not 4: the address is returned on the same i32 that
         * carries the error codes, so bit 31 must stay clear. */
        uint64_t phys = pfa_alloc_pages_below(WASM_BLOCK_BUFFER_PAGES, BLOCK_BUFFER_PHYS_LIMIT);
        if (!phys) {
            m3ApiReturn(WASMOS_ERR_BLOCK_NO_BACKING);
        }
        slot->buffer_phys = phys;
    }

    wasmos_error_code_t phys_rc = block_buffer_check_phys(slot->buffer_phys);
    if (phys_rc != WASMOS_OK) {
        m3ApiReturn(phys_rc);
    }
    m3ApiReturn((int32_t)slot->buffer_phys);
}

m3ApiRawFunction(wasmos_block_buffer_copy) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, phys) m3ApiGetArgMem(uint8_t*, ptr)
        m3ApiGetArg(int32_t, len) m3ApiGetArg(int32_t, offset)

            wasmos_error_code_t rc = wasm_block_buffer_validate_args(phys, len, offset);
    if (rc != WASMOS_OK) {
        m3ApiReturn(rc);
    }
    m3ApiCheckMem(ptr, (uint32_t)len);
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), ptr, (uint32_t)len,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(proc->context_id, ptr_user, (uint64_t)(uint32_t)len,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }

    const uint8_t* src = ptr_cast(uint8_t, ((uint32_t)phys + (uint32_t)offset));
    if (wasm_copy_to_user_bytes(proc->context_id, ptr_user, src, (uint32_t)len) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_block_buffer_write) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, phys) m3ApiGetArgMem(const uint8_t*, ptr)
        m3ApiGetArg(int32_t, len) m3ApiGetArg(int32_t, offset)

            wasmos_error_code_t rc = wasm_block_buffer_validate_args(phys, len, offset);
    if (rc != WASMOS_OK) {
        m3ApiReturn(rc);
    }
    m3ApiCheckMem(ptr, (uint32_t)len);
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), ptr, (uint32_t)len,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(proc->context_id, ptr_user, (uint64_t)(uint32_t)len,
                                MEM_REGION_FLAG_READ) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }

    uint8_t* dst = ptr_cast(uint8_t, ((uint32_t)phys + (uint32_t)offset));
    uint32_t copied = 0;
    uint8_t bounce[256];
    while (copied < (uint32_t)len) {
        uint32_t chunk = (uint32_t)len - copied;
        if (chunk > (uint32_t)sizeof(bounce)) {
            chunk = (uint32_t)sizeof(bounce);
        }
        if (wasm_copy_from_user_bytes(proc->context_id, ptr_user + (uint64_t)copied, bounce,
                                      chunk) != 0) {
            m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
        }
        for (uint32_t i = 0; i < chunk; ++i) {
            dst[copied + i] = bounce[i];
        }
        copied += chunk;
    }
    m3ApiReturn(0);
}

/* Overlay the caller's own 8 KiB block buffer into its linear-memory window so
 * the owner reads/writes block data in place instead of copying through
 * block_buffer_copy/write.  Idempotent: repeated calls return the same offset.
 * The physical pages are owned by the block slot and freed by wasm3_release_pid;
 * the overlay itself is torn down with the address space, so it is tracked with
 * a zero phys_base/pages to reserve the linmem window without a second free. */
m3ApiRawFunction(wasmos_block_buffer_map) {
    m3ApiReturnType(int32_t)

        process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    wasm_block_slot_t* slot = wasm_block_slot_for_pid(proc->pid);
    if (!slot) {
        m3ApiReturn(WASMOS_ERR_BLOCK_NO_SLOT);
    }
    if (slot->buffer_phys == 0) {
        uint64_t phys = pfa_alloc_pages_below(WASM_BLOCK_BUFFER_PAGES, BLOCK_BUFFER_PHYS_LIMIT);
        if (!phys) {
            m3ApiReturn(WASMOS_ERR_BLOCK_NO_BACKING);
        }
        if (block_buffer_check_phys(phys) != WASMOS_OK) {
            pfa_free_pages(phys, WASM_BLOCK_BUFFER_PAGES);
            m3ApiReturn(WASMOS_ERR_BLOCK_ABOVE_4G);
        }
        slot->buffer_phys = phys;
    }
    if (slot->map_offset != 0) {
        m3ApiReturn((int32_t)slot->map_offset);
    }

    uint32_t mem_size = 0;
    uint8_t* mem_base = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem_base || mem_size == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_WINDOW);
    }

    const uint64_t region_bytes = (uint64_t)WASM_BLOCK_BUFFER_SIZE_BYTES;
    uint64_t mem_size64 = (uint64_t)mem_size;
    uint64_t off64 = 0;
    uint8_t found = 0;

    for (off64 = 0x200000ULL; off64 + region_bytes <= mem_size64; off64 += 0x1000ULL) {
        uint64_t probe_virt = 0;
        if (wasm_linear_window_overlaps(proc->pid, (uint32_t)off64, (uint32_t)region_bytes)) {
            continue;
        }
        if (wasm_user_va_from_offset(proc->context_id, (uint32_t)off64, (uint32_t)region_bytes,
                                     &probe_virt) != 0) {
            continue;
        }
        if (mm_user_range_permitted(proc->context_id, probe_virt, region_bytes,
                                    MEM_REGION_FLAG_WRITE) != 0) {
            continue;
        }
        if ((probe_virt & 0xFFFULL) != 0) {
            continue;
        }
        found = 1;
        break;
    }

    if (!found) {
        off64 = (mem_size64 + 0xFFFULL) & ~0xFFFULL;
        uint64_t required = off64 + region_bytes;
        if (required > mem_size64) {
            uint32_t target_pages = (uint32_t)((required + 0xFFFFULL) >> 16);
            if (ResizeMemory(runtime, target_pages) != m3Err_none) {
                m3ApiReturn(WASMOS_ERR_KERNEL_NO_WINDOW);
            }
            mem_size64 = (uint64_t)m3_GetMemorySize(runtime);
            if (required > mem_size64) {
                m3ApiReturn(WASMOS_ERR_KERNEL_NO_WINDOW);
            }
        }
    }

    uint32_t off32 = (uint32_t)off64;
    uint64_t virt = 0;
    if (wasm_user_va_from_offset(proc->context_id, off32, (uint32_t)region_bytes, &virt) != 0 ||
        mm_user_range_permitted(proc->context_id, virt, region_bytes, MEM_REGION_FLAG_WRITE) != 0 ||
        (virt & 0xFFFULL) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_WINDOW);
    }
    if (mm_context_map_physical(proc->context_id, virt, slot->buffer_phys, region_bytes,
                                MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE |
                                    MEM_REGION_FLAG_USER) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_MAP_FAILED);
    }

    wasm_dma_region_map_track(proc->pid, off32, (uint32_t)region_bytes, 0, 0);
    slot->map_offset = off32;
    m3ApiReturn((int32_t)off32);
}

/* Synthetic tracking-id namespace for xfer-buffer linmem overlays (bit 30),
 * disjoint from real shmem ids and the region/DMA windows, so they share the
 * overlap-tracking table without colliding. Mirrors WARP_XFER_TRACK_ID. */
#define WASM_XFER_TRACK_ID(id) ((uint32_t)(id) | 0x40000000u)

/* Overlay an OWNED xfer-buffer's backing into the caller's WASM linear memory,
 * mirroring wasmos_block_buffer_map but resolving the phys/size from the
 * xfer-buffer object. Zero-copy socket-ring data plane (docs/architecture/22):
 * the app drives its TX/RX rings with ringbuf.h at the returned offset instead
 * of copy hostcalls. Owner-only; idempotent per buffer_id. The pages are owned
 * by the object (freed on release/reap) so they are NOT pinned here. */
m3ApiRawFunction(wasmos_xfer_buffer_map) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, buffer_id)

        process_t* proc = process_get(process_current_pid());
    xfer_buffer_t desc = {0};
    xfer_buffer_owner_t owner = {0};
    uint64_t phys = 0;
    if (buffer_id <= 0 || !proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT);
    }
    if (xfer_buffer_describe((uint32_t)buffer_id, BUFFER_KIND_TRANSFER, proc->context_id, &desc) !=
            WASMOS_ERR_NONE ||
        xfer_buffer_get_owned(&desc, proc->context_id, &owner) != WASMOS_ERR_NONE) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_INVALID_KIND); /* owner-only: the overlay is the owner's
                                                             private view */
    }
    phys = xfer_buffer_object_phys(&desc);
    if (phys == 0 || (phys & 0xFFFULL) != 0 || desc.size_bytes == 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_NO_BACKING);
    }
    uint32_t track_id = WASM_XFER_TRACK_ID((uint32_t)buffer_id);
    for (uint32_t i = 0; i < WASM_SHMEM_MAP_SLOTS; ++i) {
        if (g_wasm_shmem_maps[i].valid && g_wasm_shmem_maps[i].pid == proc->pid &&
            g_wasm_shmem_maps[i].shmem_id == track_id) {
            m3ApiReturn((int32_t)g_wasm_shmem_maps[i].offset); /* idempotent */
        }
    }

    uint32_t mem_size = 0;
    uint8_t* mem_base = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem_base || mem_size == 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_NO_BACKING);
    }
    const uint64_t region_bytes = ((uint64_t)desc.size_bytes + 0xFFFULL) & ~0xFFFULL;
    uint64_t mem_size64 = (uint64_t)mem_size;
    uint64_t off64 = 0;
    uint8_t found = 0;
    for (off64 = 0x200000ULL; off64 + region_bytes <= mem_size64; off64 += 0x1000ULL) {
        uint64_t probe_virt = 0;
        if (wasm_linear_window_overlaps(proc->pid, (uint32_t)off64, (uint32_t)region_bytes)) {
            continue;
        }
        if (wasm_user_va_from_offset(proc->context_id, (uint32_t)off64, (uint32_t)region_bytes,
                                     &probe_virt) != 0) {
            continue;
        }
        if (mm_user_range_permitted(proc->context_id, probe_virt, region_bytes,
                                    MEM_REGION_FLAG_WRITE) != 0) {
            continue;
        }
        if ((probe_virt & 0xFFFULL) != 0) {
            continue;
        }
        found = 1;
        break;
    }
    if (!found) {
        off64 = (mem_size64 + 0xFFFULL) & ~0xFFFULL;
        uint64_t required = off64 + region_bytes;
        if (required > mem_size64) {
            uint32_t target_pages = (uint32_t)((required + 0xFFFFULL) >> 16);
            if (ResizeMemory(runtime, target_pages) != m3Err_none) {
                m3ApiReturn(WASMOS_ERR_XFER_BUFFER_CAPACITY_EXCEEDED);
            }
            mem_size64 = (uint64_t)m3_GetMemorySize(runtime);
            if (required > mem_size64) {
                m3ApiReturn(WASMOS_ERR_XFER_BUFFER_CAPACITY_EXCEEDED);
            }
        }
    }

    uint32_t off32 = (uint32_t)off64;
    uint64_t virt = 0;
    if (wasm_user_va_from_offset(proc->context_id, off32, (uint32_t)region_bytes, &virt) != 0 ||
        mm_user_range_permitted(proc->context_id, virt, region_bytes, MEM_REGION_FLAG_WRITE) != 0 ||
        (virt & 0xFFFULL) != 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_INTERNAL);
    }
    if (mm_context_map_physical(proc->context_id, virt, phys, region_bytes,
                                MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE |
                                    MEM_REGION_FLAG_USER) != 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_INTERNAL);
    }
    wasm_shmem_map_track(proc->pid, track_id, off32, (uint32_t)region_bytes);
    m3ApiReturn((int32_t)off32);
}

/* Untrack an xfer-buffer overlay window. Like wasmos_shmem_unmap this does not
 * yet restore the previous linear-memory page mappings (same FIXME); the object
 * backing is reclaimed on process reap. Callers must therefore keep the buffer
 * alive (do not release it) until process exit. Returns 0. */
m3ApiRawFunction(wasmos_xfer_buffer_unmap) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, buffer_id)

        process_t* proc = process_get(process_current_pid());
    if (buffer_id <= 0 || !proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT);
    }
    /* FIXME(xfer-unmap): mirror wasmos_shmem_unmap — restore the prior PTEs. */
    wasm_shmem_map_untrack(proc->pid, WASM_XFER_TRACK_ID((uint32_t)buffer_id));
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_xfer_buffer_size) {
    m3ApiReturnType(int32_t) m3ApiReturn((int32_t)xfer_buffer_size(BUFFER_KIND_TRANSFER));
}

m3ApiRawFunction(wasmos_fs_endpoint) {
    m3ApiReturnType(int32_t) uint32_t endpoint = process_manager_fs_endpoint();
    if (endpoint == IPC_ENDPOINT_NONE) {
        m3ApiReturn(WASMOS_NOENT); /* no FS service has registered yet */
    }
    m3ApiReturn((int32_t)endpoint);
}

m3ApiRawFunction(wasmos_xfer_buffer_read) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, buffer_id) m3ApiGetArgMem(uint8_t*, ptr)
        m3ApiGetArg(int32_t, len) m3ApiGetArg(int32_t, offset) uint32_t context_id = 0;
    xfer_buffer_t desc = {0};
    uint64_t phys = 0;
    int rc = 0;

    if (buffer_id <= 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_NOT_FOUND);
    }
    /* A zero-length transfer is a no-op success, matching WARP. A NEGATIVE
     * length is still refused: it cannot be a no-op, it is a bad argument. */
    if (len == 0) {
        m3ApiReturn(WASMOS_ERR_NONE);
    }
    if (len < 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_INVALID_SIZE);
    }
    if (offset < 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_RANGE);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT);
    }
    /* Look up the object the caller named; describe confirms the caller owns or
     * borrows it, and can_access confirms the READ right for this operation. */
    rc = xfer_buffer_describe((uint32_t)buffer_id, BUFFER_KIND_TRANSFER, context_id, &desc);
    if (rc != WASMOS_ERR_NONE) {
        m3ApiReturn(rc);
    }
    if (!xfer_buffer_can_access(&desc, context_id, BUFFER_BORROW_READ)) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_NO_ACCESS);
    }
    if ((uint32_t)offset + (uint32_t)len > desc.size_bytes) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_RANGE);
    }
    m3ApiCheckMem(ptr, (uint32_t)len);
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), ptr, (uint32_t)len,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(context_id, ptr_user, (uint64_t)(uint32_t)len,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_RANGE);
    }

    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT);
    }
    phys = xfer_buffer_object_phys(&desc);
    if (phys == 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_NOT_FOUND);
    }
    const uint8_t* src = ptr_cast(uint8_t, (phys | KERNEL_HIGHER_HALF_BASE));
    if (wasm_copy_to_user_bytes(proc->context_id, ptr_user, src + offset, (uint32_t)len) != 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_RANGE);
    }
    m3ApiReturn(WASMOS_ERR_NONE);
}

m3ApiRawFunction(wasmos_xfer_buffer_write) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, buffer_id) m3ApiGetArgMem(const uint8_t*, ptr)
        m3ApiGetArg(int32_t, len) m3ApiGetArg(int32_t, offset) uint32_t context_id = 0;
    xfer_buffer_t desc = {0};
    uint64_t phys = 0;
    int rc = 0;

    if (buffer_id <= 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_NOT_FOUND);
    }
    /* A zero-length transfer is a no-op success, matching WARP. A NEGATIVE
     * length is still refused: it cannot be a no-op, it is a bad argument. */
    if (len == 0) {
        m3ApiReturn(WASMOS_ERR_NONE);
    }
    if (len < 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_INVALID_SIZE);
    }
    if (offset < 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_RANGE);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT);
    }
    rc = xfer_buffer_describe((uint32_t)buffer_id, BUFFER_KIND_TRANSFER, context_id, &desc);
    if (rc != WASMOS_ERR_NONE) {
        m3ApiReturn(rc);
    }
    if (!xfer_buffer_can_access(&desc, context_id, BUFFER_BORROW_WRITE)) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_NO_ACCESS);
    }
    if ((uint32_t)offset + (uint32_t)len > desc.size_bytes) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_RANGE);
    }
    m3ApiCheckMem(ptr, (uint32_t)len);
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), ptr, (uint32_t)len,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(context_id, ptr_user, (uint64_t)(uint32_t)len,
                                MEM_REGION_FLAG_READ) != 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_RANGE);
    }

    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT);
    }
    phys = xfer_buffer_object_phys(&desc);
    if (phys == 0) {
        m3ApiReturn(WASMOS_ERR_XFER_BUFFER_NOT_FOUND);
    }
    uint8_t* dst = ptr_cast(uint8_t, (phys | KERNEL_HIGHER_HALF_BASE));
    uint32_t copied = 0;
    uint8_t bounce[256];
    while (copied < (uint32_t)len) {
        uint32_t chunk = (uint32_t)len - copied;
        if (chunk > (uint32_t)sizeof(bounce)) {
            chunk = (uint32_t)sizeof(bounce);
        }
        if (wasm_copy_from_user_bytes(proc->context_id, ptr_user + (uint64_t)copied, bounce,
                                      chunk) != 0) {
            m3ApiReturn(WASMOS_ERR_XFER_BUFFER_RANGE);
        }
        for (uint32_t i = 0; i < chunk; ++i) {
            dst[(uint32_t)offset + copied + i] = bounce[i];
        }
        copied += chunk;
    }
    m3ApiReturn(WASMOS_ERR_NONE);
}

m3ApiRawFunction(wasmos_early_log_size) {
    m3ApiReturnType(int32_t) m3ApiReturn((int32_t)serial_early_log_size());
}

m3ApiRawFunction(wasmos_early_log_copy) {
    m3ApiReturnType(int32_t) m3ApiGetArgMem(uint8_t*, ptr) m3ApiGetArg(int32_t, len)
        m3ApiGetArg(int32_t, offset)

            if (len < 0 || offset < 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    uint32_t total = serial_early_log_size();
    uint32_t start = (uint32_t)offset;
    uint32_t count = (uint32_t)len;
    if (start > total || count > total - start) {
        m3ApiReturn(WASMOS_INVAL);
    }
    if (count == 0) {
        m3ApiReturn(0);
    }
    m3ApiCheckMem(ptr, count);
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), ptr, count,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(proc->context_id, ptr_user, count, MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }
    uint32_t copied = 0;
    uint8_t bounce[256];
    while (copied < count) {
        uint32_t chunk = count - copied;
        if (chunk > (uint32_t)sizeof(bounce)) {
            chunk = (uint32_t)sizeof(bounce);
        }
        serial_early_log_copy(bounce, start + copied, chunk);
        if (mm_copy_to_user(proc->context_id, ptr_user + (uint64_t)copied, bounce,
                            (uint64_t)chunk) != 0) {
            m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
        }
        copied += chunk;
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_boot_config_size) {
    m3ApiReturnType(int32_t) if (!g_wasm_boot_info || !g_wasm_boot_info->boot_config ||
                                 g_wasm_boot_info->boot_config_size == 0) {
        m3ApiReturn(WASMOS_NOENT);
    }
    m3ApiReturn((int32_t)g_wasm_boot_info->boot_config_size);
}

m3ApiRawFunction(wasmos_boot_config_copy) {
    m3ApiReturnType(int32_t) m3ApiGetArgMem(uint8_t*, ptr) m3ApiGetArg(int32_t, len)
        m3ApiGetArg(int32_t, offset)

            if (len < 0 || offset < 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    if (!g_wasm_boot_info || !g_wasm_boot_info->boot_config ||
        g_wasm_boot_info->boot_config_size == 0) {
        m3ApiReturn(WASMOS_NOENT);
    }

    uint32_t total = g_wasm_boot_info->boot_config_size;
    uint32_t start = (uint32_t)offset;
    uint32_t count = (uint32_t)len;
    if (start > total || count > total - start) {
        m3ApiReturn(WASMOS_INVAL);
    }
    if (count == 0) {
        m3ApiReturn(0);
    }

    m3ApiCheckMem(ptr, count);
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), ptr, count,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(proc->context_id, ptr_user, count, MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }
    const uint8_t* src = ptr_cast(uint8_t, g_wasm_boot_info->boot_config);
    if (wasm_copy_to_user_bytes(proc->context_id, ptr_user, src + start, count) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_env_get) {
    m3ApiReturnType(int32_t) m3ApiGetArgMem(const char*, name_ptr) m3ApiGetArg(int32_t, name_len)
        m3ApiGetArgMem(char*, buf_ptr) m3ApiGetArg(int32_t, buf_len)

            if (name_len <= 0 || buf_len <= 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    m3ApiCheckMem(name_ptr, (uint32_t)name_len);
    m3ApiCheckMem(buf_ptr, (uint32_t)buf_len);
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t name_user = 0;
    uint64_t buf_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), name_ptr,
                                   (uint32_t)name_len, &name_user) != 0 ||
        mm_user_range_permitted(proc->context_id, name_user, (uint64_t)(uint32_t)name_len,
                                MEM_REGION_FLAG_READ) != 0 ||
        wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), buf_ptr, (uint32_t)buf_len,
                                   &buf_user) != 0 ||
        mm_user_range_permitted(proc->context_id, buf_user, (uint64_t)(uint32_t)buf_len,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }
    /* Refused rather than truncated: see kenv_get. */
    if ((uint32_t)name_len >= KENV_KEY_MAX) {
        m3ApiReturn(WASMOS_ERR_ENV_TOO_LONG);
    }
    char local_name[KENV_KEY_MAX];
    if (mm_copy_from_user(proc->context_id, local_name, name_user, (uint64_t)(uint32_t)name_len) !=
        0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    local_name[(uint32_t)name_len] = '\0';

    char local_val[KENV_VAL_MAX];
    uint32_t out_size = (uint32_t)buf_len < KENV_VAL_MAX ? (uint32_t)buf_len : KENV_VAL_MAX;
    uint32_t write_len = 0;
    wasmos_error_code_t rc = kenv_get(local_name, local_val, out_size, &write_len);
    if (rc != WASMOS_OK) {
        m3ApiReturn(rc);
    }
    /* write_len + 1 for the NUL, which kenv_get has already placed. */
    if (wasm_copy_to_user_bytes(proc->context_id, buf_user, local_val, write_len + 1u) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    m3ApiReturn((int32_t)write_len);
}

m3ApiRawFunction(wasmos_env_set) {
    m3ApiReturnType(int32_t) m3ApiGetArgMem(const char*, name_ptr) m3ApiGetArg(int32_t, name_len)
        m3ApiGetArgMem(const char*, val_ptr) m3ApiGetArg(int32_t, val_len)

            if (name_len <= 0 || val_len < 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    if ((uint32_t)name_len >= KENV_KEY_MAX || (uint32_t)val_len >= KENV_VAL_MAX) {
        m3ApiReturn(WASMOS_ERR_ENV_TOO_LONG);
    }
    m3ApiCheckMem(name_ptr, (uint32_t)name_len);
    if (val_len > 0) {
        m3ApiCheckMem(val_ptr, (uint32_t)val_len);
    }
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t name_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), name_ptr,
                                   (uint32_t)name_len, &name_user) != 0 ||
        mm_user_range_permitted(proc->context_id, name_user, (uint64_t)(uint32_t)name_len,
                                MEM_REGION_FLAG_READ) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }
    char local_name[KENV_KEY_MAX];
    if (mm_copy_from_user(proc->context_id, local_name, name_user, (uint64_t)(uint32_t)name_len) !=
        0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    local_name[name_len] = '\0';
    char local_val[KENV_VAL_MAX];
    local_val[0] = '\0';
    if (val_len > 0) {
        uint64_t val_user = 0;
        if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                       (uint64_t)m3_GetMemorySize(runtime), val_ptr,
                                       (uint32_t)val_len, &val_user) != 0 ||
            mm_user_range_permitted(proc->context_id, val_user, (uint64_t)(uint32_t)val_len,
                                    MEM_REGION_FLAG_READ) != 0) {
            m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
        }
        if (mm_copy_from_user(proc->context_id, local_val, val_user, (uint64_t)(uint32_t)val_len) !=
            0) {
            m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
        }
        local_val[val_len] = '\0';
    }
    m3ApiReturn(kenv_set(local_name, local_val));
}

m3ApiRawFunction(wasmos_env_unset) {
    m3ApiReturnType(int32_t) m3ApiGetArgMem(const char*, name_ptr) m3ApiGetArg(int32_t, name_len)

        if (name_len <= 0 || name_len >= KENV_KEY_MAX) {
        m3ApiReturn(0);
    }
    m3ApiCheckMem(name_ptr, (uint32_t)name_len);
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(0);
    }
    uint64_t name_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), name_ptr,
                                   (uint32_t)name_len, &name_user) != 0 ||
        mm_user_range_permitted(proc->context_id, name_user, (uint64_t)(uint32_t)name_len,
                                MEM_REGION_FLAG_READ) != 0) {
        m3ApiReturn(0);
    }
    char local_name[KENV_KEY_MAX];
    if (mm_copy_from_user(proc->context_id, local_name, name_user, (uint64_t)(uint32_t)name_len) !=
        0) {
        m3ApiReturn(0);
    }
    local_name[name_len] = '\0';
    m3ApiReturn(kenv_unset(local_name));
}

/* Region-addressed I/O. The driver supplies (region, offset), never an absolute
 * port, so it cannot express an access outside the window its spawn profile
 * granted -- the kernel owns the base. `region` indexes those windows in
 * declaration order. Failures propagate the specific WASMOS_ERR_IO_* reason
 * rather than collapsing to one value: "no such region" and "offset past the
 * end" are different bugs. */
static int io_region_port(uint32_t region, uint32_t offset, uint32_t width, uint16_t* out_port) {
    uint32_t context_id = 0;
    if (current_process_context(&context_id) != 0) {
        return WASMOS_ERR_IO_NOT_AUTHORIZED;
    }
    return capability_io_region_port(context_id, region, offset, width, out_port);
}

/* Reads return the datum through linear memory, not as the result: a 32-bit port
 * read can legitimately be 0xFFFFFFFF and must stay distinguishable from a
 * failure code. */
#define WASMOS_IO_REGION_READ(width, width_expr)                                                   \
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, region) m3ApiGetArg(int32_t, offset)             \
        m3ApiGetArgMem(uint32_t*, out) m3ApiCheckMem(out, sizeof(uint32_t));                       \
    uint16_t port = 0;                                                                             \
    int rc = 0;                                                                                    \
    if (region < 0 || offset < 0) {                                                                \
        m3ApiReturn(WASMOS_ERR_IO_BAD_REGION);                                                     \
    }                                                                                              \
    rc = io_region_port((uint32_t)region, (uint32_t)offset, (width), &port);                       \
    if (rc != 0) {                                                                                 \
        m3ApiReturn(rc);                                                                           \
    }                                                                                              \
    *out = (width_expr);                                                                           \
    m3ApiReturn(0)

m3ApiRawFunction(wasmos_io_region_in8) {
    WASMOS_IO_REGION_READ(1u, (uint32_t)inb(port));
}

m3ApiRawFunction(wasmos_io_region_in16) {
    WASMOS_IO_REGION_READ(2u, (uint32_t)inw(port));
}

m3ApiRawFunction(wasmos_io_region_in32) {
    WASMOS_IO_REGION_READ(4u, (uint32_t)inl(port));
}

#define WASMOS_IO_REGION_WRITE(width, write_stmt)                                                  \
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, region) m3ApiGetArg(int32_t, offset)             \
        m3ApiGetArg(int32_t, value);                                                               \
    uint16_t port = 0;                                                                             \
    int rc = 0;                                                                                    \
    if (region < 0 || offset < 0) {                                                                \
        m3ApiReturn(WASMOS_ERR_IO_BAD_REGION);                                                     \
    }                                                                                              \
    rc = io_region_port((uint32_t)region, (uint32_t)offset, (width), &port);                       \
    if (rc != 0) {                                                                                 \
        m3ApiReturn(rc);                                                                           \
    }                                                                                              \
    write_stmt;                                                                                    \
    m3ApiReturn(0)

m3ApiRawFunction(wasmos_io_region_out8) {
    WASMOS_IO_REGION_WRITE(1u, outb(port, (uint8_t)((uint32_t)value & 0xFFu)));
}

m3ApiRawFunction(wasmos_io_region_out16) {
    WASMOS_IO_REGION_WRITE(2u, outw(port, (uint16_t)((uint32_t)value & 0xFFFFu)));
}

m3ApiRawFunction(wasmos_io_region_out32) {
    WASMOS_IO_REGION_WRITE(4u, outl(port, (uint32_t)value));
}

/* The whole port-read family reports its value through `out` and its outcome
 * through the return. For in32 the two cannot share one signed i32 at all, since
 * a read uses the full range. For in8 and in16 the value could not collide with
 * a code, but no caller ever read the sign -- each masked it off -- so a denied
 * capability arrived as 0xFF / 0xFFFF, which is what an absent device reads. */
m3ApiRawFunction(wasmos_io_in8) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, port) m3ApiGetArgMem(uint8_t*, out)
        m3ApiCheckMem(out, sizeof(uint8_t));
    uint32_t context_id = 0;
    if (port < 0 || port > 0xFFFF) {
        m3ApiReturn(WASMOS_ERR_IO_BAD_PORT);
    }
    if (current_process_context(&context_id) != 0 ||
        require_io_capability(context_id, (uint16_t)port) != 0) {
        m3ApiReturn(WASMOS_ERR_IO_NOT_AUTHORIZED);
    }
    *out = inb((uint16_t)port);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_io_in16) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, port) m3ApiGetArgMem(uint16_t*, out)
        m3ApiCheckMem(out, sizeof(uint16_t));
    uint32_t context_id = 0;
    if (port < 0 || port > 0xFFFF) {
        m3ApiReturn(WASMOS_ERR_IO_BAD_PORT);
    }
    if (current_process_context(&context_id) != 0 ||
        require_io_capability(context_id, (uint16_t)port) != 0) {
        m3ApiReturn(WASMOS_ERR_IO_NOT_AUTHORIZED);
    }
    *out = inw((uint16_t)port);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_io_in32) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, port) m3ApiGetArgMem(uint32_t*, out)
        m3ApiCheckMem(out, sizeof(uint32_t));
    uint32_t context_id = 0;
    if (port < 0 || port > 0xFFFF) {
        m3ApiReturn(WASMOS_ERR_IO_BAD_PORT);
    }
    if (current_process_context(&context_id) != 0 ||
        require_io_capability(context_id, (uint16_t)port) != 0) {
        m3ApiReturn(WASMOS_ERR_IO_NOT_AUTHORIZED);
    }
    *out = (uint32_t)inl((uint16_t)port);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_io_out8) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, port) m3ApiGetArg(int32_t, value)
        uint32_t context_id = 0;
    if (port < 0 || port > 0xFFFF || value < 0 || value > 0xFF) {
        m3ApiReturn(WASMOS_ERR_IO_BAD_PORT);
    }
    if (current_process_context(&context_id) != 0 ||
        require_io_capability(context_id, (uint16_t)port) != 0) {
        m3ApiReturn(WASMOS_ERR_IO_NOT_AUTHORIZED);
    }
    outb((uint16_t)port, (uint8_t)value);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_io_out16) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, port) m3ApiGetArg(int32_t, value)
        uint32_t context_id = 0;
    if (port < 0 || port > 0xFFFF || value < 0 || value > 0xFFFF) {
        m3ApiReturn(WASMOS_ERR_IO_BAD_PORT);
    }
    if (current_process_context(&context_id) != 0 ||
        require_io_capability(context_id, (uint16_t)port) != 0) {
        m3ApiReturn(WASMOS_ERR_IO_NOT_AUTHORIZED);
    }
    outw((uint16_t)port, (uint16_t)value);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_io_out32) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, port) m3ApiGetArg(int32_t, value)
        uint32_t context_id = 0;
    if (port < 0 || port > 0xFFFF) {
        m3ApiReturn(WASMOS_ERR_IO_BAD_PORT);
    }
    if (current_process_context(&context_id) != 0 ||
        require_io_capability(context_id, (uint16_t)port) != 0) {
        m3ApiReturn(WASMOS_ERR_IO_NOT_AUTHORIZED);
    }
    outl((uint16_t)port, (uint32_t)value);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_io_wait) {
    m3ApiReturnType(int32_t) uint32_t context_id = 0;
    if (current_process_context(&context_id) != 0 || require_io_capability(context_id, 0x80) != 0) {
        m3ApiReturn(WASMOS_ERR_IO_NOT_AUTHORIZED);
    }
    io_wait();
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_framebuffer_pixel) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, x) m3ApiGetArg(int32_t, y)
        m3ApiGetArg(int32_t, color)
            m3ApiReturn(framebuffer_put_pixel((uint32_t)x, (uint32_t)y, (uint32_t)color));
}

m3ApiRawFunction(wasmos_framebuffer_info) {
    m3ApiReturnType(int32_t) m3ApiGetArgMem(uint8_t*, out_ptr) m3ApiGetArg(int32_t, len)

        if (len <= 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    if (len < (int32_t)sizeof(framebuffer_info_t)) {
        m3ApiReturn(WASMOS_ERR_FRAMEBUFFER_TOO_SMALL);
    }
    if (!out_ptr) {
        m3ApiReturn(WASMOS_INVAL);
    }
    framebuffer_info_t info = {0};
    wasmos_error_code_t info_rc = framebuffer_get_info(&info);
    if (info_rc != WASMOS_OK) {
        m3ApiReturn(info_rc);
    }
    m3ApiCheckMem(out_ptr, (uint32_t)len);
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t out_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), out_ptr, (uint32_t)len,
                                   &out_user) != 0 ||
        mm_user_range_permitted(proc->context_id, out_user, (uint64_t)(uint32_t)len,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }
    if (mm_copy_to_user(proc->context_id, out_user, &info, sizeof(info)) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_framebuffer_map) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, ptr) m3ApiGetArg(int32_t, size)

        if (ptr < 0 || size <= 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    if ((size & 0xFFF) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_UNALIGNED);
    }

    framebuffer_info_t info = {0};
    wasmos_error_code_t info_rc = framebuffer_get_info(&info);
    if (info_rc != WASMOS_OK) {
        m3ApiReturn(info_rc);
    }
    if ((uint32_t)size < info.framebuffer_size) {
        m3ApiReturn(WASMOS_ERR_FRAMEBUFFER_TOO_SMALL);
    }

    /* Split, so "no caller" and "not permitted to map MMIO" stay distinct --
     * the same conflation the io family had. */
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    if (require_mmio_capability(proc->context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NOT_AUTHORIZED);
    }

    mm_context_t* ctx = mm_context_get(proc->context_id);
    if (!ctx || ctx->root_table == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }

    /* Map the physical framebuffer over caller-provided linear-memory pages.
     * Resolve the WASM offset into the process-owned user VA explicitly. */
    uint32_t off32 = (uint32_t)ptr;
    uint32_t map_size32 = (uint32_t)size;
    if ((uint64_t)off32 + (uint64_t)map_size32 > (uint64_t)m3_GetMemorySize(runtime)) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_WINDOW);
    }
    uint64_t virt = 0;
    int va_rc = wasm_user_va_from_offset(proc->context_id, off32, map_size32, &virt);
    int perm_rc = 0;
    if (va_rc == 0) {
        perm_rc = mm_user_range_permitted(proc->context_id, virt, (uint64_t)map_size32,
                                          MEM_REGION_FLAG_WRITE);
    }
    if (va_rc != 0 || perm_rc != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }
    if ((virt & 0xFFFULL) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_UNALIGNED);
    }

    uint64_t pages = (uint64_t)map_size32 / 0x1000ULL;
    if (pages == 0) {
        klog_write("[framebuffer-map] zero pages\n");
        m3ApiReturn(WASMOS_INVAL);
    }
    uint64_t cur_virt = virt;
    uint64_t cur_phys = info.framebuffer_base;
    for (uint64_t i = 0; i < pages; ++i) {
        (void)paging_unmap_4k_in_root(ctx->root_table, cur_virt);
        if (paging_map_4k_in_root(ctx->root_table, cur_virt, cur_phys,
                                  MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE |
                                      MEM_REGION_FLAG_USER) < 0) {
            m3ApiReturn(WASMOS_ERR_KERNEL_MAP_FAILED);
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
/* Driver-owned DMA region allocation. The region is allocated from low
 * physical memory, mapped into a non-overlapping window in the caller's linear
 * memory, and reclaimed on process reap via wasm3_release_pid(). */
m3ApiRawFunction(wasmos_region_alloc) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, pages) m3ApiGetArg(int32_t, cache_policy)
        m3ApiGetArg(int32_t, out_phys_off)

            if (pages <= 0 || pages > 1024 || out_phys_off < 0) {
        m3ApiReturn(WASMOS_ERR_DMA_INVALID);
    }
    if (cache_policy != WASMOS_REGION_CACHE_WB) {
        m3ApiReturn(WASMOS_ERR_DMA_INVALID);
    }

    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0 || require_dma_capability(proc->context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_DMA_DENY);
    }

    uint32_t mem_size = 0;
    uint8_t* mem_base = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem_base || mem_size == 0 ||
        (uint64_t)(uint32_t)out_phys_off + sizeof(uint64_t) > (uint64_t)mem_size) {
        m3ApiReturn(WASMOS_ERR_DMA_INVALID);
    }

    uint64_t region_bytes = (uint64_t)(uint32_t)pages * 0x1000ULL;
    uint64_t phys_base = pfa_alloc_pages_below((uint64_t)(uint32_t)pages, 0x80000000ULL);
    if (!phys_base) {
        m3ApiReturn(WASMOS_ERR_DMA_UNAVAILABLE);
    }
    if (!capability_dma_range_allowed(proc->context_id, phys_base, region_bytes)) {
        pfa_free_pages(phys_base, (uint64_t)(uint32_t)pages);
        m3ApiReturn(WASMOS_ERR_DMA_RANGE);
    }
    /* Enforce the driver's declared DMA page budget (dma.buffer manifest flags). */
    if (!capability_dma_within_budget(proc->context_id, region_bytes)) {
        pfa_free_pages(phys_base, (uint64_t)(uint32_t)pages);
        m3ApiReturn(WASMOS_ERR_DMA_RANGE);
    }

    uint64_t mem_size64 = (uint64_t)mem_size;
    const uint64_t map_auto_min_off = 0x200000ULL;
    uint64_t scan_off = (map_auto_min_off + 0xFFFULL) & ~0xFFFULL;
    uint64_t off64 = 0;
    uint8_t found = 0;

    for (off64 = scan_off; off64 + region_bytes <= mem_size64; off64 += 0x1000ULL) {
        uint64_t probe_virt = 0;
        if (wasm_linear_window_overlaps(proc->pid, (uint32_t)off64, (uint32_t)region_bytes)) {
            continue;
        }
        if (wasm_user_va_from_offset(proc->context_id, (uint32_t)off64, (uint32_t)region_bytes,
                                     &probe_virt) != 0) {
            continue;
        }
        if (mm_user_range_permitted(proc->context_id, probe_virt, region_bytes,
                                    MEM_REGION_FLAG_WRITE) != 0) {
            continue;
        }
        if ((probe_virt & 0xFFFULL) != 0) {
            continue;
        }
        found = 1;
        break;
    }

    if (!found) {
        off64 = (mem_size64 + 0xFFFULL) & ~0xFFFULL;
        uint64_t required = off64 + region_bytes;
        if (required > mem_size64) {
            uint32_t target_pages = (uint32_t)((required + 0xFFFFULL) >> 16);
            if (ResizeMemory(runtime, target_pages) != m3Err_none) {
                pfa_free_pages(phys_base, (uint64_t)(uint32_t)pages);
                m3ApiReturn(WASMOS_ERR_DMA_UNAVAILABLE);
            }
            mem_base = m3_GetMemory(runtime, &mem_size, 0);
            if (!mem_base || mem_size == 0) {
                pfa_free_pages(phys_base, (uint64_t)(uint32_t)pages);
                m3ApiReturn(WASMOS_ERR_DMA_UNAVAILABLE);
            }
            mem_size64 = (uint64_t)m3_GetMemorySize(runtime);
            if (required > mem_size64) {
                pfa_free_pages(phys_base, (uint64_t)(uint32_t)pages);
                m3ApiReturn(WASMOS_ERR_DMA_UNAVAILABLE);
            }
        }
    }

    uint32_t off32 = (uint32_t)off64;
    uint64_t virt = 0;
    if (wasm_user_va_from_offset(proc->context_id, off32, (uint32_t)region_bytes, &virt) != 0 ||
        mm_user_range_permitted(proc->context_id, virt, region_bytes, MEM_REGION_FLAG_WRITE) != 0 ||
        (virt & 0xFFFULL) != 0) {
        pfa_free_pages(phys_base, (uint64_t)(uint32_t)pages);
        m3ApiReturn(WASMOS_ERR_DMA_INVALID);
    }
    if (mm_context_map_physical(proc->context_id, virt, phys_base, region_bytes,
                                MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE |
                                    MEM_REGION_FLAG_USER) != 0) {
        pfa_free_pages(phys_base, (uint64_t)(uint32_t)pages);
        m3ApiReturn(WASMOS_ERR_DMA_UNAVAILABLE);
    }

    /* The pages are already exclusively allocated from the PFA, so unlike
     * shared mappings they remain stable for the process lifetime without an
     * extra pin reference; wasm3_release_pid() returns them on reap. */
    wasm_dma_region_map_track(proc->pid, off32, (uint32_t)region_bytes, phys_base, (uint32_t)pages);
    __builtin_memcpy(mem_base + (uint32_t)out_phys_off, &phys_base, sizeof(uint64_t));
    capability_dma_commit(proc->context_id, region_bytes); /* charge the pinned footprint */
    m3ApiReturn(off32);
}

m3ApiRawFunction(wasmos_phys_map) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, phys_lo) m3ApiGetArg(int32_t, phys_hi)
        m3ApiGetArg(int32_t, size) m3ApiGetArg(int32_t, wasm_offset)

            if (size <= 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    if ((size & 0xFFF) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_UNALIGNED);
    }
    if (wasm_offset < 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    if ((wasm_offset & 0xFFF) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_UNALIGNED);
    }

    uint64_t phys = ((uint64_t)(uint32_t)phys_hi << 32) | (uint64_t)(uint32_t)phys_lo;
    if (phys == 0) {
        m3ApiReturn(WASMOS_INVAL);
    }

    uint32_t off32 = (uint32_t)wasm_offset;
    uint32_t size32 = (uint32_t)size;

    uint32_t mem_size = 0;
    uint8_t* mem_base = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem_base || mem_size == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_WINDOW);
    }
    if ((uint64_t)off32 + (uint64_t)size32 > (uint64_t)mem_size) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_WINDOW);
    }

    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    if (require_mmio_capability(proc->context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NOT_AUTHORIZED);
    }

    /* Copy physical memory into wasm3 linear-memory host buffer via the
     * kernel higher-half mapping.  wasm3 accesses linear memory exclusively
     * through this host pointer; the user-VA page table is irrelevant here. */
    memcpy(mem_base + off32, ptr_cast(void, (phys | KERNEL_HIGHER_HALF_BASE)), (size_t)size32);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_shmem_create) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, pages) m3ApiGetArg(int32_t, flags)

        if (pages <= 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_ARGS);
    }

    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0 || require_dma_capability(proc->context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_CAP);
    }

    uint32_t id = 0;
    uint64_t phys = 0;
    uint32_t create_flags =
        (flags > 0) ? (uint32_t)flags : (MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE);
    if (mm_shared_create(proc->context_id, (uint64_t)(uint32_t)pages, create_flags, &id, &phys) !=
        0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_MAP);
    }
    (void)phys;
    m3ApiReturn((int32_t)id);
}

/* Register the caller's shared-memory region (id) as the kernel klog ring.  The
 * caller (the VT) has already created + mapped + wasmos_ringbuf_init'd it; the
 * kernel retains it and reaches it through the higher-half alias.  Ownership is
 * enforced by mm_shared_get_phys inside klog_register_ring, so no extra
 * capability gate is needed here beyond the one the caller used to create it. */
m3ApiRawFunction(wasmos_klog_register_ring) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, id)

        process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    if (id <= 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    m3ApiReturn(klog_register_ring(proc->context_id, (uint32_t)id));
}

m3ApiRawFunction(wasmos_shmem_map) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, id) m3ApiGetArg(int32_t, ptr)
        m3ApiGetArg(int32_t, size)

            if (id <= 0 || ptr < 0 || size <= 0 || (size & 0xFFF) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_UNALIGNED);
    }

    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0 || require_dma_capability(proc->context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_CAP);
    }
    mm_context_t* ctx = mm_context_get(proc->context_id);
    if (!ctx || ctx->root_table == 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_CAP);
    }

    uint64_t phys_base = 0;
    uint64_t shared_pages = 0;
    if (mm_shared_get_phys(proc->context_id, (uint32_t)id, &phys_base, &shared_pages) != 0 ||
        shared_pages == 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_ID);
    }
    uint64_t map_size = (uint64_t)(uint32_t)size;
    uint64_t needed_size = shared_pages * 0x1000ULL;
    if (map_size < needed_size) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_SIZE);
    }

    uint32_t off32 = (uint32_t)ptr;
    uint32_t map_size32 = (uint32_t)size;
    if ((uint64_t)off32 + (uint64_t)map_size32 > (uint64_t)m3_GetMemorySize(runtime)) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_WINDOW);
    }
    uint64_t virt = 0;
    if (wasm_user_va_from_offset(proc->context_id, off32, map_size32, &virt) != 0 ||
        mm_user_range_permitted(proc->context_id, virt, (uint64_t)map_size32,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_WINDOW);
    }
    if ((virt & 0xFFFULL) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_UNALIGNED);
    }

    if (mm_context_map_physical(proc->context_id, virt, phys_base, needed_size,
                                MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE |
                                    MEM_REGION_FLAG_USER) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_SIZE);
    }

    if (mm_shared_retain(proc->context_id, (uint32_t)id) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_MAP);
    }
    wasm_shmem_map_track(proc->pid, (uint32_t)id, off32, map_size32);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_shmem_map_auto) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, id) m3ApiGetArg(int32_t, size)

        if (id <= 0 || size <= 0 || (size & 0xFFF) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_ARGS);
    }

    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0 || require_dma_capability(proc->context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_CAP);
    }
    mm_context_t* ctx = mm_context_get(proc->context_id);
    if (!ctx || ctx->root_table == 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_CAP);
    }

    uint64_t phys_base = 0;
    uint64_t shared_pages = 0;
    int get_phys_rc = mm_shared_get_phys(proc->context_id, (uint32_t)id, &phys_base, &shared_pages);
    if (get_phys_rc != 0 || shared_pages == 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_ID);
    }

    uint64_t map_size = (uint64_t)(uint32_t)size;
    uint64_t needed_size = shared_pages * 0x1000ULL;
    if (map_size < needed_size) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_SIZE);
    }

    uint64_t mem_size = (uint64_t)m3_GetMemorySize(runtime);
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
        if (wasm_linear_window_overlaps(proc->pid, (uint32_t)off64, (uint32_t)map_size)) {
            continue;
        }
        uint64_t probe_virt = 0;
        if (wasm_user_va_from_offset(proc->context_id, (uint32_t)off64, (uint32_t)map_size,
                                     &probe_virt) != 0) {
            continue;
        }
        if (mm_user_range_permitted(proc->context_id, probe_virt, (uint64_t)(uint32_t)map_size,
                                    MEM_REGION_FLAG_WRITE) != 0) {
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
                m3ApiReturn(WASMOS_ERR_SHMEM_NO_WINDOW);
            }
            mem_size = (uint64_t)m3_GetMemorySize(runtime);
            if (required > mem_size) {
                m3ApiReturn(WASMOS_ERR_SHMEM_NO_WINDOW);
            }
        }
    }

    uint32_t off32 = (uint32_t)off64;
    uint32_t map_size32 = (uint32_t)map_size;
    uint64_t virt = 0;
    if (wasm_user_va_from_offset(proc->context_id, off32, map_size32, &virt) != 0 ||
        mm_user_range_permitted(proc->context_id, virt, (uint64_t)map_size32,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_MAP);
    }
    if ((virt & 0xFFFULL) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_UNALIGNED);
    }

    if (mm_context_map_physical(proc->context_id, virt, phys_base, needed_size,
                                MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE |
                                    MEM_REGION_FLAG_USER) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_MAP);
    }
    if (mm_shared_retain(proc->context_id, (uint32_t)id) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_MAP);
    }
    wasm_shmem_map_track(proc->pid, (uint32_t)id, off32, map_size32);

    /* FIXME(shmem-map-auto): unmap currently only drops shared refs and does
     * not reclaim or reuse grown linear-memory pages for future map_auto
     * allocations. */
    m3ApiReturn((int32_t)off32);
}

m3ApiRawFunction(wasmos_shmem_grant) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, id) m3ApiGetArg(int32_t, target_pid)

        process_t* proc = process_get(process_current_pid());
    process_t* target = 0;
    if (id <= 0 || target_pid <= 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_ID);
    }
    if (!proc || proc->context_id == 0 || require_dma_capability(proc->context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_CAP);
    }
    target = process_get((uint32_t)target_pid);
    if (!target || target->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_CAP);
    }
    m3ApiReturn(mm_shared_grant(proc->context_id, (uint32_t)id, target->context_id));
}

m3ApiRawFunction(wasmos_shmem_revoke) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, id) m3ApiGetArg(int32_t, target_pid)

        process_t* proc = process_get(process_current_pid());
    process_t* target = 0;
    if (id <= 0 || target_pid <= 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_ID);
    }
    if (!proc || proc->context_id == 0 || require_dma_capability(proc->context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_CAP);
    }
    target = process_get((uint32_t)target_pid);
    if (!target || target->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_CAP);
    }
    m3ApiReturn(mm_shared_revoke(proc->context_id, (uint32_t)id, target->context_id));
}

m3ApiRawFunction(wasmos_shmem_unmap) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, id)

        if (id <= 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_ARGS);
    }
    /* FIXME: This currently only releases shared-region ownership/refcount.
     * It does not restore the previous linear-memory page mappings. */
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_CAP);
    }
    wasm_shmem_map_untrack(proc->pid, (uint32_t)id);
    m3ApiReturn(mm_shared_release(proc->context_id, (uint32_t)id));
}

m3ApiRawFunction(wasmos_shmem_flush) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, id) m3ApiGetArg(int32_t, ptr)
        m3ApiGetArg(int32_t, size)

            if (id <= 0 || ptr < 0 || size <= 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_ARGS);
    }

    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0 || require_dma_capability(proc->context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_CAP);
    }

    uint64_t phys_base = 0;
    uint64_t shared_pages = 0;
    if (mm_shared_get_phys(proc->context_id, (uint32_t)id, &phys_base, &shared_pages) != 0 ||
        shared_pages == 0 || phys_base == 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_ID);
    }

    uint32_t mem_size = 0;
    uint8_t* mem_base = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem_base || mem_size == 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_WINDOW);
    }

    uint32_t off32 = (uint32_t)ptr;
    uint32_t len32 = (uint32_t)size;
    if ((uint64_t)off32 + (uint64_t)len32 > (uint64_t)mem_size) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_WINDOW);
    }

    uint64_t max_len = shared_pages * 0x1000ULL;
    if ((uint64_t)len32 > max_len) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_SIZE);
    }

    memcpy(ptr_cast(void, (phys_base | KERNEL_HIGHER_HALF_BASE)), mem_base + off32, (size_t)len32);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_shmem_refresh) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, id) m3ApiGetArg(int32_t, ptr)
        m3ApiGetArg(int32_t, size)

            if (id <= 0 || ptr < 0 || size <= 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_ARGS);
    }

    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0 || require_dma_capability(proc->context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_CAP);
    }

    uint64_t phys_base = 0;
    uint64_t shared_pages = 0;
    if (mm_shared_get_phys(proc->context_id, (uint32_t)id, &phys_base, &shared_pages) != 0 ||
        shared_pages == 0 || phys_base == 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_ID);
    }

    uint32_t mem_size = 0;
    uint8_t* mem_base = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem_base || mem_size == 0) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_WINDOW);
    }

    uint32_t off32 = (uint32_t)ptr;
    uint32_t len32 = (uint32_t)size;
    if ((uint64_t)off32 + (uint64_t)len32 > (uint64_t)mem_size) {
        m3ApiReturn(WASMOS_ERR_SHMEM_NO_WINDOW);
    }

    uint64_t max_len = shared_pages * 0x1000ULL;
    if ((uint64_t)len32 > max_len) {
        m3ApiReturn(WASMOS_ERR_SHMEM_BAD_SIZE);
    }

    memcpy(mem_base + off32, ptr_cast(void, (phys_base | KERNEL_HIGHER_HALF_BASE)), (size_t)len32);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_irq_route_ipc) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, irq_line) m3ApiGetArg(int32_t, msg_endpoint)

        uint32_t context_id = 0;
    if (irq_line < 0) {
        m3ApiReturn(WASMOS_ERR_IRQ_BAD_LINE);
    }
    if (msg_endpoint < 0) {
        m3ApiReturn(WASMOS_ERR_IRQ_BAD_ENDPOINT);
    }
    if (current_process_context(&context_id) != 0 ||
        require_irq_route_capability(context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_IRQ_NOT_AUTHORIZED);
    }
    m3ApiReturn(irq_register(context_id, (uint32_t)irq_line, (uint32_t)msg_endpoint));
}

m3ApiRawFunction(wasmos_irq_ack) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, irq_line)

        uint32_t context_id = 0;
    if (irq_line < 0) {
        m3ApiReturn(WASMOS_ERR_IRQ_BAD_LINE);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_IRQ_NOT_AUTHORIZED);
    }
    m3ApiReturn(irq_ack(context_id, (uint32_t)irq_line));
}

m3ApiRawFunction(wasmos_irq_configure) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, irq_line) m3ApiGetArg(int32_t, flags)

        uint32_t context_id = 0;
    if (irq_line < 0) {
        m3ApiReturn(WASMOS_ERR_IRQ_BAD_LINE);
    }
    if (current_process_context(&context_id) != 0 ||
        require_irq_route_capability(context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_IRQ_NOT_AUTHORIZED);
    }
    m3ApiReturn(irq_configure((uint32_t)irq_line, (uint32_t)flags));
}

m3ApiRawFunction(wasmos_irq_unroute) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, irq_line)

        uint32_t context_id = 0;
    if (irq_line < 0) {
        m3ApiReturn(WASMOS_ERR_IRQ_BAD_LINE);
    }
    if (current_process_context(&context_id) != 0 ||
        require_irq_route_capability(context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_IRQ_NOT_AUTHORIZED);
    }
    m3ApiReturn(irq_unregister(context_id, (uint32_t)irq_line));
}

/* Allocate an MSI vector bound to one of the caller's endpoints. The kernel owns
 * the vector namespace; the caller passes the returned address/data pair to the
 * bus driver that programs the device (pci-bus, PCI_IPC_MSI_BIND). */
m3ApiRawFunction(wasmos_msi_alloc) {
    typedef struct {
        uint32_t address_lo;
        uint32_t address_hi;
        uint32_t data;
        uint32_t vector;
    } msi_desc_t;
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, endpoint) m3ApiGetArgMem(msi_desc_t*, out)
        m3ApiCheckMem(out, sizeof(msi_desc_t));
    uint32_t context_id = 0;
    if (endpoint < 0) {
        m3ApiReturn(WASMOS_ERR_MSI_BAD_ENDPOINT);
    }
    if (current_process_context(&context_id) != 0 ||
        require_irq_route_capability(context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_MSI_NOT_AUTHORIZED);
    }
    m3ApiReturn(msi_alloc(context_id, (uint32_t)endpoint, &out->address_lo, &out->address_hi,
                          &out->data, &out->vector));
}

m3ApiRawFunction(wasmos_msi_free) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, vector)

        uint32_t context_id = 0;
    if (vector < 0) {
        m3ApiReturn(WASMOS_ERR_MSI_BAD_VECTOR);
    }
    if (current_process_context(&context_id) != 0 ||
        require_irq_route_capability(context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_MSI_NOT_AUTHORIZED);
    }
    m3ApiReturn(msi_free(context_id, (uint32_t)vector));
}

/* Poke a memory-mapped device register (pci-bus placing an MSI-X table entry).
 * mmio_write32_phys refuses anything overlapping system RAM, so the mmio.map
 * capability buys MMIO access, not arbitrary physical memory access. */
m3ApiRawFunction(wasmos_mmio_write32) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, phys_lo) m3ApiGetArg(int32_t, phys_hi)
        m3ApiGetArg(int32_t, value)

            uint32_t context_id = 0;
    if (current_process_context(&context_id) != 0 || require_mmio_capability(context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_MSI_NOT_AUTHORIZED);
    }
    uint64_t phys = ((uint64_t)(uint32_t)phys_hi << 32) | (uint64_t)(uint32_t)phys_lo;
    m3ApiReturn(mmio_write32_phys(phys, (uint32_t)value));
}

m3ApiRawFunction(wasmos_system_halt) {
    m3ApiReturnType(int32_t) uint32_t context_id = 0;
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    if (require_system_control_capability(context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NOT_AUTHORIZED);
    }
    kernel_system_poweroff();
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_system_reboot) {
    m3ApiReturnType(int32_t) uint32_t context_id = 0;
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    if (require_system_control_capability(context_id) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NOT_AUTHORIZED);
    }
    kernel_system_reboot();
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_acpi_rsdp_info) {
    m3ApiReturnType(int32_t) m3ApiGetArgMem(uint8_t*, out_ptr)
        m3ApiGetArgMem(uint32_t*, out_len_ptr) m3ApiGetArg(int32_t, max_len)

            if (max_len <= 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    if (!g_wasm_boot_info || !g_wasm_boot_info->rsdp || g_wasm_boot_info->rsdp_length == 0) {
        m3ApiReturn(WASMOS_NOENT); /* no ACPI RSDP was handed over at boot */
    }
    uint32_t len = g_wasm_boot_info->rsdp_length;
    if (len > (uint32_t)max_len) {
        m3ApiReturn(WASMOS_ERR_KERNEL_TOO_LARGE);
    }
    m3ApiCheckMem(out_ptr, len);
    m3ApiCheckMem(out_len_ptr, sizeof(uint32_t));
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t out_user = 0;
    uint64_t out_len_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), out_ptr, len,
                                   &out_user) != 0 ||
        mm_user_range_permitted(proc->context_id, out_user, len, MEM_REGION_FLAG_WRITE) != 0 ||
        wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), out_len_ptr,
                                   sizeof(uint32_t), &out_len_user) != 0 ||
        mm_user_range_permitted(proc->context_id, out_len_user, sizeof(uint32_t),
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }

    const uint8_t* src = ptr_cast(uint8_t, g_wasm_boot_info->rsdp);
    if (wasm_copy_to_user_bytes(proc->context_id, out_user, src, len) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    if (wasm_copy_to_user_bytes(proc->context_id, out_len_user, &len, sizeof(len)) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_boot_module_name) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, index) m3ApiGetArgMem(char*, out_ptr)
        m3ApiGetArg(int32_t, out_len)

            if (index < 0 || out_len <= 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    m3ApiCheckMem(out_ptr, (uint32_t)out_len);
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t out_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), out_ptr, (uint32_t)out_len,
                                   &out_user) != 0 ||
        mm_user_range_permitted(proc->context_id, out_user, (uint64_t)(uint32_t)out_len,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }

    char local_name[64];
    uint32_t name_len = 0;
    if (boot_module_name_at((uint32_t)index, local_name, sizeof(local_name), &name_len) != 0) {
        m3ApiReturn(WASMOS_NOENT); /* no module at that index */
    }
    uint32_t copy_len = name_len;
    if (copy_len >= (uint32_t)out_len) {
        copy_len = (uint32_t)out_len - 1U;
    }
    if (wasm_copy_to_user_bytes(proc->context_id, out_user, local_name, copy_len) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    char nul = '\0';
    if (wasm_copy_to_user_bytes(proc->context_id, out_user + (uint64_t)copy_len, &nul, 1) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    m3ApiReturn((int32_t)name_len);
}

m3ApiRawFunction(wasmos_initfs_entry_count) {
    m3ApiReturnType(int32_t) const wasmos_initfs_header_t* hdr = 0;
    const uint8_t* base = 0;
    if (initfs_header_get(&hdr, &base) != 0) {
        m3ApiReturn(WASMOS_ERR_FS_NO_IMAGE);
    }
    wasmos_error_code_t count_rc = hostcall_value_check(hdr->entry_count);
    if (count_rc != WASMOS_OK) {
        m3ApiReturn(count_rc);
    }
    m3ApiReturn((int32_t)hdr->entry_count);
}

m3ApiRawFunction(wasmos_initfs_entry_name) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, index) m3ApiGetArgMem(char*, out_ptr)
        m3ApiGetArg(int32_t, out_len)

            if (index < 0 || out_len <= 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    m3ApiCheckMem(out_ptr, (uint32_t)out_len);
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t out_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), out_ptr, (uint32_t)out_len,
                                   &out_user) != 0 ||
        mm_user_range_permitted(proc->context_id, out_user, (uint64_t)(uint32_t)out_len,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }

    wasmos_initfs_entry_t entry;
    if (initfs_entry_at((uint32_t)index, &entry) != 0) {
        m3ApiReturn(WASMOS_ERR_FS_NOT_FOUND);
    }
    uint32_t name_len = 0;
    uint32_t copy_len = 0;
    wasmos_error_code_t clamp_rc = hostcall_name_clamp(entry.path, (uint32_t)sizeof(entry.path),
                                                       (uint32_t)out_len, &name_len, &copy_len);
    if (clamp_rc != WASMOS_OK) {
        m3ApiReturn(clamp_rc);
    }
    if (copy_len > 0 &&
        wasm_copy_to_user_bytes(proc->context_id, out_user, entry.path, copy_len) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }

    char nul = '\0';
    if (wasm_copy_to_user_bytes(proc->context_id, out_user + (uint64_t)copy_len, &nul, 1) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }

    /* The TRUE length, so a caller can tell its buffer was too small. */
    m3ApiReturn((int32_t)name_len);
}

m3ApiRawFunction(wasmos_initfs_entry_size) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, index) wasmos_initfs_entry_t entry;
    if (index < 0 || initfs_entry_at((uint32_t)index, &entry) != 0) {
        m3ApiReturn(WASMOS_ERR_FS_NOT_FOUND);
    }
    wasmos_error_code_t size_rc = hostcall_value_check(entry.size);
    if (size_rc != WASMOS_OK) {
        m3ApiReturn(size_rc);
    }
    m3ApiReturn((int32_t)entry.size);
}

m3ApiRawFunction(wasmos_initfs_entry_copy) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, index) m3ApiGetArgMem(uint8_t*, out_ptr)
        m3ApiGetArg(int32_t, len) m3ApiGetArg(int32_t, offset)

            if (index < 0 || len <= 0 || offset < 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    m3ApiCheckMem(out_ptr, (uint32_t)len);
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t out_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), out_ptr, (uint32_t)len,
                                   &out_user) != 0 ||
        mm_user_range_permitted(proc->context_id, out_user, (uint64_t)(uint32_t)len,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }

    wasmos_initfs_entry_t entry;
    if (initfs_entry_at((uint32_t)index, &entry) != 0) {
        m3ApiReturn(WASMOS_ERR_FS_NOT_FOUND);
    }
    if ((uint32_t)offset >= entry.size) {
        m3ApiReturn(0);
    }
    uint32_t copy_len = (uint32_t)len;
    uint32_t available = entry.size - (uint32_t)offset;
    if (copy_len > available) {
        copy_len = available;
    }
    const uint8_t* src = (const uint8_t*)g_wasm_boot_info->initfs + entry.offset + (uint32_t)offset;
    if (wasm_copy_to_user_bytes(proc->context_id, out_user, src, copy_len) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    m3ApiReturn((int32_t)copy_len);
}

m3ApiRawFunction(wasmos_initfs_find_path) {
    m3ApiReturnType(int32_t) m3ApiGetArgMem(const char*, path_ptr) m3ApiGetArg(int32_t, path_len)

        if (path_len <= 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    if (path_len >= 112) {
        m3ApiReturn(WASMOS_ERR_FS_PATH_TOO_LONG);
    }
    m3ApiCheckMem(path_ptr, (uint32_t)path_len);
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t path_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), path_ptr,
                                   (uint32_t)path_len, &path_user) != 0 ||
        mm_user_range_permitted(proc->context_id, path_user, (uint64_t)(uint32_t)path_len,
                                MEM_REGION_FLAG_READ) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }

    char local_path[112];
    if (mm_copy_from_user(proc->context_id, local_path, path_user, (uint64_t)(uint32_t)path_len) !=
        0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    local_path[path_len] = '\0';

    uint32_t ri = 0;
    while (local_path[ri] == '/') {
        ri++;
    }
    if ((local_path[ri] == 'i' || local_path[ri] == 'I') &&
        (local_path[ri + 1] == 'n' || local_path[ri + 1] == 'N') &&
        (local_path[ri + 2] == 'i' || local_path[ri + 2] == 'I') &&
        (local_path[ri + 3] == 't' || local_path[ri + 3] == 'T') && local_path[ri + 4] == '/') {
        ri += 5;
    }
    if (local_path[ri] == '\0') {
        m3ApiReturn(WASMOS_INVAL);
    }
    const wasmos_initfs_header_t* hdr = 0;
    const uint8_t* base = 0;
    if (initfs_header_get(&hdr, &base) != 0) {
        m3ApiReturn(WASMOS_ERR_FS_NO_IMAGE);
    }
    for (uint32_t i = 0; i < hdr->entry_count; ++i) {
        wasmos_initfs_entry_t entry;
        if (initfs_entry_at(i, &entry) != 0) {
            continue;
        }
        if (strcasecmp(entry.path, &local_path[ri]) == 0) {
            m3ApiReturn((int32_t)i);
        }
        const char* base_name = entry.path;
        for (uint32_t j = 0; entry.path[j] != '\0'; ++j) {
            if (entry.path[j] == '/') {
                base_name = &entry.path[j + 1];
            }
        }
        if (strcasecmp(base_name, &local_path[ri]) == 0) {
            m3ApiReturn((int32_t)i);
        }
    }
    m3ApiReturn(WASMOS_ERR_FS_NOT_FOUND);
}

m3ApiRawFunction(wasmos_console_write) {
    m3ApiReturnType(int32_t) m3ApiGetArgMem(const char*, ptr) m3ApiGetArg(int32_t, len)

        if (len < 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    if (len == 0) {
        m3ApiReturn(0); /* nothing to write is not a failure */
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

m3ApiRawFunction(wasmos_debug_mark) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, tag)
        trace_write_unlocked("[wasm] debug_mark tag=");
    trace_do(serial_write_hex64_unlocked((uint64_t)(uint32_t)tag));
    trace_write_unlocked("[wasm] debug_mark pid=");
    trace_do(serial_write_hex64_unlocked((uint64_t)process_current_pid()));
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_kmap_dump) {
    m3ApiReturnType(int32_t) process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t root = mm_context_root_table(proc->context_id);
    if (!root) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    paging_dump_user_root_kernel_mappings(root);
    if ((proc->ctx.cs & 0x3u) == 0x3u) {
        m3ApiReturn(paging_verify_user_root_no_low_slot(root, 1) == 0 ? 0 : -1);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_kmap_dump_all) {
    m3ApiReturnType(int32_t) uint32_t count = process_count_active();
    int failures = 0;

    trace_do(klog_write("[kmap] contexts begin\n"));
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t pid = 0;
        uint32_t parent_pid = 0;
        const char* name = 0;
        if (process_info_at_ex(i, &pid, &parent_pid, &name) != 0) {
            continue;
        }
        process_t* proc = process_get(pid);
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

m3ApiRawFunction(wasmos_console_read) {
    m3ApiReturnType(int32_t) m3ApiGetArgMem(char*, ptr) m3ApiGetArg(int32_t, len)

        /* A zero-capacity buffer cannot receive a byte. Reporting 0 here would
         * say "no byte was available", which is a different fact and hides the
         * caller's bug; WARP already refused it. (The comment this replaced was
         * copied from console_write, where a zero length IS a no-op success.) */
        if (len <= 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    m3ApiCheckMem(ptr, 1);
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), ptr, 1, &ptr_user) != 0 ||
        mm_user_range_permitted(proc->context_id, ptr_user, 1, MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }
    uint8_t ch = 0;
    int rc = serial_read_char(&ch);
    if (rc <= 0) {
        m3ApiReturn(rc);
    }
    char out = (char)ch;
    if (wasm_copy_to_user_bytes(proc->context_id, ptr_user, &out, 1) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    m3ApiReturn(1);
}

m3ApiRawFunction(wasmos_sync_user_read) {
    m3ApiReturnType(int32_t) m3ApiGetArgMem(uint8_t*, ptr) m3ApiGetArg(int32_t, len)

        if (len < 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    if (len == 0) {
        m3ApiReturn(0);
    }
    m3ApiCheckMem(ptr, (uint32_t)len);
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), ptr, (uint32_t)len,
                                   &ptr_user) != 0 ||
        mm_user_range_permitted(proc->context_id, ptr_user, (uint64_t)(uint32_t)len,
                                MEM_REGION_FLAG_READ) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }
    if (mm_copy_from_user(proc->context_id, ptr, ptr_user, (uint64_t)(uint32_t)len) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_input_push) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, ch) serial_input_push((uint8_t)(ch & 0xFF));
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_input_read) {
    m3ApiReturnType(int32_t) uint8_t ch = 0;
    if (serial_input_read(&ch)) {
        m3ApiReturn((int32_t)ch);
    }
    m3ApiReturn(WASMOS_AGAIN); /* nothing queued, not a failure */
}

m3ApiRawFunction(wasmos_serial_register) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, endpoint) uint32_t endpoint_u32 = 0;
    if (wasm_arg_u32_nonneg(endpoint, &endpoint_u32) != 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    m3ApiReturn(serial_register_remote_driver(endpoint_u32));
}

m3ApiRawFunction(wasmos_proc_count) {
    m3ApiReturnType(int32_t) m3ApiReturn((int32_t)process_count_active());
}

m3ApiRawFunction(wasmos_proc_exit) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, status) process_t* proc =
        process_get(process_current_pid());
    if (!proc) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    process_set_exit_status(proc, status);
    process_yield(PROCESS_RUN_EXITED);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_proc_notify_ready) {
    m3ApiReturnType(int32_t) process_t* proc = process_get(process_current_pid());
    if (proc) {
        process_notify_ready(proc);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_sched_ticks) {
    /* Monotonic: kept positive and wrapping at 2^31 so deltas stay correct.
     * A plain cast went negative at ~99 days of uptime at 250 Hz, and a
     * negative return is how this ABI spells "error". */
    m3ApiReturnType(int32_t) m3ApiReturn(hostcall_value_counter(timer_ticks()));
}

m3ApiRawFunction(wasmos_sched_ready_count) {
    m3ApiReturnType(int32_t) m3ApiReturn((int32_t)process_ready_count());
}

m3ApiRawFunction(wasmos_sched_cpu_count) {
    m3ApiReturnType(int32_t) m3ApiReturn((int32_t)g_cpu_count);
}

m3ApiRawFunction(wasmos_physmem_stats) {
    typedef struct {
        uint64_t total_bytes;
        uint64_t free_bytes;
    } physmem_stats_t;
    m3ApiReturnType(int32_t) m3ApiGetArgMem(physmem_stats_t*, out)
        m3ApiCheckMem(out, sizeof(physmem_stats_t));
    out->total_bytes = pfa_total_bytes();
    out->free_bytes = pfa_free_bytes();
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_kernel_runtime) {
    m3ApiReturnType(int32_t) m3ApiReturn(0); /* wasm3 */
}

m3ApiRawFunction(wasmos_sched_cpu_stats) {
    typedef struct {
        uint32_t ready_count;
        uint32_t running_pid;
        uint32_t steal_count;
        uint32_t dispatch_count;
        uint32_t last_pid;
    } cpu_stats_t;
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, cpu_id) m3ApiGetArgMem(cpu_stats_t*, out)
        m3ApiCheckMem(out, sizeof(cpu_stats_t));
    if (cpu_id < 0 || (uint32_t)cpu_id >= g_cpu_count) {
        m3ApiReturn(WASMOS_INVAL);
    }
    cpu_sched_t* cs = &g_cpus[(uint32_t)cpu_id].sched;
    uint32_t ready = 0;
    for (int p = 0; p < SCHED_PRIO_MAX; p++) {
        ready += cs->thread_count[p];
    }
    out->ready_count = ready;
    out->running_pid = g_cpus[(uint32_t)cpu_id].current_process
                           ? g_cpus[(uint32_t)cpu_id].current_process->pid
                           : 0;
    out->steal_count = g_cpus[(uint32_t)cpu_id].steal_count;
    out->dispatch_count = g_cpus[(uint32_t)cpu_id].dispatch_count;
    out->last_pid = g_cpus[(uint32_t)cpu_id].last_dispatched_pid;
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_sched_current_pid) {
    m3ApiReturnType(int32_t) m3ApiReturn((int32_t)process_current_pid());
}

m3ApiRawFunction(wasmos_sched_yield) {
    m3ApiReturnType(int32_t) process_yield(PROCESS_RUN_YIELDED);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_thread_gettid) {
    m3ApiReturnType(int32_t) m3ApiReturn((int32_t)thread_current_tid());
}

m3ApiRawFunction(wasmos_thread_yield) {
    m3ApiReturnType(int32_t) process_yield(PROCESS_RUN_YIELDED);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_proc_info) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, index) m3ApiGetArgMem(char*, buf)
        m3ApiGetArg(int32_t, buf_len)

            if (index < 0 || buf_len <= 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    m3ApiCheckMem(buf, (uint32_t)buf_len);
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t buf_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), buf, (uint32_t)buf_len,
                                   &buf_user) != 0 ||
        mm_user_range_permitted(proc->context_id, buf_user, (uint64_t)(uint32_t)buf_len,
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }

    uint32_t pid = 0;
    const char* name = 0;
    if (process_info_at((uint32_t)index, &pid, &name) != 0) {
        m3ApiReturn(WASMOS_NOENT); /* no process at that index */
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
            if (mm_copy_to_user(proc->context_id, buf_user + (uint64_t)chunk_base, bounce,
                                (uint64_t)sizeof(bounce)) != 0) {
                m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
            }
        }
    }
    uint32_t tail = copied % (uint32_t)sizeof(bounce);
    if (tail > 0) {
        if (mm_copy_to_user(proc->context_id, buf_user + (uint64_t)(copied - tail), bounce,
                            (uint64_t)tail) != 0) {
            m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
        }
    }
    bounce[0] = '\0';
    if (mm_copy_to_user(proc->context_id, buf_user + (uint64_t)(out_len - 1U), bounce, 1) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    m3ApiReturn((int32_t)pid);
}

m3ApiRawFunction(wasmos_proc_info_ex) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, index) m3ApiGetArgMem(char*, buf)
        m3ApiGetArg(int32_t, buf_len) m3ApiGetArgMem(uint32_t*, parent_ptr)

            if (index < 0 || buf_len <= 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    m3ApiCheckMem(buf, (uint32_t)buf_len);
    m3ApiCheckMem(parent_ptr, sizeof(uint32_t));
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t buf_user = 0;
    uint64_t parent_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), buf, (uint32_t)buf_len,
                                   &buf_user) != 0 ||
        mm_user_range_permitted(proc->context_id, buf_user, (uint64_t)(uint32_t)buf_len,
                                MEM_REGION_FLAG_WRITE) != 0 ||
        wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), parent_ptr,
                                   sizeof(uint32_t), &parent_user) != 0 ||
        mm_user_range_permitted(proc->context_id, parent_user, sizeof(uint32_t),
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }

    uint32_t pid = 0;
    uint32_t parent_pid = 0;
    const char* name = 0;
    if (process_info_at_ex((uint32_t)index, &pid, &parent_pid, &name) != 0) {
        m3ApiReturn(WASMOS_NOENT); /* no process at that index */
    }
    if (mm_copy_to_user(proc->context_id, parent_user, &parent_pid, sizeof(parent_pid)) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
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
            if (mm_copy_to_user(proc->context_id, buf_user + (uint64_t)chunk_base, bounce,
                                (uint64_t)sizeof(bounce)) != 0) {
                m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
            }
        }
    }
    uint32_t tail = copied % (uint32_t)sizeof(bounce);
    if (tail > 0) {
        if (mm_copy_to_user(proc->context_id, buf_user + (uint64_t)(copied - tail), bounce,
                            (uint64_t)tail) != 0) {
            m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
        }
    }
    bounce[0] = '\0';
    if (mm_copy_to_user(proc->context_id, buf_user + (uint64_t)(out_len - 1U), bounce, 1) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    m3ApiReturn((int32_t)pid);
}

m3ApiRawFunction(wasmos_proc_info_stats) {
    typedef struct {
        uint32_t state;
        uint32_t block_reason;
        char runtime_tag[8];
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

    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, index) m3ApiGetArgMem(char*, buf)
        m3ApiGetArg(int32_t, buf_len) m3ApiGetArgMem(uint32_t*, parent_ptr)
            m3ApiGetArgMem(wasm_proc_stats_t*, stats_ptr)

                if (index < 0 || buf_len <= 0) {
        m3ApiReturn(WASMOS_INVAL);
    }
    m3ApiCheckMem(buf, (uint32_t)buf_len);
    m3ApiCheckMem(parent_ptr, sizeof(uint32_t));
    m3ApiCheckMem(stats_ptr, sizeof(wasm_proc_stats_t));
    process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_NO_CALLER);
    }
    uint64_t buf_user = 0;
    uint64_t parent_user = 0;
    uint64_t stats_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), buf, (uint32_t)buf_len,
                                   &buf_user) != 0 ||
        mm_user_range_permitted(proc->context_id, buf_user, (uint64_t)(uint32_t)buf_len,
                                MEM_REGION_FLAG_WRITE) != 0 ||
        wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), parent_ptr,
                                   sizeof(uint32_t), &parent_user) != 0 ||
        mm_user_range_permitted(proc->context_id, parent_user, sizeof(uint32_t),
                                MEM_REGION_FLAG_WRITE) != 0 ||
        wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), stats_ptr,
                                   sizeof(wasm_proc_stats_t), &stats_user) != 0 ||
        mm_user_range_permitted(proc->context_id, stats_user, sizeof(wasm_proc_stats_t),
                                MEM_REGION_FLAG_WRITE) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_BAD_POINTER);
    }

    uint32_t pid = 0;
    uint32_t parent_pid = 0;
    const char* name = 0;
    process_stats_t stats = {0};
    if (process_info_at_stats((uint32_t)index, &pid, &parent_pid, &name, &stats) != 0) {
        m3ApiReturn(WASMOS_NOENT); /* no process at that index */
    }
    wasm_proc_stats_t out_stats = {.state = stats.state,
                                   .block_reason = stats.block_reason,
                                   .thread_count = stats.thread_count,
                                   .live_thread_count = stats.live_thread_count,
                                   .current_tid = stats.current_tid,
                                   .context_id = stats.context_id,
                                   .cpu_ticks = stats.cpu_ticks,
                                   .vm_total_bytes = stats.vm_total_bytes,
                                   .thread_kstack_total_bytes = stats.thread_kstack_total_bytes,
                                   .heap_committed_bytes = stats.heap_committed_bytes,
                                   .rss_est_bytes = stats.rss_est_bytes,
                                   .last_cpu = stats.last_cpu};
    for (uint32_t i = 0; i < sizeof(out_stats.runtime_tag); ++i) {
        out_stats.runtime_tag[i] = stats.runtime_tag[i];
    }
    if (mm_copy_to_user(proc->context_id, parent_user, &parent_pid, sizeof(parent_pid)) != 0 ||
        mm_copy_to_user(proc->context_id, stats_user, &out_stats, sizeof(out_stats)) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
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
            if (mm_copy_to_user(proc->context_id, buf_user + (uint64_t)chunk_base, bounce,
                                (uint64_t)sizeof(bounce)) != 0) {
                m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
            }
        }
    }
    uint32_t tail = copied % (uint32_t)sizeof(bounce);
    if (tail > 0) {
        if (mm_copy_to_user(proc->context_id, buf_user + (uint64_t)(copied - tail), bounce,
                            (uint64_t)tail) != 0) {
            m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
        }
    }
    bounce[0] = '\0';
    if (mm_copy_to_user(proc->context_id, buf_user + (uint64_t)(out_len - 1U), bounce, 1) != 0) {
        m3ApiReturn(WASMOS_ERR_KERNEL_COPY_FAILED);
    }
    m3ApiReturn((int32_t)pid);
}

m3ApiRawFunction(wasmos_strlen) {
    m3ApiReturnType(int32_t) m3ApiGetArgMem(const char*, ptr)

        process_t* proc = process_get(process_current_pid());
    if (!proc || proc->context_id == 0) {
        m3ApiReturn(0);
    }
    uint64_t ptr_user = 0;
    if (wasm_user_va_from_host_ptr(proc->context_id, (const uint8_t*)_mem,
                                   (uint64_t)m3_GetMemorySize(runtime), ptr, 1, &ptr_user) != 0 ||
        mm_user_range_permitted(proc->context_id, ptr_user, 1, MEM_REGION_FLAG_READ) != 0) {
        m3ApiReturn(0);
    }

    const uint8_t* start = (const uint8_t*)ptr;
    const uint8_t* end = (const uint8_t*)_mem + m3_GetMemorySize(runtime);
    if ((const uint8_t*)ptr < (const uint8_t*)_mem || start >= end) {
        m3ApiReturn(0);
    }
    int32_t len = 0;
    uint64_t max_len = (uint64_t)(end - start);
    for (uint64_t i = 0; i < max_len; ++i) {
        char ch = 0;
        if (wasm_copy_from_user_bytes(proc->context_id, ptr_user + i, &ch, 1) != 0) {
            m3ApiReturn(0);
        }
        if (ch == '\0') {
            break;
        }
        len++;
    }
    m3ApiReturn(len);
}

m3ApiRawFunction(wasmos_futex_wait) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, addr) m3ApiGetArg(int32_t, expected)
        m3ApiGetArg(int32_t, timeout_ms) uint32_t context_id = 0;
    if (current_process_context(&context_id) != 0) {
        /* Named, not a bare -1: futex_wait's own returns are packed codes, and
         * a guest cannot act on a value it cannot tell apart from a timeout. */
        m3ApiReturn(IPC_ERR_INVALID);
    }
    int result = futex_wait((uint32_t)addr, (uint32_t)expected, (uint32_t)timeout_ms, context_id);
    m3ApiReturn((int32_t)result);
}

m3ApiRawFunction(wasmos_futex_wake) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, addr) m3ApiGetArg(int32_t, count)
        uint32_t context_id = 0;
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(0);
    }
    int woken = futex_wake((uint32_t)addr, (uint32_t)count, context_id);
    m3ApiReturn((int32_t)woken);
}

m3ApiRawFunction(wasmos_env_abort) {
    m3ApiReturnType(void)(void) raw_return;
    m3ApiGetArg(int32_t, msg) m3ApiGetArg(int32_t, file) m3ApiGetArg(int32_t, line)
        m3ApiGetArg(int32_t, column)(void) msg;
    (void)file;
    (void)line;
    (void)column;

    process_t* proc = process_get(process_current_pid());
    if (proc) {
        process_set_exit_status(proc, -1);
        process_yield(PROCESS_RUN_EXITED);
    }
    m3ApiSuccess();
}

static void wasm3_link_error(const char* name, const char* res) {
    klog_write("[wasm3] link failed ");
    klog_write(name);
    klog_write(": ");
    klog_write(res ? res : "unknown");
    klog_write("\n");
}

static int wasm3_link_raw(IM3Module module, const char* mod, const char* name, const char* sig,
                          M3RawCall fn) {
    M3Result res = m3_LinkRawFunction(module, mod, name, sig, fn);
    if (res && res != m3Err_functionLookupFailed) {
        wasm3_link_error(name, res);
        return -1;
    }
    return 0;
}

void wasm3_link_init(const boot_info_t* boot_info) {
    g_wasm_boot_info = boot_info;
    wasm_ipc_slots_init();
}

int wasm3_link_wasmos(IM3Module module) {
    if (!module) {
        return -1;
    }
    int rc = 0;
    /* Host-call links generated from abi/hostcalls.yaml
     * (scripts/gen_abi_hostcalls.py); env/wasi links stay hand-written
     * in wasm3_link_env below. */
#define X(mod, name, sig, fn) rc |= wasm3_link_raw(module, mod, name, sig, fn);
    WASMOS_WASM3_LINKS(X)
#undef X
    if (rc != 0) {
        klog_write("[kernel] wasm3 link errors\n");
        return -1;
    }
    return 0;
}

int wasm3_link_env(IM3Module module) {
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
