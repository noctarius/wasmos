#ifndef WASMOS_PROCESS_MANAGER_BUFFER_STATE_H
#define WASMOS_PROCESS_MANAGER_BUFFER_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t borrow_active;
    uint8_t borrow_flags;
    uint32_t borrow_source_context_id;
    uint8_t dma_mapped;
    uint32_t dma_direction_flags;
    uint32_t dma_offset;
    uint32_t dma_length;
} process_manager_buffer_state_t;

void process_manager_buffer_state_clear(process_manager_buffer_state_t *state);
int process_manager_buffer_state_borrow_from_source(process_manager_buffer_state_t *state,
                                                    uint32_t source_context_id,
                                                    uint32_t flags,
                                                    uint32_t allowed_flags);
int process_manager_buffer_state_borrow_local(process_manager_buffer_state_t *state,
                                              uint32_t flags,
                                              uint32_t allowed_flags);
int process_manager_buffer_state_release(process_manager_buffer_state_t *state);
void process_manager_buffer_state_drop_if_borrowed_from(process_manager_buffer_state_t *state,
                                                        uint32_t source_context_id);
int process_manager_buffer_state_dma_map(process_manager_buffer_state_t *state,
                                         uint32_t source_context_id,
                                         uint32_t buffer_size,
                                         uint32_t offset,
                                         uint32_t length,
                                         uint32_t direction_flags);
int process_manager_buffer_state_dma_sync(const process_manager_buffer_state_t *state,
                                          uint32_t offset,
                                          uint32_t length);
int process_manager_buffer_state_dma_unmap(process_manager_buffer_state_t *state,
                                           uint32_t source_context_id);

#ifdef __cplusplus
}
#endif

#endif
