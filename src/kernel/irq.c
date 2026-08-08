/* irq.c - Architecture-independent IRQ init/registration shim.
 * All calls are forwarded to the x86_64 backend in arch/x86_64/irq_x86_64.c.
 * irq_register() ties a hardware IRQ line to an IPC endpoint so the ISR
 * delivers a notification message to the registered driver context. */
#include "irq.h"
#include "msi.h"
#include "arch/x86_64/irq_x86_64.h"

void irq_init(void) {
    x86_irq_init();
    /* The MSI vector table shares this init point; its IDT gates were installed
     * alongside the IRQ ones in x86_cpu_init. */
    msi_init();
}
void irq_late_init(const boot_info_t* boot_info) {
    x86_irq_late_init(boot_info);
}
int irq_register(uint32_t context_id, uint32_t irq_line, uint32_t endpoint) {
    return x86_irq_register(context_id, irq_line, endpoint);
}
int irq_ack(uint32_t context_id, uint32_t irq_line) {
    return x86_irq_ack(context_id, irq_line);
}
int irq_unregister(uint32_t context_id, uint32_t irq_line) {
    return x86_irq_unregister(context_id, irq_line);
}
void irq_release_context(uint32_t context_id) {
    x86_irq_release_context(context_id);
}
int irq_mask(uint32_t irq_line) {
    return x86_irq_mask(irq_line);
}
int irq_unmask(uint32_t irq_line) {
    return x86_irq_unmask(irq_line);
}
int irq_configure(uint32_t irq_line, uint32_t flags) {
    return x86_irq_configure(irq_line, flags);
}
