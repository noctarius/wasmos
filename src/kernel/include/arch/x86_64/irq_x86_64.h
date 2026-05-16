#ifndef WASMOS_ARCH_X86_64_IRQ_X86_64_H
#define WASMOS_ARCH_X86_64_IRQ_X86_64_H

#include <stdint.h>

int x86_irq_mask(uint32_t irq_line);
int x86_irq_unmask(uint32_t irq_line);
void x86_irq_init(void);
int x86_irq_register(uint32_t context_id, uint32_t irq_line, uint32_t endpoint);
int x86_irq_unregister(uint32_t context_id, uint32_t irq_line);

#endif
