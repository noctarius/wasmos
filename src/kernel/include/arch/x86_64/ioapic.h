/* ioapic.h - I/O APIC driver: MADT discovery, redirection-table programming,
 * and per-line mask/trigger control.
 *
 * Every irq_line below is an ISA IRQ number in [0, 16), translated internally
 * through the MADT interrupt-source overrides to the matching GSI. Lines at or
 * above 16, and any call made before ioapic_init() found an I/O APIC, are
 * silently ignored. */
#pragma once
#include "boot.h"
#include <stdint.h>

/* Locate the I/O APIC and the interrupt-source overrides in the ACPI MADT reached through
 * boot_info's RSDP, map its MMIO window, and program every redirection entry to its
 * IRQ_VECTOR_BASE-relative vector, masked.  Without a usable MADT no I/O APIC is recorded
 * and every other entry point below becomes a no-op. */
void ioapic_init(const boot_info_t* boot_info);

/* Set / clear the mask bit of the redirection entry for an ISA line, leaving its vector,
 * destination, trigger mode and polarity untouched. */
void ioapic_mask_irq(uint32_t irq_line);
void ioapic_unmask_irq(uint32_t irq_line);
/* Set trigger mode and polarity, preserving the line's vector, destination and
 * mask. PCI INTx lines are level-triggered active-low and must be switched
 * from the boot default (level-triggered, active-high) before use. */
void ioapic_configure_irq(uint32_t irq_line, int level, int active_low);
