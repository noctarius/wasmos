/* timer.h - Kernel timer abstraction backed by the PIT (WASMOS_IRQ_MODE 0) or
 * the LAPIC timer (modes 1 and 2).  Drives the scheduler tick and provides a
 * monotonic tick counter. */
#ifndef WASMOS_TIMER_H
#define WASMOS_TIMER_H

#include <stdint.h>

/* Program the timer to fire at hz interrupts per second and reset the tick
 * counter to 0. hz == 0 falls back to 250. */
void timer_init(uint32_t hz);

/* Called from the timer IRQ handler to advance the tick counter and trigger
 * scheduler preemption. */
void timer_handle_irq(void);

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
