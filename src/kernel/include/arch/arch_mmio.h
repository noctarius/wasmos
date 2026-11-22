/* arch_mmio.h - selects the target architecture's device-register accessors.
 * Each architecture provides, as static inline functions on a kernel VA that
 * maps device memory (MEM_REGION_FLAG_DEVICE):
 *   arch_mmio_read8/16/32(addr)          one load of exactly that width
 *   arch_mmio_write8/16/32(addr, value)  one store of exactly that width
 * Each access is issued exactly once, at its width, in program order with
 * respect to every other memory access of the calling CPU, so a doorbell write
 * cannot overtake the descriptor stores that precede it. The contract of each
 * is documented at the x86_64 implementation. This header is the single
 * selection point; x86_64 and aarch64 are the targets. */
#pragma once

#if defined(WASMOS_ARCH_AARCH64)
#include "arch/aarch64/arch_mmio.h"
#else
#include "arch/x86_64/arch_mmio.h"
#endif
