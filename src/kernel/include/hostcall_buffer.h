#ifndef WASMOS_KERNEL_HOSTCALL_BUFFER_H
#define WASMOS_KERNEL_HOSTCALL_BUFFER_H

#include <stdint.h>

#include "wasmos_status.h"

/*
 * hostcall_buffer.h — copying a name into a caller-supplied buffer.
 *
 * The shape recurs across the host-call surface: the kernel holds a bounded,
 * possibly unterminated string, the guest supplies a buffer, and the call must
 * fill it without overrunning either side while still letting the caller tell
 * that truncation happened. Both runtimes wrote the arithmetic out by hand and
 * WARP's copy had two faults:
 *
 *   - It never validated the buffer size, then computed `nlen = out_len - 1`.
 *     With out_len == 0 that underflows to 0xFFFFFFFF, giving a 4 GiB memcpy
 *     and a write at out[0xFFFFFFFF].
 *
 *   - It returned the CLAMPED length rather than the true one, so a caller
 *     could not distinguish "the name is exactly this long" from "the name was
 *     cut to fit". fs_init.c depends on the difference: it skips an entry whose
 *     reported length does not fit its buffer, a test that can never fire if
 *     the reported length is the one that was made to fit.
 *
 * So the split is returned explicitly: `copy_len` bytes to move, `true_len` to
 * report. A caller that wants snprintf's semantics returns true_len.
 */
wasmos_error_code_t hostcall_name_clamp(const char* name, uint32_t name_max, uint32_t out_size,
                                        uint32_t* true_len, uint32_t* copy_len);

#endif /* WASMOS_KERNEL_HOSTCALL_BUFFER_H */
