#ifndef WASMOS_IRQ_H
#define WASMOS_IRQ_H

#include <stdint.h>
#include "process.h"

#define IRQ_VECTOR_BASE 32
#define IRQ_COUNT 16

void irq_init(void);
int irq_register(uint32_t context_id, uint32_t irq_line, uint32_t endpoint);
int irq_unregister(uint32_t context_id, uint32_t irq_line);
int irq_mask(uint32_t irq_line);
int irq_unmask(uint32_t irq_line);
void x86_irq_handler(uint64_t vector);
void x86_timer_irq_handler(irq_frame_t *frame);
void x86_irq_iret_corrupt(const uint64_t *saved, const uint64_t *current);
void x86_irq_ist_corrupt(void);

#endif
