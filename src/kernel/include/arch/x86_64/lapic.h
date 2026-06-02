#pragma once

#include <stdint.h>

/*
 * Local APIC interface. lapic_init() discovers the LAPIC base via the
 * IA32_APIC_BASE MSR, maps the MMIO region into kernel virtual space,
 * enables the LAPIC, disables the legacy 8259 PIC, and configures the
 * LAPIC periodic timer as the scheduler clock source.
 *
 * Only compiled when WASMOS_IRQ_MODE >= 1 (LAPIC or IOAPIC build).
 */

void lapic_init(uint32_t hz);
void lapic_eoi(void);

#if WASMOS_SMP
/* Read the physical LAPIC ID of the calling CPU (bits [27:24] of the ID reg). */
uint32_t lapic_read_id(void);
/* Send INIT IPI to the given physical APIC ID and wait for delivery. */
void lapic_send_init_ipi(uint32_t apic_id);
/* Send a Startup IPI (SIPI) with the given startup vector (page number). */
void lapic_send_sipi(uint32_t apic_id, uint8_t vector);
/* Enable and configure the LAPIC timer on an AP (LAPIC already mapped by BSP). */
void lapic_ap_enable(uint32_t hz);
#endif
