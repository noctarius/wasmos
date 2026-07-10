#ifndef WASMOS_PROCESS_MANAGER_BUFFER_POLICY_H
#define WASMOS_PROCESS_MANAGER_BUFFER_POLICY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t process_manager_buffer_policy_allowed_flags(uint32_t kind);
int process_manager_buffer_policy_validate_borrow(uint32_t kind,
                                                  uint32_t borrower_context_id,
                                                  uint32_t source_context_id,
                                                  uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif
