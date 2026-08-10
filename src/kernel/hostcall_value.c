#include "hostcall_value.h"

wasmos_error_code_t hostcall_value_check(uint64_t value) {
    if (value > 0x7FFFFFFFULL) {
        return WASMOS_ERR_KERNEL_TOO_LARGE;
    }
    return WASMOS_OK;
}

int32_t hostcall_value_counter(uint64_t value) {
    return (int32_t)(uint32_t)(value & 0x7FFFFFFFULL);
}
