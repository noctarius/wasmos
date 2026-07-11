#ifndef WASMOS_KERNEL_XFER_BUFFER_STATE_H
#define WASMOS_KERNEL_XFER_BUFFER_STATE_H

#include <stdint.h>

typedef struct {
    uint8_t borrow_active;
    uint8_t borrow_flags;
    uint32_t borrow_source_context_id;
    uint8_t dma_mapped;
    uint32_t dma_direction_flags;
    uint32_t dma_offset;
    uint32_t dma_length;
} xfer_buffer_state_t;

void xfer_buffer_state_clear(xfer_buffer_state_t *state);
int xfer_buffer_state_borrow_from_source(xfer_buffer_state_t *state,
                                         uint32_t source_context_id,
                                         uint32_t flags,
                                         uint32_t allowed_flags);
int xfer_buffer_state_borrow_local(xfer_buffer_state_t *state,
                                   uint32_t flags,
                                   uint32_t allowed_flags);
int xfer_buffer_state_release(xfer_buffer_state_t *state);
void xfer_buffer_state_drop_if_borrowed_from(xfer_buffer_state_t *state,
                                             uint32_t source_context_id);
int xfer_buffer_state_dma_map(xfer_buffer_state_t *state,
                              uint32_t source_context_id,
                              uint32_t buffer_size,
                              uint32_t offset,
                              uint32_t length,
                              uint32_t direction_flags);
int xfer_buffer_state_dma_sync(const xfer_buffer_state_t *state,
                               uint32_t offset,
                               uint32_t length);
int xfer_buffer_state_dma_unmap(xfer_buffer_state_t *state,
                                uint32_t source_context_id);

#endif
