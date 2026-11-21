/* arch_cpu.h - selects the target architecture's per-CPU control primitives.
 * Each architecture provides, as static inline functions acting on the calling
 * CPU:
 *   arch_irq_flags_t, arch_irq_save(), arch_irq_restore()
 *   arch_irq_enable(), arch_irq_disable()
 *   arch_wait_for_interrupt()  unmask + wait, with no lost-wakeup window
 *   arch_halt_forever()        mask + stop, never returns
 *   arch_cpu_relax()           spin-wait hint
 * The contract of each is documented at the x86_64 implementation. This header
 * is the single selection point; x86_64 is the only target. */
#pragma once

#include "arch/x86_64/arch_cpu.h"
