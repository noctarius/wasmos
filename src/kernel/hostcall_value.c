#include "hostcall_value.h"

/* Bounds a host-call scalar to what survives the int32 return channel: anything
 * above INT32_MAX would come back with the sign bit set and be read as a packed
 * error code.  Returns WASMOS_OK or WASMOS_ERR_KERNEL_TOO_LARGE. */
wasmos_error_code_t hostcall_value_check(uint64_t value) {
    if (value > 0x7FFFFFFFULL) {
        return WASMOS_ERR_KERNEL_TOO_LARGE;
    }
    return WASMOS_OK;
}

/* Narrows a counter to the int32 return channel by masking off the sign bit, so
 * the result is always in [0, INT32_MAX] and can never be mistaken for a packed
 * error code.  A value above INT32_MAX is silently wrapped, not clamped — call
 * hostcall_value_check first when the caller must distinguish the two. */
int32_t hostcall_value_counter(uint64_t value) {
    return (int32_t)(uint32_t)(value & 0x7FFFFFFFULL);
}
