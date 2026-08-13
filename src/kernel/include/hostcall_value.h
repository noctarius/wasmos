#ifndef WASMOS_KERNEL_HOSTCALL_VALUE_H
#define WASMOS_KERNEL_HOSTCALL_VALUE_H

#include <stdint.h>

#include "wasmos_status.h"

/*
 * hostcall_value.h — the rule for host calls that return a value.
 *
 * The ABI gives a host call one signed i32 on which to carry both its result
 * and its errors, and the error codes are negative (abi/errors.yaml). A success
 * value with bit 31 set is therefore read by the guest as an error, silently.
 * Most of the `returns: value` calls in abi/hostcalls.yaml carry ids, counts,
 * pids, or offsets into a 2 GiB linear-memory window and cannot reach that bit.
 * The ones that can are physical and bus addresses, and monotonic counters --
 * they go through one of the two functions below rather than each open-coding
 * its own `> 0x7FFFFFFF` comparison.
 *
 * A value that genuinely spans the full 32-bit range fits neither function and
 * needs an out-parameter instead; that is why the io_region_in* and io_in*
 * families all have one. Width alone is not the test, though: in8 and in16
 * could never have collided with an error code, yet every caller masked the
 * sign off regardless. Ask whether the caller can act on the distinction, not
 * only whether the bits allow one.
 */

/*
 * For a value that should always be small: an address from a bounded pool, an
 * id, a size. Returns WASMOS_OK when value fits in 0..0x7FFFFFFF, else
 * WASMOS_ERR_KERNEL_TOO_LARGE. Exceeding the bound means something upstream is
 * wrong, and saying so beats handing back a number the guest reads as an error
 * anyway.
 */
wasmos_error_code_t hostcall_value_check(uint64_t value);

/*
 * For a monotonic counter, which has no small bound to enforce and is consumed
 * as deltas. Keeps the value positive and wrapping at 2^31 so that differences
 * stay correct across the wrap; refusing at that point would break every caller
 * rather than fix anything.
 */
int32_t hostcall_value_counter(uint64_t value);

#endif /* WASMOS_KERNEL_HOSTCALL_VALUE_H */
