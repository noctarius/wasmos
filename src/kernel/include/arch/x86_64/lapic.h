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
