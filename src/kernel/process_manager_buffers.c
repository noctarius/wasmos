#include "process_manager.h"
#include "process_manager_internal.h"
#include "physmem.h"

typedef struct {
    uint8_t in_use;
    uint32_t context_id;
    uint64_t buffer_phys;
    uint8_t borrow_active;
    uint8_t borrow_flags;
    uint32_t borrow_source_context_id;
} pm_fs_buffer_slot_t;

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
    empty->borrow_active = 0;
    empty->borrow_flags = 0;
    empty->borrow_source_context_id = 0;
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
    borrower->borrow_active = 0;
    borrower->borrow_source_context_id = 0;
    borrower->borrow_flags = 0;
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

void *
process_manager_buffer_for_context(uint32_t kind, uint32_t context_id)
{
    if (kind == PM_BUFFER_KIND_FS) {
        return pm_fs_buffer_for_context(context_id);
    }
    return 0;
}

uint32_t
process_manager_buffer_size(uint32_t kind)
{
    if (kind == PM_BUFFER_KIND_FS) {
        return pm_fs_buffer_size();
    }
    return 0;
}

int
process_manager_buffer_borrow_context(uint32_t kind,
                                      uint32_t borrower_context_id,
                                      uint32_t source_context_id,
                                      uint32_t flags)
{
    if (kind == PM_BUFFER_KIND_FS) {
        return pm_fs_buffer_borrow_context(borrower_context_id, source_context_id, flags);
    }
    return -1;
}

int
process_manager_buffer_release_context(uint32_t kind, uint32_t borrower_context_id)
{
    if (kind == PM_BUFFER_KIND_FS) {
        return pm_fs_buffer_release_context(borrower_context_id);
    }
    return -1;
}

uint32_t
process_manager_buffer_borrow_flags(uint32_t kind, uint32_t context_id)
{
    if (kind == PM_BUFFER_KIND_FS) {
        return pm_fs_buffer_borrow_flags(context_id);
    }
    return 0;
}
