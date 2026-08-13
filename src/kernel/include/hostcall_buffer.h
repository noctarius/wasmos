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
 * that truncation happened. Both runtimes share this one arithmetic rather than
 * each writing the bound out by hand.
 *
 * Both lengths are reported because they answer different questions.
 * `copy_len` is how many bytes to move: it is clamped to out_size - 1 so a
 * terminator always fits, and the caller writes that terminator itself.
 * `true_len` is the name's actual length, which the caller reports to the guest
 * -- snprintf semantics. Returning the clamped length in its place would make
 * "the name is exactly this long" indistinguishable from "the name was cut to
 * fit", and callers act on that difference: fs_init skips an entry whose
 * reported length does not fit its buffer, a test that cannot fire if the
 * reported length is the one that was made to fit.
 *
 * `name` is read for at most name_max bytes and need not be NUL-terminated.
 * Returns WASMOS_OK, or WASMOS_INVAL when name, true_len, or copy_len is NULL,
 * or when out_size is 0 -- a zero-sized buffer has no room even for the
 * terminator, and out_size must be validated before out_size - 1 is evaluated.
 * A NULL-pointer rejection leaves both outputs untouched; every other path
 * writes both, so the out_size == 0 rejection reports 0/0.
 */
wasmos_error_code_t hostcall_name_clamp(const char* name, uint32_t name_max, uint32_t out_size,
                                        uint32_t* true_len, uint32_t* copy_len);

#endif /* WASMOS_KERNEL_HOSTCALL_BUFFER_H */
