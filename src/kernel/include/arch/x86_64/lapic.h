#pragma once

#include <stdint.h>

/*
 * Local APIC (xAPIC) interface. lapic_init() discovers the LAPIC base via the
 * IA32_APIC_BASE MSR, maps the MMIO region into kernel virtual space, enables
 * the LAPIC, and programs the LAPIC periodic timer as the scheduler clock
 * source at `hz`.
 *
 * What it does to the legacy 8259 depends on WASMOS_IRQ_MODE. In mode 1
 * (virtual wire) the PIC stays live and delivers device IRQs through LINT0 in
 * ExtINT mode; only in mode 2 (IOAPIC) is the PIC masked off entirely. The
 * timer LVT is aimed at IRQ_VECTOR_BASE either way, so a LAPIC tick reaches the
 * scheduler through the same path as a PIT IRQ 0.
 *
 * The translation unit is compiled in every IRQ mode; the entry points are only
 * called when WASMOS_IRQ_MODE >= 1.
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
/* Send an NMI to all CPUs except the caller (used by kpanic to stop the world). */
void lapic_send_nmi_allbutself(void);
/* Enable and configure the LAPIC timer on an AP (LAPIC already mapped by BSP). */
void lapic_ap_enable(uint32_t hz);
#endif
