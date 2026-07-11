#include "xfer_buffer.h"
#include "policy.h"

uint32_t
xfer_buffer_policy_allowed_flags(uint32_t kind)
{
    if (kind == BUFFER_KIND_TRANSFER || kind == BUFFER_KIND_FRAMEBUFFER) {
        return BUFFER_BORROW_READ | BUFFER_BORROW_WRITE;
    }
    return 0;
}

int
xfer_buffer_policy_validate_borrow(uint32_t kind,
                                   uint32_t borrower_context_id,
                                   uint32_t source_context_id,
                                   uint32_t flags)
{
    uint32_t allowed_flags = xfer_buffer_policy_allowed_flags(kind);

    if (borrower_context_id == 0 || allowed_flags == 0 ||
        flags == 0 || (flags & allowed_flags) != flags) {
        return -1;
    }

    if (kind == BUFFER_KIND_TRANSFER) {
        if (source_context_id == 0 || source_context_id == borrower_context_id) {
            return -1;
        }
        return 0;
    }

    if (kind == BUFFER_KIND_FRAMEBUFFER) {
        if (source_context_id != 0) {
            return -1;
        }
        return 0;
    }

    return -1;
}
