#include "process_manager.h"
#include "process_manager_internal.h"
#include "framebuffer.h"
#include "physmem.h"

typedef struct {
    uint8_t in_use;
    uint32_t context_id;
    uint64_t buffer_phys;
    uint8_t borrow_active;
    uint8_t borrow_flags;
    uint32_t borrow_source_context_id;
    uint8_t dma_mapped;
    uint32_t dma_direction_flags;
    uint32_t dma_offset;
    uint32_t dma_length;
} pm_fs_buffer_slot_t;

static pm_fs_buffer_slot_t g_pm_fs_slots[PROCESS_MAX_COUNT];
static pm_fs_buffer_slot_t g_pm_fb_slots[PROCESS_MAX_COUNT];

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
    empty->borrow_active = 0;
    empty->borrow_flags = 0;
    empty->borrow_source_context_id = 0;
    empty->dma_mapped = 0;
    empty->dma_direction_flags = 0;
    empty->dma_offset = 0;
    empty->dma_length = 0;
    empty->buffer_phys = pfa_alloc_pages(pages);
    if (empty->buffer_phys == 0) {
        empty->in_use = 0;
        empty->context_id = 0;
        return 0;
    }
    return empty;
}

static pm_fs_buffer_slot_t *
pm_fs_slot_find(uint32_t context_id)
{
    if (context_id == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_pm_fs_slots[i].in_use && g_pm_fs_slots[i].context_id == context_id) {
            return &g_pm_fs_slots[i];
        }
    }
    return 0;
}

static void *
pm_fs_buffer_for_context(uint32_t context_id)
{
    pm_fs_buffer_slot_t *slot = pm_fs_slot_for_context(context_id);
    if (!slot) {
        return 0;
    }
    if (slot->borrow_active && slot->borrow_source_context_id != 0) {
        pm_fs_buffer_slot_t *source = pm_fs_slot_for_context(slot->borrow_source_context_id);
        if (source) {
            return (void *)(uintptr_t)source->buffer_phys;
        }
    }
    return (void *)(uintptr_t)slot->buffer_phys;
}

static uint32_t
pm_fs_buffer_size(void)
{
    return PM_FS_BUFFER_SIZE;
}

static int
pm_fs_buffer_borrow_context(uint32_t borrower_context_id,
                            uint32_t source_context_id,
                            uint32_t flags)
{
    pm_fs_buffer_slot_t *borrower = 0;
    pm_fs_buffer_slot_t *source = 0;

    if (borrower_context_id == 0 || source_context_id == 0 ||
        borrower_context_id == source_context_id || (flags & 0x3u) == 0) {
        return -1;
    }
    borrower = pm_fs_slot_for_context(borrower_context_id);
    source = pm_fs_slot_for_context(source_context_id);
    if (!borrower || !source) {
        return -1;
    }
    borrower->borrow_active = 1;
    borrower->borrow_source_context_id = source_context_id;
    borrower->borrow_flags = (uint8_t)(flags & 0x3u);
    return 0;
}

static int
pm_fs_buffer_release_context(uint32_t borrower_context_id)
{
    pm_fs_buffer_slot_t *borrower = pm_fs_slot_find(borrower_context_id);
    if (!borrower) {
        return -1;
    }
    if (borrower->dma_mapped) {
        return -1;
    }
    borrower->borrow_active = 0;
    borrower->borrow_source_context_id = 0;
    borrower->borrow_flags = 0;
    borrower->dma_mapped = 0;
    borrower->dma_direction_flags = 0;
    borrower->dma_offset = 0;
    borrower->dma_length = 0;
    return 0;
}

static uint32_t
pm_fs_buffer_borrow_flags(uint32_t context_id)
{
    pm_fs_buffer_slot_t *slot = pm_fs_slot_find(context_id);
    if (!slot || !slot->borrow_active) {
        return 0;
    }
    return (uint32_t)(slot->borrow_flags & 0x3u);
}

