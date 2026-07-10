/* process_manager_buffers.c - PM shared DMA buffer management.
 * Manages the borrow/release lifecycle for PM_BUFFER_KIND_FILESYSTEM and
 * PM_BUFFER_KIND_FRAMEBUFFER shared memory.  Only one active borrow per
 * (kind, context) at a time is enforced to prevent concurrent DMA corruption. */
#include "process_manager.h"
#include "process_manager_internal.h"
#include "framebuffer.h"
#include "physmem.h"
#include "paging.h"
#include "list.h"
#include "process_manager_buffer_policy.h"
#include "process_manager_buffer_state.h"
#include "spinlock.h"
#include "string.h"

typedef struct {
    uint8_t in_use;
    uint32_t context_id;
    uint64_t buffer_phys;
    process_manager_buffer_state_t state;
} pm_xfer_buffer_slot_t;

static list_t g_pm_fs_slots;
static list_t g_pm_fb_slots;
static uint8_t g_pm_slots_initialized;
static spinlock_t g_pm_slots_lock;

static int
pm_slots_init_once_locked(void)
{
    if (g_pm_slots_initialized) {
        return 0;
    }
    if (list_init(&g_pm_fs_slots, (uint32_t)sizeof(pm_xfer_buffer_slot_t), LIST_IMPL_ARRAY_CHUNK, 16) != 0) {
        return -1;
    }
    if (list_init(&g_pm_fb_slots, (uint32_t)sizeof(pm_xfer_buffer_slot_t), LIST_IMPL_ARRAY_CHUNK, 16) != 0) {
        return -1;
    }
    g_pm_slots_initialized = 1;
    return 0;
}

static pm_xfer_buffer_slot_t *
pm_fs_slot_find_iter(uint32_t context_id, list_iter_t *out_iter)
{
    list_iter_t it;
    pm_xfer_buffer_slot_t *slot = 0;
    if (pm_slots_init_once_locked() != 0 || context_id == 0) {
        return 0;
    }
    slot = (pm_xfer_buffer_slot_t *)list_first(&g_pm_fs_slots, &it);
    while (slot) {
        if (slot->in_use && slot->context_id == context_id) {
            if (out_iter) {
                *out_iter = it;
            }
            return slot;
        }
        slot = (pm_xfer_buffer_slot_t *)list_next(&it);
    }
    return 0;
}

static pm_xfer_buffer_slot_t *
pm_fb_slot_find_iter(uint32_t context_id, list_iter_t *out_iter)
{
    list_iter_t it;
    pm_xfer_buffer_slot_t *slot = 0;
    if (pm_slots_init_once_locked() != 0 || context_id == 0) {
        return 0;
    }
    slot = (pm_xfer_buffer_slot_t *)list_first(&g_pm_fb_slots, &it);
    while (slot) {
        if (slot->in_use && slot->context_id == context_id) {
            if (out_iter) {
                *out_iter = it;
            }
            return slot;
        }
        slot = (pm_xfer_buffer_slot_t *)list_next(&it);
    }
    return 0;
}

static pm_xfer_buffer_slot_t *
pm_fs_slot_for_context(uint32_t context_id)
{
    pm_xfer_buffer_slot_t *slot = 0;
    const uint64_t page_size = 4096u;
    const uint64_t pages = PM_XFER_BUFFER_SIZE / page_size;

    if (pm_slots_init_once_locked() != 0 || context_id == 0) {
        return 0;
    }
    slot = pm_fs_slot_find_iter(context_id, 0);
    if (slot) {
        return slot;
    }
    slot = (pm_xfer_buffer_slot_t *)list_alloc(&g_pm_fs_slots);
    if (!slot) {
        return 0;
    }
    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    slot->context_id = context_id;
    slot->buffer_phys = pfa_alloc_pages(pages);
    if (slot->buffer_phys == 0) {
        (void)list_remove(&g_pm_fs_slots, slot);
        return 0;
    }
    return slot;
}

static pm_xfer_buffer_slot_t *
pm_fs_slot_find(uint32_t context_id)
{
    return pm_fs_slot_find_iter(context_id, 0);
}

static void *
pm_xfer_buffer_for_context(uint32_t context_id)
{
    pm_xfer_buffer_slot_t *slot = pm_fs_slot_for_context(context_id);
    if (!slot) {
        return 0;
    }
    if (slot->state.borrow_active && slot->state.borrow_source_context_id != 0) {
        pm_xfer_buffer_slot_t *source = pm_fs_slot_for_context(slot->state.borrow_source_context_id);
        if (source) {
            return (void *)(uintptr_t)(source->buffer_phys | KERNEL_HIGHER_HALF_BASE);
        }
    }
    return (void *)(uintptr_t)(slot->buffer_phys | KERNEL_HIGHER_HALF_BASE);
}

