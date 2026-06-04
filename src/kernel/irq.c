/* irq.c - Architecture-independent IRQ init/registration shim.
 * All calls are forwarded to the x86_64 backend in arch/x86_64/irq_x86_64.c.
 * irq_register() ties a hardware IRQ line to an IPC endpoint so the ISR
 * delivers a notification message to the registered driver context. */
#include "irq.h"
#include "arch/x86_64/irq_x86_64.h"

void irq_init(void) { x86_irq_init(); }
void irq_late_init(const boot_info_t *boot_info) { x86_irq_late_init(boot_info); }
int irq_register(uint32_t context_id, uint32_t irq_line, uint32_t endpoint) {
    return x86_irq_register(context_id, irq_line, endpoint);
}
int irq_ack(uint32_t context_id, uint32_t irq_line) {
    return x86_irq_ack(context_id, irq_line);
}
int irq_unregister(uint32_t context_id, uint32_t irq_line) {
    return x86_irq_unregister(context_id, irq_line);
}
int irq_mask(uint32_t irq_line) { return x86_irq_mask(irq_line); }
int irq_unmask(uint32_t irq_line) { return x86_irq_unmask(irq_line); }
