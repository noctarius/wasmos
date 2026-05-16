#include "irq.h"
#include "arch/x86_64/irq_x86_64.h"

void irq_init(void) { x86_irq_init(); }
int irq_register(uint32_t context_id, uint32_t irq_line, uint32_t endpoint) {
    return x86_irq_register(context_id, irq_line, endpoint);
}
int irq_unregister(uint32_t context_id, uint32_t irq_line) {
    return x86_irq_unregister(context_id, irq_line);
}
int irq_mask(uint32_t irq_line) { return x86_irq_mask(irq_line); }
int irq_unmask(uint32_t irq_line) { return x86_irq_unmask(irq_line); }
