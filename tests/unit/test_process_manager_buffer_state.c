#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "process_manager_buffer_state.h"

static void
test_borrow_from_source_rejects_second_active_borrow(void)
{
    process_manager_buffer_state_t state = {0};

    assert(process_manager_buffer_state_borrow_from_source(&state, 41u, 0x1u, 0x3u) == 0);
    assert(state.borrow_active == 1u);
    assert(state.borrow_source_context_id == 41u);
    assert(process_manager_buffer_state_borrow_from_source(&state, 77u, 0x2u, 0x3u) != 0);
    assert(state.borrow_source_context_id == 41u);
    assert(state.borrow_flags == 0x1u);
}

static void
test_release_requires_active_borrow(void)
{
    process_manager_buffer_state_t state = {0};

    assert(process_manager_buffer_state_release(&state) != 0);
    assert(state.borrow_active == 0u);
}

static void
test_release_rejects_dma_mapped_borrow(void)
{
    process_manager_buffer_state_t state = {0};

    assert(process_manager_buffer_state_borrow_from_source(&state, 9u, 0x3u, 0x3u) == 0);
    assert(process_manager_buffer_state_dma_map(&state, 9u, 4096u, 16u, 32u, 0x3u) == 0);
    assert(process_manager_buffer_state_release(&state) != 0);
    assert(state.borrow_active == 1u);
    assert(state.dma_mapped == 1u);
}

static void
test_drop_if_borrowed_from_clears_borrow_and_dma_state(void)
{
    process_manager_buffer_state_t state = {0};

    assert(process_manager_buffer_state_borrow_from_source(&state, 19u, 0x3u, 0x3u) == 0);
    assert(process_manager_buffer_state_dma_map(&state, 19u, 4096u, 64u, 128u, 0x1u) == 0);
    process_manager_buffer_state_drop_if_borrowed_from(&state, 19u);
    assert(state.borrow_active == 0u);
    assert(state.borrow_flags == 0u);
    assert(state.borrow_source_context_id == 0u);
    assert(state.dma_mapped == 0u);
    assert(state.dma_direction_flags == 0u);
    assert(state.dma_offset == 0u);
    assert(state.dma_length == 0u);
}

static void
test_dma_unmap_keeps_borrow_until_release(void)
{
    process_manager_buffer_state_t state = {0};

    assert(process_manager_buffer_state_borrow_from_source(&state, 33u, 0x1u, 0x3u) == 0);
    assert(process_manager_buffer_state_dma_map(&state, 33u, 4096u, 128u, 64u, 0x1u) == 0);
    assert(process_manager_buffer_state_dma_unmap(&state, 33u) == 0);
    assert(state.borrow_active == 1u);
    assert(state.borrow_source_context_id == 33u);
    assert(state.borrow_flags == 0x1u);
    assert(state.dma_mapped == 0u);
    assert(state.dma_direction_flags == 0u);
    assert(state.dma_offset == 0u);
    assert(state.dma_length == 0u);
}

int
main(void)
{
    test_borrow_from_source_rejects_second_active_borrow();
    test_release_requires_active_borrow();
    test_release_rejects_dma_mapped_borrow();
    test_drop_if_borrowed_from_clears_borrow_and_dma_state();
    test_dma_unmap_keeps_borrow_until_release();
    printf("test_process_manager_buffer_state: ok\n");
    return 0;
}
