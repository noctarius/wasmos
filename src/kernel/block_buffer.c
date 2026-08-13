#include "block_buffer.h"

wasmos_error_code_t block_buffer_check_range(uint64_t offset, uint64_t len, uint64_t buf_bytes) {
    /* 64-bit throughout, with the sum guarded against its own overflow.  With
     * the zero-extended 32-bit values callers are required to pass, `end` cannot
     * wrap; the guard holds the bound even if that contract is broken. */
    uint64_t end = offset + len;

    if (end < offset || end > buf_bytes) {
        return WASMOS_ERR_BLOCK_RANGE;
    }
    return WASMOS_OK;
}

wasmos_error_code_t block_buffer_check_phys(uint64_t phys) {
    if (phys == 0 || phys >= BLOCK_BUFFER_PHYS_LIMIT) {
        return WASMOS_ERR_BLOCK_ABOVE_4G;
    }
    return WASMOS_OK;
}
