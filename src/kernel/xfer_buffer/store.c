/* xfer_buffer/store.c - transfer-buffer object store and current slot model.
 *
 * This file implements the current per-context slot model for transfer-buffer
 * borrows. A borrow redirects a borrower's access to a source context's buffer
 * object with explicit flags; it does not allocate a second implicit reply
 * buffer.
 *
 * Current limitation:
 * only one active borrow state is tracked per (kind, borrower_context_id).
 * That makes the implementation narrower than the architectural borrow model
 * and is known to constrain brokered request/reply workflows that want
 * simultaneous read and write borrows.
 */
#include "store.h"

#include "process_manager_internal.h"
#include "framebuffer.h"
#include "physmem.h"
#include "paging.h"
#include "list.h"
#include "sync/spinlock.h"
#include "string.h"

#include "policy.h"
#include "state.h"

typedef struct {
    uint8_t in_use;
    uint32_t context_id;
    uint64_t buffer_phys;
    xfer_buffer_state_t state;
} xfer_buffer_slot_t;

static list_t g_xfer_slots;
static list_t g_fb_slots;
static uint8_t g_xfer_slots_initialized;
static ksync_spinlock_t g_xfer_slots_lock;

static int xfer_slots_init_once_locked(void);
static xfer_buffer_slot_t *xfer_slot_find_iter(uint32_t context_id, list_iter_t *out_iter);
static xfer_buffer_slot_t *fb_slot_find_iter(uint32_t context_id, list_iter_t *out_iter);
static xfer_buffer_slot_t *xfer_slot_for_context(uint32_t context_id);
static xfer_buffer_slot_t *xfer_slot_find(uint32_t context_id);
static xfer_buffer_slot_t *fb_slot_for_context(uint32_t context_id);
static xfer_buffer_slot_t *fb_slot_find(uint32_t context_id);

static int
xfer_slots_init_once_locked(void)
{
    if (g_xfer_slots_initialized) {
        return 0;
    }
    if (list_init(&g_xfer_slots, (uint32_t)sizeof(xfer_buffer_slot_t), LIST_IMPL_ARRAY_CHUNK, 16) != 0) {
        return -1;
    }
    if (list_init(&g_fb_slots, (uint32_t)sizeof(xfer_buffer_slot_t), LIST_IMPL_ARRAY_CHUNK, 16) != 0) {
        return -1;
    }
    g_xfer_slots_initialized = 1;
    return 0;
}

static xfer_buffer_slot_t *
xfer_slot_find_iter(uint32_t context_id, list_iter_t *out_iter)
{
    list_iter_t it;
    xfer_buffer_slot_t *slot = 0;
    if (xfer_slots_init_once_locked() != 0 || context_id == 0) {
        return 0;
    }
    slot = (xfer_buffer_slot_t *)list_first(&g_xfer_slots, &it);
    while (slot) {
        if (slot->in_use && slot->context_id == context_id) {
            if (out_iter) {
                *out_iter = it;
            }
            return slot;
        }
        slot = (xfer_buffer_slot_t *)list_next(&it);
    }
    return 0;
}

static xfer_buffer_slot_t *
fb_slot_find_iter(uint32_t context_id, list_iter_t *out_iter)
{
    list_iter_t it;
    xfer_buffer_slot_t *slot = 0;
    if (xfer_slots_init_once_locked() != 0 || context_id == 0) {
        return 0;
    }
    slot = (xfer_buffer_slot_t *)list_first(&g_fb_slots, &it);
    while (slot) {
        if (slot->in_use && slot->context_id == context_id) {
            if (out_iter) {
                *out_iter = it;
            }
            return slot;
        }
        slot = (xfer_buffer_slot_t *)list_next(&it);
    }
    return 0;
}

static xfer_buffer_slot_t *
xfer_slot_for_context(uint32_t context_id)
{
    xfer_buffer_slot_t *slot = 0;
    const uint64_t page_size = 4096u;
    const uint64_t pages = PM_XFER_BUFFER_SIZE / page_size;

    if (xfer_slots_init_once_locked() != 0 || context_id == 0) {
        return 0;
    }
    slot = xfer_slot_find_iter(context_id, 0);
    if (slot) {
        return slot;
    }
    slot = (xfer_buffer_slot_t *)list_alloc(&g_xfer_slots);
    if (!slot) {
        return 0;
    }
    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    slot->context_id = context_id;
    slot->buffer_phys = pfa_alloc_pages(pages);
    if (slot->buffer_phys == 0) {
        (void)list_remove(&g_xfer_slots, slot);
        return 0;
    }
    return slot;
}

static xfer_buffer_slot_t *
xfer_slot_find(uint32_t context_id)
{
    return xfer_slot_find_iter(context_id, 0);
}

static void *
xfer_buffer_for_context_locked(uint32_t context_id)
{
    xfer_buffer_slot_t *slot = xfer_slot_for_context(context_id);
    if (!slot) {
        return 0;
    }
    if (slot->state.borrow_active && slot->state.borrow_source_context_id != 0) {
        xfer_buffer_slot_t *source = xfer_slot_for_context(slot->state.borrow_source_context_id);
        if (source) {
            return (void *)(uintptr_t)(source->buffer_phys | KERNEL_HIGHER_HALF_BASE);
        }
    }
    return (void *)(uintptr_t)(slot->buffer_phys | KERNEL_HIGHER_HALF_BASE);
}

static uint32_t
xfer_buffer_size_locked(void)
{
    return PM_XFER_BUFFER_SIZE;
}

static int
xfer_buffer_borrow_locked(uint32_t borrower_context_id,
                          uint32_t source_context_id,
                          uint32_t flags)
{
    xfer_buffer_slot_t *borrower = 0;
    xfer_buffer_slot_t *source = 0;

    if (xfer_buffer_policy_validate_borrow(BUFFER_KIND_TRANSFER,
                                           borrower_context_id,
                                           source_context_id,
                                           flags) != 0) {
        return -1;
    }
    borrower = xfer_slot_for_context(borrower_context_id);
    source = xfer_slot_for_context(source_context_id);
    if (!borrower || !source) {
        return -1;
    }
    return xfer_buffer_state_borrow_from_source(&borrower->state,
                                                source_context_id,
                                                flags,
                                                xfer_buffer_policy_allowed_flags(
                                                    BUFFER_KIND_TRANSFER));
}

static int
xfer_buffer_release_locked(uint32_t borrower_context_id)
{
    xfer_buffer_slot_t *borrower = xfer_slot_find(borrower_context_id);
    if (!borrower) {
        return -1;
    }
    return xfer_buffer_state_release(&borrower->state);
}

static uint32_t
xfer_buffer_borrow_flags_locked(uint32_t context_id)
{
    xfer_buffer_slot_t *slot = xfer_slot_find(context_id);
    if (!slot || !slot->state.borrow_active) {
        return 0;
    }
    return (uint32_t)(slot->state.borrow_flags & 0x3u);
}

static xfer_buffer_slot_t *
fb_slot_for_context(uint32_t context_id)
{
    xfer_buffer_slot_t *slot = 0;

    if (xfer_slots_init_once_locked() != 0 || context_id == 0) {
        return 0;
    }
    slot = fb_slot_find_iter(context_id, 0);
    if (slot) {
        return slot;
    }
    slot = (xfer_buffer_slot_t *)list_alloc(&g_fb_slots);
    if (!slot) {
        return 0;
    }
    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    slot->context_id = context_id;
    return slot;
}

static xfer_buffer_slot_t *
fb_slot_find(uint32_t context_id)
{
    return fb_slot_find_iter(context_id, 0);
}