static uint32_t
pm_xfer_buffer_size(void)
{
    return PM_XFER_BUFFER_SIZE;
}

static int
pm_xfer_buffer_borrow_context(uint32_t borrower_context_id,
                            uint32_t source_context_id,
                            uint32_t flags)
{
    pm_xfer_buffer_slot_t *borrower = 0;
    pm_xfer_buffer_slot_t *source = 0;

    if (process_manager_buffer_policy_validate_borrow(PM_BUFFER_KIND_FILESYSTEM,
                                                      borrower_context_id,
                                                      source_context_id,
                                                      flags) != 0) {
        return -1;
    }
    borrower = pm_fs_slot_for_context(borrower_context_id);
    source = pm_fs_slot_for_context(source_context_id);
    if (!borrower || !source) {
        return -1;
    }
    return process_manager_buffer_state_borrow_from_source(&borrower->state,
                                                           source_context_id,
                                                           flags,
                                                           process_manager_buffer_policy_allowed_flags(
                                                               PM_BUFFER_KIND_FILESYSTEM));
}

static int
pm_xfer_buffer_release_context(uint32_t borrower_context_id)
{
    pm_xfer_buffer_slot_t *borrower = pm_fs_slot_find(borrower_context_id);
    if (!borrower) {
        return -1;
    }
    return process_manager_buffer_state_release(&borrower->state);
}

static uint32_t
pm_xfer_buffer_borrow_flags(uint32_t context_id)
{
    pm_xfer_buffer_slot_t *slot = pm_fs_slot_find(context_id);
    if (!slot || !slot->state.borrow_active) {
        return 0;
    }
    return (uint32_t)(slot->state.borrow_flags & 0x3u);
}

static pm_xfer_buffer_slot_t *
pm_fb_slot_for_context(uint32_t context_id)
{
    pm_xfer_buffer_slot_t *slot = 0;

    if (pm_slots_init_once_locked() != 0 || context_id == 0) {
        return 0;
    }
    slot = pm_fb_slot_find_iter(context_id, 0);
    if (slot) {
        return slot;
    }
    slot = (pm_xfer_buffer_slot_t *)list_alloc(&g_pm_fb_slots);
    if (!slot) {
        return 0;
    }
    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    slot->context_id = context_id;
    return slot;
}

static pm_xfer_buffer_slot_t *
pm_fb_slot_find(uint32_t context_id)
{
    return pm_fb_slot_find_iter(context_id, 0);
}

static void *
pm_fb_buffer_for_context(uint32_t context_id)
{
    pm_xfer_buffer_slot_t *slot = pm_fb_slot_find(context_id);
    framebuffer_info_t fb_info = {0};
    if (!slot || !slot->state.borrow_active || slot->state.borrow_source_context_id != 0) {
        return 0;
    }
    if (framebuffer_get_info(&fb_info) != 0 || fb_info.framebuffer_base == 0) {
        return 0;
    }
    return (void *)(uintptr_t)fb_info.framebuffer_base;
}

static uint32_t
pm_fb_buffer_size(void)
{
    framebuffer_info_t fb_info = {0};
    if (framebuffer_get_info(&fb_info) != 0) {
        return 0;
    }
    if (fb_info.framebuffer_size > 0xFFFFFFFFu) {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)fb_info.framebuffer_size;
}

static uint32_t
pm_buffer_size_locked(uint32_t kind)
{
    if (kind == PM_BUFFER_KIND_FILESYSTEM) {
        return pm_xfer_buffer_size();
    }
    if (kind == PM_BUFFER_KIND_FRAMEBUFFER) {
        return pm_fb_buffer_size();
    }
    return 0;
}

static uint64_t
pm_buffer_phys_for_context_locked(uint32_t kind, uint32_t context_id)
{
    if (kind == PM_BUFFER_KIND_FILESYSTEM) {
        pm_xfer_buffer_slot_t *slot = pm_fs_slot_for_context(context_id);
        if (!slot) {
            return 0;
        }
        if (slot->state.borrow_active && slot->state.borrow_source_context_id != 0) {
            pm_xfer_buffer_slot_t *source = pm_fs_slot_for_context(slot->state.borrow_source_context_id);
            return source ? source->buffer_phys : 0;
        }
        return slot->buffer_phys;
    }
    if (kind == PM_BUFFER_KIND_FRAMEBUFFER) {
        framebuffer_info_t fb_info = {0};
        if (framebuffer_get_info(&fb_info) != 0) {
            return 0;
        }
        return fb_info.framebuffer_base;
    }
    return 0;
}

static int
pm_fb_buffer_borrow_context(uint32_t borrower_context_id,
                            uint32_t source_context_id,
                            uint32_t flags)
{
    pm_xfer_buffer_slot_t *borrower = 0;
    if (process_manager_buffer_policy_validate_borrow(PM_BUFFER_KIND_FRAMEBUFFER,
                                                      borrower_context_id,
                                                      source_context_id,
                                                      flags) != 0) {
        return -1;
    }
    borrower = pm_fb_slot_for_context(borrower_context_id);
    if (!borrower) {
        return -1;
    }
    return process_manager_buffer_state_borrow_local(&borrower->state,
                                                     flags,
                                                     process_manager_buffer_policy_allowed_flags(
                                                         PM_BUFFER_KIND_FRAMEBUFFER));
}

static int
pm_fb_buffer_release_context(uint32_t borrower_context_id)
{
    pm_xfer_buffer_slot_t *borrower = pm_fb_slot_find(borrower_context_id);
    if (!borrower) {
        return -1;
    }
    return process_manager_buffer_state_release(&borrower->state);
}

static uint32_t
pm_fb_buffer_borrow_flags(uint32_t context_id)
{
    pm_xfer_buffer_slot_t *slot = pm_fb_slot_find(context_id);
    if (!slot || !slot->state.borrow_active) {
        return 0;
    }
    return (uint32_t)(slot->state.borrow_flags & (PM_BUFFER_BORROW_READ | PM_BUFFER_BORROW_WRITE));
}

void *
process_manager_buffer_for_context(uint32_t kind, uint32_t context_id)
{
    void *buffer = 0;
    spinlock_lock(&g_pm_slots_lock);
    if (kind == PM_BUFFER_KIND_FILESYSTEM) {
        buffer = pm_xfer_buffer_for_context(context_id);
    } else if (kind == PM_BUFFER_KIND_FRAMEBUFFER) {
        buffer = pm_fb_buffer_for_context(context_id);
    }
    spinlock_unlock(&g_pm_slots_lock);
    return buffer;
}

uint64_t
process_manager_buffer_phys_for_context(uint32_t kind, uint32_t context_id)
{
    uint64_t phys = 0;
    spinlock_lock(&g_pm_slots_lock);
    phys = pm_buffer_phys_for_context_locked(kind, context_id);
    spinlock_unlock(&g_pm_slots_lock);
    return phys;
}

uint32_t
process_manager_buffer_size(uint32_t kind)
{
    if (kind == PM_BUFFER_KIND_FILESYSTEM) {
        return pm_xfer_buffer_size();
    }
    if (kind == PM_BUFFER_KIND_FRAMEBUFFER) {
        return pm_fb_buffer_size();
    }
    return 0;
}

int
process_manager_buffer_borrow_context(uint32_t kind,
                                      uint32_t borrower_context_id,
                                      uint32_t source_context_id,
                                      uint32_t flags)
{
    int rc = -1;
    spinlock_lock(&g_pm_slots_lock);
    if (kind == PM_BUFFER_KIND_FILESYSTEM) {
        rc = pm_xfer_buffer_borrow_context(borrower_context_id, source_context_id, flags);
    } else if (kind == PM_BUFFER_KIND_FRAMEBUFFER) {
        rc = pm_fb_buffer_borrow_context(borrower_context_id, source_context_id, flags);
    }
    spinlock_unlock(&g_pm_slots_lock);
    return rc;
}

int
process_manager_buffer_release_context(uint32_t kind, uint32_t borrower_context_id)
{
    int rc = -1;
    spinlock_lock(&g_pm_slots_lock);
    if (kind == PM_BUFFER_KIND_FILESYSTEM) {
        rc = pm_xfer_buffer_release_context(borrower_context_id);
    } else if (kind == PM_BUFFER_KIND_FRAMEBUFFER) {
        rc = pm_fb_buffer_release_context(borrower_context_id);
    }
    spinlock_unlock(&g_pm_slots_lock);
    return rc;
}

uint32_t
process_manager_buffer_borrow_flags(uint32_t kind, uint32_t context_id)
{
    uint32_t flags = 0;
    spinlock_lock(&g_pm_slots_lock);
    if (kind == PM_BUFFER_KIND_FILESYSTEM) {
        flags = pm_xfer_buffer_borrow_flags(context_id);
    } else if (kind == PM_BUFFER_KIND_FRAMEBUFFER) {
        flags = pm_fb_buffer_borrow_flags(context_id);
    }
    spinlock_unlock(&g_pm_slots_lock);
    return flags;
}

static pm_xfer_buffer_slot_t *
pm_slot_find_by_kind(uint32_t kind, uint32_t context_id)
{
    if (kind == PM_BUFFER_KIND_FILESYSTEM) {
        return pm_fs_slot_find(context_id);
    }
    if (kind == PM_BUFFER_KIND_FRAMEBUFFER) {
        return pm_fb_slot_find(context_id);
    }
    return 0;
}

uint32_t
process_manager_buffer_borrow_source_context(uint32_t kind, uint32_t borrower_context_id)
{
    uint32_t source_context_id = 0;
    spinlock_lock(&g_pm_slots_lock);
    pm_xfer_buffer_slot_t *slot = pm_slot_find_by_kind(kind, borrower_context_id);
    if (slot && slot->state.borrow_active) {
        source_context_id = slot->state.borrow_source_context_id;
    }
    spinlock_unlock(&g_pm_slots_lock);
    return source_context_id;
}

void
process_manager_buffer_drop_context(uint32_t context_id)
{
    const uint64_t page_size = 4096u;
    const uint64_t fs_pages = PM_XFER_BUFFER_SIZE / page_size;
    list_iter_t it;
    pm_xfer_buffer_slot_t *slot = 0;

    if (context_id == 0) {
        return;
    }

    spinlock_lock(&g_pm_slots_lock);
    if (pm_slots_init_once_locked() != 0) {
        spinlock_unlock(&g_pm_slots_lock);
        return;
    }

    slot = (pm_xfer_buffer_slot_t *)list_first(&g_pm_fs_slots, &it);
    while (slot) {
        process_manager_buffer_state_drop_if_borrowed_from(&slot->state, context_id);
        slot = (pm_xfer_buffer_slot_t *)list_next(&it);
    }

    while ((slot = pm_fs_slot_find(context_id)) != 0) {
        if (slot->buffer_phys != 0) {
            pfa_free_pages(slot->buffer_phys, fs_pages);
        }
        (void)list_remove(&g_pm_fs_slots, slot);
    }

    while ((slot = pm_fb_slot_find(context_id)) != 0) {
        (void)list_remove(&g_pm_fb_slots, slot);
    }
    spinlock_unlock(&g_pm_slots_lock);
}

int
process_manager_buffer_dma_map(uint32_t kind,
                               uint32_t borrower_context_id,
                               uint32_t source_context_id,
                               uint32_t offset,
                               uint32_t length,
                               uint32_t direction_flags,
                               uint64_t *out_device_addr)
{
    pm_xfer_buffer_slot_t *slot = 0;
    uint32_t buffer_size = 0;
    uint64_t addr = 0;

    spinlock_lock(&g_pm_slots_lock);
    slot = pm_slot_find_by_kind(kind, borrower_context_id);
    buffer_size = pm_buffer_size_locked(kind);
    if (!slot || !out_device_addr) {
        spinlock_unlock(&g_pm_slots_lock);
        return -1;
    }
    if (process_manager_buffer_state_dma_map(&slot->state,
                                             source_context_id,
                                             buffer_size,
                                             offset,
                                             length,
                                             direction_flags) != 0) {
        spinlock_unlock(&g_pm_slots_lock);
        return -1;
    }
    addr = pm_buffer_phys_for_context_locked(kind, borrower_context_id);
    if (addr == 0) {
        spinlock_unlock(&g_pm_slots_lock);
        return -1;
    }
    addr += (uint64_t)offset;
    *out_device_addr = addr;
    spinlock_unlock(&g_pm_slots_lock);
    return 0;
}

int
process_manager_buffer_dma_sync(uint32_t kind,
                                uint32_t borrower_context_id,
                                uint32_t offset,
                                uint32_t length,
                                uint32_t sync_op)
{
    pm_xfer_buffer_slot_t *slot = 0;
    (void)sync_op;
    spinlock_lock(&g_pm_slots_lock);
    slot = pm_slot_find_by_kind(kind, borrower_context_id);
    if (process_manager_buffer_state_dma_sync(slot ? &slot->state : 0,
                                              offset,
                                              length) != 0) {
        spinlock_unlock(&g_pm_slots_lock);
        return -1;
    }
    /* Cache maintenance is currently a no-op on the baseline x86 target.
     * This call still enforces map-state/range semantics for correctness. */
    spinlock_unlock(&g_pm_slots_lock);
    return 0;
}

int
process_manager_buffer_dma_unmap(uint32_t kind,
                                 uint32_t borrower_context_id,
                                 uint32_t source_context_id)
{
    pm_xfer_buffer_slot_t *slot = 0;
    spinlock_lock(&g_pm_slots_lock);
    slot = pm_slot_find_by_kind(kind, borrower_context_id);
    if (process_manager_buffer_state_dma_unmap(slot ? &slot->state : 0,
                                               source_context_id) != 0) {
        spinlock_unlock(&g_pm_slots_lock);
        return -1;
    }
    spinlock_unlock(&g_pm_slots_lock);
    return 0;
}
