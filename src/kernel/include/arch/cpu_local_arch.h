/* cpu_local_arch.h - selects the target architecture's per-CPU block.
 * Each architecture provides `arch_cpu_t` (embedded in cpu_local_t as `arch`)
 * and `arch_cpu_local_self()` (the calling CPU's cpu_local_t on an SMP build).
 * This header is the single selection point; code outside arch/ includes
 * cpu_local.h, never an architecture directory. x86_64 is the only target. */
#pragma once

#include "arch/x86_64/cpu_local_arch.h"
