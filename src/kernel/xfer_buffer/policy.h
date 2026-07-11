#ifndef WASMOS_KERNEL_XFER_BUFFER_POLICY_H
#define WASMOS_KERNEL_XFER_BUFFER_POLICY_H

#include <stdint.h>

uint32_t xfer_buffer_policy_allowed_flags(uint32_t kind);
int xfer_buffer_policy_validate_borrow(uint32_t kind,
                                       uint32_t borrower_context_id,
                                       uint32_t source_context_id,
                                       uint32_t flags);

#endif
