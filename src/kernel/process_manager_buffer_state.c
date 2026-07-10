#include "process_manager_buffer_state.h"

static int
pm_buffer_state_validate_flags(uint32_t flags, uint32_t allowed_flags)
{
    if (flags == 0 || allowed_flags == 0) {
        return -1;
    }
    if ((flags & allowed_flags) != flags) {
        return -1;
    }
    return 0;
}

static void
pm_buffer_state_clear_dma(process_manager_buffer_state_t *state)
{
    state->dma_mapped = 0;
    state->dma_direction_flags = 0;
    state->dma_offset = 0;
    state->dma_length = 0;
}

void
process_manager_buffer_state_clear(process_manager_buffer_state_t *state)
{
    if (!state) {
        return;
    }
    state->borrow_active = 0;
    state->borrow_flags = 0;
    state->borrow_source_context_id = 0;
    pm_buffer_state_clear_dma(state);
}

int
process_manager_buffer_state_borrow_from_source(process_manager_buffer_state_t *state,
                                                uint32_t source_context_id,
                                                uint32_t flags,
                                                uint32_t allowed_flags)
{
    if (!state || source_context_id == 0 ||
        pm_buffer_state_validate_flags(flags, allowed_flags) != 0) {
        return -1;
    }
    if (state->borrow_active || state->dma_mapped) {
        return -1;
    }
    state->borrow_active = 1;
    state->borrow_flags = (uint8_t)flags;
    state->borrow_source_context_id = source_context_id;
    pm_buffer_state_clear_dma(state);
    return 0;
}

int
process_manager_buffer_state_borrow_local(process_manager_buffer_state_t *state,
                                          uint32_t flags,
                                          uint32_t allowed_flags)
{
    if (!state || pm_buffer_state_validate_flags(flags, allowed_flags) != 0) {
        return -1;
    }
    if (state->borrow_active || state->dma_mapped) {
        return -1;
    }
    state->borrow_active = 1;
    state->borrow_flags = (uint8_t)flags;
    state->borrow_source_context_id = 0;
    pm_buffer_state_clear_dma(state);
    return 0;
}

int
process_manager_buffer_state_release(process_manager_buffer_state_t *state)
{
    if (!state || !state->borrow_active || state->dma_mapped) {
        return -1;
    }
    process_manager_buffer_state_clear(state);
    return 0;
}

void
process_manager_buffer_state_drop_if_borrowed_from(process_manager_buffer_state_t *state,
                                                   uint32_t source_context_id)
{
    if (!state || source_context_id == 0) {
        return;
    }
    if (state->borrow_active && state->borrow_source_context_id == source_context_id) {
        process_manager_buffer_state_clear(state);
    }
}

int
process_manager_buffer_state_dma_map(process_manager_buffer_state_t *state,
                                     uint32_t source_context_id,
                                     uint32_t buffer_size,
                                     uint32_t offset,
                                     uint32_t length,
                                     uint32_t direction_flags)
{
    if (!state || !state->borrow_active || state->dma_mapped ||
        direction_flags == 0 || length == 0) {
        return -1;
    }
    if (state->borrow_source_context_id != source_context_id) {
        return -1;
    }
    if ((uint64_t)offset >= (uint64_t)buffer_size ||
        (uint64_t)length > (uint64_t)buffer_size ||
        ((uint64_t)offset + (uint64_t)length) > (uint64_t)buffer_size) {
        return -1;
    }
    state->dma_mapped = 1;
    state->dma_direction_flags = direction_flags;
    state->dma_offset = offset;
    state->dma_length = length;
    return 0;
}

int
process_manager_buffer_state_dma_sync(const process_manager_buffer_state_t *state,
                                      uint32_t offset,
                                      uint32_t length)
{
    if (!state || !state->dma_mapped || length == 0) {
        return -1;
    }
    if ((uint64_t)offset > (uint64_t)state->dma_length ||
        (uint64_t)length > (uint64_t)state->dma_length ||
        ((uint64_t)offset + (uint64_t)length) > (uint64_t)state->dma_length) {
        return -1;
    }
    return 0;
}

int
process_manager_buffer_state_dma_unmap(process_manager_buffer_state_t *state,
                                       uint32_t source_context_id)
{
    if (!state || !state->borrow_active || !state->dma_mapped) {
        return -1;
    }
    if (state->borrow_source_context_id != source_context_id) {
        return -1;
    }
    pm_buffer_state_clear_dma(state);
    return 0;
}