static void *
fb_buffer_for_context_locked(uint32_t context_id)
{
    xfer_buffer_slot_t *slot = fb_slot_find(context_id);
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
fb_buffer_size_locked(void)
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
buffer_size_by_kind_locked(uint32_t kind)
{
    if (kind == BUFFER_KIND_TRANSFER) {
        return xfer_buffer_size_locked();
    }
    if (kind == BUFFER_KIND_FRAMEBUFFER) {
        return fb_buffer_size_locked();
    }
    return 0;
}

static uint64_t
buffer_phys_for_context_locked(uint32_t kind, uint32_t context_id)
{
    if (kind == BUFFER_KIND_TRANSFER) {
        xfer_buffer_slot_t *slot = xfer_slot_for_context(context_id);
        if (!slot) {
            return 0;
        }
        if (slot->state.borrow_active && slot->state.borrow_source_context_id != 0) {
            xfer_buffer_slot_t *source = xfer_slot_for_context(slot->state.borrow_source_context_id);
            return source ? source->buffer_phys : 0;
        }
        return slot->buffer_phys;
    }
    if (kind == BUFFER_KIND_FRAMEBUFFER) {
        framebuffer_info_t fb_info = {0};
        if (framebuffer_get_info(&fb_info) != 0) {
            return 0;
        }
        return fb_info.framebuffer_base;
    }
    return 0;
}

static int
fb_buffer_borrow_locked(uint32_t borrower_context_id,
                        uint32_t source_context_id,
                        uint32_t flags)
{
    xfer_buffer_slot_t *borrower = 0;
    if (xfer_buffer_policy_validate_borrow(BUFFER_KIND_FRAMEBUFFER,
                                           borrower_context_id,
                                           source_context_id,
                                           flags) != 0) {
        return -1;
    }
    borrower = fb_slot_for_context(borrower_context_id);
    if (!borrower) {
        return -1;
    }
    return xfer_buffer_state_borrow_local(&borrower->state,
                                          flags,
                                          xfer_buffer_policy_allowed_flags(
                                              BUFFER_KIND_FRAMEBUFFER));
}

static int
fb_buffer_release_locked(uint32_t borrower_context_id)
{
    xfer_buffer_slot_t *borrower = fb_slot_find(borrower_context_id);
    if (!borrower) {
        return -1;
    }
    return xfer_buffer_state_release(&borrower->state);
}

static uint32_t
fb_buffer_borrow_flags_locked(uint32_t context_id)
{
    xfer_buffer_slot_t *slot = fb_slot_find(context_id);
    if (!slot || !slot->state.borrow_active) {
        return 0;
    }
    return (uint32_t)(slot->state.borrow_flags & (BUFFER_BORROW_READ | BUFFER_BORROW_WRITE));
}

static xfer_buffer_slot_t *
slot_find_by_kind_locked(uint32_t kind, uint32_t context_id)
{
    if (kind == BUFFER_KIND_TRANSFER) {
        return xfer_slot_find(context_id);
    }
    if (kind == BUFFER_KIND_FRAMEBUFFER) {
        return fb_slot_find(context_id);
    }
    return 0;
}

void *
xfer_buffer_for_context(uint32_t kind, uint32_t context_id)
{
    void *buffer = 0;
    ksync_spinlock_lock(&g_xfer_slots_lock);
    if (kind == BUFFER_KIND_TRANSFER) {
        buffer = xfer_buffer_for_context_locked(context_id);
    } else if (kind == BUFFER_KIND_FRAMEBUFFER) {
        buffer = fb_buffer_for_context_locked(context_id);
    }
    ksync_spinlock_unlock(&g_xfer_slots_lock);
    return buffer;
}

uint64_t
xfer_buffer_phys_for_context(uint32_t kind, uint32_t context_id)
{
    uint64_t phys = 0;
    ksync_spinlock_lock(&g_xfer_slots_lock);
    phys = buffer_phys_for_context_locked(kind, context_id);
    ksync_spinlock_unlock(&g_xfer_slots_lock);
    return phys;
}

uint32_t
xfer_buffer_size(uint32_t kind)
{
    if (kind == BUFFER_KIND_TRANSFER) {
        return xfer_buffer_size_locked();
    }
    if (kind == BUFFER_KIND_FRAMEBUFFER) {
        return fb_buffer_size_locked();
    }
    return 0;
}

int
xfer_buffer_borrow(uint32_t kind,
                   uint32_t borrower_context_id,
                   uint32_t source_context_id,
                   uint32_t flags)
{
    int rc = -1;
    ksync_spinlock_lock(&g_xfer_slots_lock);
    if (kind == BUFFER_KIND_TRANSFER) {
        rc = xfer_buffer_borrow_locked(borrower_context_id, source_context_id, flags);
    } else if (kind == BUFFER_KIND_FRAMEBUFFER) {
        rc = fb_buffer_borrow_locked(borrower_context_id, source_context_id, flags);
    }
    ksync_spinlock_unlock(&g_xfer_slots_lock);
    return rc;
}

int
xfer_buffer_release(uint32_t kind, uint32_t borrower_context_id)
{
    int rc = -1;
    ksync_spinlock_lock(&g_xfer_slots_lock);
    if (kind == BUFFER_KIND_TRANSFER) {
        rc = xfer_buffer_release_locked(borrower_context_id);
    } else if (kind == BUFFER_KIND_FRAMEBUFFER) {
        rc = fb_buffer_release_locked(borrower_context_id);
    }
    ksync_spinlock_unlock(&g_xfer_slots_lock);
    return rc;
}

uint32_t
xfer_buffer_borrow_flags(uint32_t kind, uint32_t context_id)
{
    uint32_t flags = 0;
    ksync_spinlock_lock(&g_xfer_slots_lock);
    if (kind == BUFFER_KIND_TRANSFER) {
        flags = xfer_buffer_borrow_flags_locked(context_id);
    } else if (kind == BUFFER_KIND_FRAMEBUFFER) {
        flags = fb_buffer_borrow_flags_locked(context_id);
    }
    ksync_spinlock_unlock(&g_xfer_slots_lock);
    return flags;
}

uint32_t
xfer_buffer_borrow_source_context(uint32_t kind, uint32_t borrower_context_id)
{
    uint32_t source_context_id = 0;
    ksync_spinlock_lock(&g_xfer_slots_lock);
    xfer_buffer_slot_t *slot = slot_find_by_kind_locked(kind, borrower_context_id);
    if (slot && slot->state.borrow_active) {
        source_context_id = slot->state.borrow_source_context_id;
    }
    ksync_spinlock_unlock(&g_xfer_slots_lock);
    return source_context_id;
}

void
xfer_buffer_drop_context(uint32_t context_id)
{
    const uint64_t page_size = 4096u;
    const uint64_t xfer_pages = PM_XFER_BUFFER_SIZE / page_size;
    list_iter_t it;
    xfer_buffer_slot_t *slot = 0;

    if (context_id == 0) {
        return;
    }

    ksync_spinlock_lock(&g_xfer_slots_lock);
    if (xfer_slots_init_once_locked() != 0) {
        ksync_spinlock_unlock(&g_xfer_slots_lock);
        return;
    }

    slot = (xfer_buffer_slot_t *)list_first(&g_xfer_slots, &it);
    while (slot) {
        xfer_buffer_state_drop_if_borrowed_from(&slot->state, context_id);
        slot = (xfer_buffer_slot_t *)list_next(&it);
    }

    while ((slot = xfer_slot_find(context_id)) != 0) {
        if (slot->buffer_phys != 0) {
            pfa_free_pages(slot->buffer_phys, xfer_pages);
        }
        (void)list_remove(&g_xfer_slots, slot);
    }

    while ((slot = fb_slot_find(context_id)) != 0) {
        (void)list_remove(&g_fb_slots, slot);
    }
    ksync_spinlock_unlock(&g_xfer_slots_lock);
}

int
xfer_buffer_dma_map(uint32_t kind,
                    uint32_t borrower_context_id,
                    uint32_t source_context_id,
                    uint32_t offset,
                    uint32_t length,
                    uint32_t direction_flags,
                    uint64_t *out_device_addr)
{
    xfer_buffer_slot_t *slot = 0;
    uint32_t buffer_size = 0;
    uint64_t addr = 0;

    ksync_spinlock_lock(&g_xfer_slots_lock);
    slot = slot_find_by_kind_locked(kind, borrower_context_id);
    buffer_size = buffer_size_by_kind_locked(kind);
    if (!slot || !out_device_addr) {
        ksync_spinlock_unlock(&g_xfer_slots_lock);
        return -1;
    }
    if (xfer_buffer_state_dma_map(&slot->state,
                                  source_context_id,
                                  buffer_size,
                                  offset,
                                  length,
                                  direction_flags) != 0) {
        ksync_spinlock_unlock(&g_xfer_slots_lock);
        return -1;
    }
    addr = buffer_phys_for_context_locked(kind, borrower_context_id);
    if (addr == 0) {
        ksync_spinlock_unlock(&g_xfer_slots_lock);
        return -1;
    }
    addr += (uint64_t)offset;
    *out_device_addr = addr;
    ksync_spinlock_unlock(&g_xfer_slots_lock);
    return 0;
}

int
xfer_buffer_dma_sync(uint32_t kind,
                     uint32_t borrower_context_id,
                     uint32_t offset,
                     uint32_t length,
                     uint32_t sync_op)
{
    xfer_buffer_slot_t *slot = 0;
    (void)sync_op;

    ksync_spinlock_lock(&g_xfer_slots_lock);
    slot = slot_find_by_kind_locked(kind, borrower_context_id);
    if (xfer_buffer_state_dma_sync(slot ? &slot->state : 0,
                                   offset,
                                   length) != 0) {
        ksync_spinlock_unlock(&g_xfer_slots_lock);
        return -1;
    }
    ksync_spinlock_unlock(&g_xfer_slots_lock);
    return 0;
}

int
xfer_buffer_dma_unmap(uint32_t kind,
                      uint32_t borrower_context_id,
                      uint32_t source_context_id)
{
    xfer_buffer_slot_t *slot = 0;

    ksync_spinlock_lock(&g_xfer_slots_lock);
    slot = slot_find_by_kind_locked(kind, borrower_context_id);
    if (xfer_buffer_state_dma_unmap(slot ? &slot->state : 0,
                                    source_context_id) != 0) {
        ksync_spinlock_unlock(&g_xfer_slots_lock);
        return -1;
    }
    ksync_spinlock_unlock(&g_xfer_slots_lock);
    return 0;
}
