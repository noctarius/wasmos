#include "hostcall_buffer.h"

wasmos_error_code_t hostcall_name_clamp(const char* name, uint32_t name_max, uint32_t out_size,
                                        uint32_t* true_len, uint32_t* copy_len) {
    if (!name || !true_len || !copy_len) {
        return WASMOS_INVAL;
    }
    *true_len = 0;
    *copy_len = 0;
    /* Checked BEFORE `out_size - 1` is evaluated: a zero-sized buffer has no
     * room even for the terminator, and the subtraction would wrap to
     * 0xFFFFFFFF rather than clamp. */
    if (out_size == 0) {
        return WASMOS_INVAL;
    }

    uint32_t len = 0;
    while (len < name_max && name[len] != '\0') {
        len++;
    }
    *true_len = len;
    if (len >= out_size) {
        len = out_size - 1u;
    }
    *copy_len = len;
    return WASMOS_OK;
}
