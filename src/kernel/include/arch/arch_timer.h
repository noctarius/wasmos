/* arch_timer.h - the scheduler tick source. Implemented per architecture
 * (x86_64: arch/x86_64/timer_x86_64.c); timer.c owns the tick bookkeeping. */
#pragma once

#include <stdint.h>

/* Program the boot CPU's periodic tick at `hz` interrupts per second (hz is
 * non-zero) and unmask it, so every tick reaches timer_handle_irq(). Called
 * once from timer_init(), after paging can map device registers. The achieved
 * rate may differ from the requested one where the hardware cannot divide to
 * it exactly; timer.c converts with the requested rate. Secondary CPUs start
 * their own tick at the same rate during SMP bring-up. */
void arch_timer_start(uint32_t hz);
