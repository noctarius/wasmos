#ifndef WASMOS_KERNEL_HOSTCALL_VALUE_H
#define WASMOS_KERNEL_HOSTCALL_VALUE_H

#include <stdint.h>

#include "wasmos_status.h"

/*
 * hostcall_value.h — the rule for host calls that return a value.
 *
 * The ABI gives a host call one signed i32 on which to carry both its result
 * and its errors, and the error codes are negative. A success value with bit 31
 * set is therefore read by the guest as an error, silently. Of the 53
 * value-returning calls in abi/hostcalls.yaml most are ids, counts, pids or
 * offsets into a 2 GiB linear-memory window and cannot reach that bit; the ones
 * that can are physical and bus addresses, and monotonic counters.
 *
 * The rule was already being applied by hand -- dma_map_borrow open-coded
 * `device_addr > 0x7FFFFFFF`, block_buffer_phys did not and was wrong for it --
 * which is exactly the divergence that a shared block-bounds check was just
 * introduced to stop. One named rule, then, rather than a comparison rewritten
 * per call site.
 *
 * Where a value genuinely uses the full 32-bit range neither function helps and
 * the call needs an out-parameter instead, as the io_region_in* and io_in*
 * families now all have. The port-read family went further: in8 and in16 could
 * not have collided with a code, but no caller read the sign -- each masked it
 * off -- so the width of the value is not the whole test. Ask whether the
 * caller can act on the distinction, not only whether the bits allow one.
 * thread_join was the last one left and now has an out-parameter too, so no
 * host call still carries a guest-chosen full-range value on the shared i32.
 */

/*
 * For a value that should always be small: an address from a bounded pool, an
 * id, a size. Exceeding the bound means something upstream is wrong, and saying
 * so beats handing back a number the guest will read as an error anyway.
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
