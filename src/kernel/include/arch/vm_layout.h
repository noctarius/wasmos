/* vm_layout.h - selects the target architecture's kernel virtual-address
 * layout. Each architecture defines KERNEL_HIGHER_HALF_BASE,
 * KERNEL_HIGHER_HALF_WINDOW_BYTES, KERNEL_BOOTSTRAP_WINDOW_BYTES, the kernel
 * MMIO window (KERNEL_MMIO_*), and the WARP linear-memory window
 * (WARP_LINMEM_*); their meaning is documented at the x86_64 definitions.
 * This header is the single selection point; x86_64 is the only target. */
#pragma once

#include "arch/x86_64/vm_layout.h"
