#ifndef WASMOS_PROCESS_MANAGER_BUFFER_ROUTING_H
#define WASMOS_PROCESS_MANAGER_BUFFER_ROUTING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t process_manager_fs_buffer_target_context(uint8_t caller_is_fs_manager,
                                                  uint32_t caller_context_id,
                                                  uint32_t borrowed_source_context_id,
                                                  uint8_t peer_valid,
                                                  uint32_t peer_context_id,
                                                  uint8_t peer_is_fs_manager);

#ifdef __cplusplus
}
#endif

#endif
