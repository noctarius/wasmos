#include "process_manager_buffer_routing.h"

uint32_t
process_manager_fs_buffer_target_context(uint8_t caller_is_fs_manager,
                                         uint32_t caller_context_id,
                                         uint32_t borrowed_source_context_id,
                                         uint8_t peer_valid,
                                         uint32_t peer_context_id,
                                         uint8_t peer_is_fs_manager)
{
    if (caller_is_fs_manager) {
        return borrowed_source_context_id != 0 ? borrowed_source_context_id : caller_context_id;
    }
    if (peer_valid && peer_context_id != 0 && !peer_is_fs_manager) {
        return peer_context_id;
    }
    return caller_context_id;
}
