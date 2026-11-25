/* timer.h - Kernel timer abstraction: drives the scheduler tick and provides a
 * monotonic tick counter. The tick hardware is the architecture's
 * (arch/arch_timer.h; x86_64: the PIT in WASMOS_IRQ_MODE 0, the LAPIC timer in
 * modes 1 and 2). */
#ifndef WASMOS_TIMER_H
#define WASMOS_TIMER_H

#include <stdint.h>

/* Program the timer to fire at hz interrupts per second and reset the tick
 * counter to 0. hz == 0 falls back to 250. */
void timer_init(uint32_t hz);

/* Called from the timer IRQ handler on every CPU's tick: runs the calling CPU's
 * quantum accounting (process_tick) and, on the boot CPU only, advances the
 * global tick counter. Returns 1 when this call advanced the counter -- the boot
 * CPU's tick, or every tick in a non-SMP kernel -- and 0 for a secondary CPU's.
 * Per-tick work shared by the whole machine, such as the interrupt-sharing
 * layer's throttle and budget reset (irq_sharing_tick), runs only on a 1: on
 * every CPU's tick it would run N times per global tick. */
int timer_handle_irq(void);

/* Drain the pending periodic tick marker to the trace log. Does NOT advance the
 * tick counter -- only timer_handle_irq does -- and is a no-op unless the kernel
 * was built with WASMOS_TRACE. */
void timer_poll(void);

/* Return the monotonically increasing tick count since timer_init(). */
uint64_t timer_ticks(void);

/* Convert a millisecond duration to a tick count at the current hz, rounding UP
 * so a nonzero duration never becomes a zero-tick (immediately expired) wait. */
uint64_t timer_ms_to_ticks(uint32_t ms);

#endif
