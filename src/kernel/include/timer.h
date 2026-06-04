/* timer.h - Kernel timer abstraction backed by PIT or LAPIC.
 * Drives the scheduler tick and provides a monotonic tick counter. */
#ifndef WASMOS_TIMER_H
#define WASMOS_TIMER_H

#include <stdint.h>

/* Program the timer to fire at hz interrupts per second. */
void timer_init(uint32_t hz);

/* Called from the timer IRQ handler to advance the tick counter and trigger
 * scheduler preemption. */
void timer_handle_irq(void);

/* Poll-mode tick advance used when interrupts are disabled (early boot). */
void timer_poll(void);

/* Return the monotonically increasing tick count since timer_init(). */
uint64_t timer_ticks(void);

/* Convert a millisecond duration to a tick count at the current hz. */
uint64_t timer_ms_to_ticks(uint32_t ms);

#endif
