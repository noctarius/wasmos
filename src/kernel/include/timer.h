#ifndef WASMOS_TIMER_H
#define WASMOS_TIMER_H

#include <stdint.h>

void timer_init(uint32_t hz);
void timer_handle_irq(void);
void timer_poll(void);
uint64_t timer_ticks(void);

#endif
