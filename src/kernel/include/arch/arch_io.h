/* arch_io.h - selects the target architecture's I/O-port access.
 * Each architecture provides, as static inline functions:
 *   arch_port_in8/16/32(port, &value)    one IN of exactly that width
 *   arch_port_out8/16/32(port, value)    one OUT of exactly that width
 * each returning 0, or non-zero when the architecture has no I/O port space
 * at all -- in which case nothing was accessed and the caller reports
 * WASMOS_ERR_IO_NO_PORT_SPACE. Port numbers are not validated here: the
 * callers check the caller's capability and the port's range first. This
 * header is the single selection point; x86_64 and aarch64 are the targets. */
#pragma once

#if defined(WASMOS_ARCH_AARCH64)
#include "arch/aarch64/arch_io.h"
#else
#include "arch/x86_64/arch_io.h"
#endif