static pm_fs_buffer_slot_t *
pm_fb_slot_for_context(uint32_t context_id)
{
    pm_fs_buffer_slot_t *empty = 0;

    if (context_id == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_pm_fb_slots[i].in_use && g_pm_fb_slots[i].context_id == context_id) {
            return &g_pm_fb_slots[i];
        }
        if (!empty && !g_pm_fb_slots[i].in_use) {
            empty = &g_pm_fb_slots[i];
        }
    }

    if (!empty) {
        return 0;
    }
    empty->in_use = 1;
    empty->context_id = context_id;
    empty->borrow_active = 0;
    empty->borrow_flags = 0;
    empty->borrow_source_context_id = 0;
    empty->dma_mapped = 0;
    empty->dma_direction_flags = 0;
    empty->dma_offset = 0;
    empty->dma_length = 0;
    empty->buffer_phys = 0;
    return empty;
}

static pm_fs_buffer_slot_t *
pm_fb_slot_find(uint32_t context_id)
{
    if (context_id == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < PROCESS_MAX_COUNT; ++i) {
        if (g_pm_fb_slots[i].in_use && g_pm_fb_slots[i].context_id == context_id) {
            return &g_pm_fb_slots[i];
        }
    }
    return 0;
}

static void *
pm_fb_buffer_for_context(uint32_t context_id)
{
    pm_fs_buffer_slot_t *slot = pm_fb_slot_find(context_id);
    framebuffer_info_t fb_info = {0};
    if (!slot || !slot->borrow_active || slot->borrow_source_context_id != 0) {
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

static int
pm_fb_buffer_borrow_context(uint32_t borrower_context_id,
                            uint32_t source_context_id,
                            uint32_t flags)
{
    pm_fs_buffer_slot_t *borrower = 0;
    if (borrower_context_id == 0 || source_context_id != 0 ||
        (flags & (PM_BUFFER_BORROW_READ | PM_BUFFER_BORROW_WRITE)) == 0) {
        return -1;
    }
    borrower = pm_fb_slot_for_context(borrower_context_id);
    if (!borrower) {
        return -1;
    }
    borrower->borrow_active = 1;
    borrower->borrow_source_context_id = 0;
    borrower->borrow_flags = (uint8_t)(flags & (PM_BUFFER_BORROW_READ | PM_BUFFER_BORROW_WRITE));
    return 0;
}

static int
pm_fb_buffer_release_context(uint32_t borrower_context_id)
{
    pm_fs_buffer_slot_t *borrower = pm_fb_slot_find(borrower_context_id);
    if (!borrower) {
        return -1;
    }
    if (borrower->dma_mapped) {
        return -1;
    }
    borrower->borrow_active = 0;
    borrower->borrow_source_context_id = 0;
    borrower->borrow_flags = 0;
    borrower->dma_mapped = 0;
    borrower->dma_direction_flags = 0;
    borrower->dma_offset = 0;
    borrower->dma_length = 0;
    return 0;
}

static uint32_t
pm_fb_buffer_borrow_flags(uint32_t context_id)
{
    pm_fs_buffer_slot_t *slot = pm_fb_slot_find(context_id);
    if (!slot || !slot->borrow_active) {
        return 0;
    }
    return (uint32_t)(slot->borrow_flags & (PM_BUFFER_BORROW_READ | PM_BUFFER_BORROW_WRITE));
}

void *
process_manager_buffer_for_context(uint32_t kind, uint32_t context_id)
{
    if (kind == PM_BUFFER_KIND_FILESYSTEM) {
        return pm_fs_buffer_for_context(context_id);
    }
    if (kind == PM_BUFFER_KIND_FRAMEBUFFER) {
        return pm_fb_buffer_for_context(context_id);
    }
    return 0;
}

uint32_t
process_manager_buffer_size(uint32_t kind)
{
    if (kind == PM_BUFFER_KIND_FILESYSTEM) {
        return pm_fs_buffer_size();
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
    if (kind == PM_BUFFER_KIND_FILESYSTEM) {
        return pm_fs_buffer_borrow_context(borrower_context_id, source_context_id, flags);
    }
    if (kind == PM_BUFFER_KIND_FRAMEBUFFER) {
        return pm_fb_buffer_borrow_context(borrower_context_id, source_context_id, flags);
    }
    return -1;
}

int
process_manager_buffer_release_context(uint32_t kind, uint32_t borrower_context_id)
{
    if (kind == PM_BUFFER_KIND_FILESYSTEM) {
        return pm_fs_buffer_release_context(borrower_context_id);
    }
    if (kind == PM_BUFFER_KIND_FRAMEBUFFER) {
        return pm_fb_buffer_release_context(borrower_context_id);
    }
    return -1;
}

uint32_t
process_manager_buffer_borrow_flags(uint32_t kind, uint32_t context_id)
{
    if (kind == PM_BUFFER_KIND_FILESYSTEM) {
        return pm_fs_buffer_borrow_flags(context_id);
    }
    if (kind == PM_BUFFER_KIND_FRAMEBUFFER) {
        return pm_fb_buffer_borrow_flags(context_id);
    }
    return 0;
}

static pm_fs_buffer_slot_t *
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
    pm_fs_buffer_slot_t *slot = pm_slot_find_by_kind(kind, borrower_context_id);
    if (!slot || !slot->borrow_active) {
        return 0;
    }
    return slot->borrow_source_context_id;
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
    pm_fs_buffer_slot_t *slot = pm_slot_find_by_kind(kind, borrower_context_id);
    uint32_t buffer_size = process_manager_buffer_size(kind);
    void *base = 0;
    uint64_t addr = 0;

    if (!slot || !out_device_addr || length == 0 || direction_flags == 0) {
        return -1;
    }
    if (!slot->borrow_active) {
        return -1;
    }
    if (slot->borrow_source_context_id != source_context_id) {
        return -1;
    }
    if (slot->dma_mapped) {
        return -1;
    }
    if ((uint64_t)offset >= (uint64_t)buffer_size ||
        (uint64_t)length > (uint64_t)buffer_size ||
        ((uint64_t)offset + (uint64_t)length) > (uint64_t)buffer_size) {
        return -1;
    }
    base = process_manager_buffer_for_context(kind, borrower_context_id);
    if (!base) {
        return -1;
    }
    addr = (uint64_t)(uintptr_t)base + (uint64_t)offset;
    slot->dma_mapped = 1;
    slot->dma_direction_flags = direction_flags;
    slot->dma_offset = offset;
    slot->dma_length = length;
    *out_device_addr = addr;
    return 0;
}

int
process_manager_buffer_dma_sync(uint32_t kind,
                                uint32_t borrower_context_id,
                                uint32_t offset,
                                uint32_t length,
                                uint32_t sync_op)
{
    pm_fs_buffer_slot_t *slot = pm_slot_find_by_kind(kind, borrower_context_id);
    (void)sync_op;
    if (!slot || !slot->dma_mapped || length == 0) {
        return -1;
    }
    if ((uint64_t)offset > (uint64_t)slot->dma_length ||
        (uint64_t)length > (uint64_t)slot->dma_length ||
        ((uint64_t)offset + (uint64_t)length) > (uint64_t)slot->dma_length) {
        return -1;
    }
    /* Cache maintenance is currently a no-op on the baseline x86 target.
     * This call still enforces map-state/range semantics for correctness. */
    return 0;
}

int
process_manager_buffer_dma_unmap(uint32_t kind,
                                 uint32_t borrower_context_id,
                                 uint32_t source_context_id)
{
    pm_fs_buffer_slot_t *slot = pm_slot_find_by_kind(kind, borrower_context_id);
    if (!slot || !slot->dma_mapped || !slot->borrow_active) {
        return -1;
    }
    if (slot->borrow_source_context_id != source_context_id) {
        return -1;
    }
    slot->dma_mapped = 0;
    slot->dma_direction_flags = 0;
    slot->dma_offset = 0;
    slot->dma_length = 0;
    return 0;
}
