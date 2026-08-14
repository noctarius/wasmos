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

/* Map and enable the LAPIC on the bootstrap processor and program its periodic timer for
 * `hz` ticks per second, aimed at IRQ_VECTOR_BASE.  Call once, after paging is active;
 * APs use lapic_ap_enable instead. */
void lapic_init(uint32_t hz);

/* Signal end-of-interrupt to the calling CPU's LAPIC.  Required for anything the LAPIC
 * itself delivered; an ExtINT interrupt passed through from the 8259 sets no LAPIC ISR
 * bit and must NOT be EOI'd here (Intel SDM, "Signaling Interrupt Servicing Completion"). */
void lapic_eoi(void);

#if WASMOS_SMP
/* Read the physical LAPIC ID of the calling CPU (bits [27:24] of the ID reg). */
uint32_t lapic_read_id(void);
/* Send INIT IPI to the given physical APIC ID and wait for delivery.  Spins until the ICR
 * send-pending bit clears, then stalls ~10 ms as the INIT-SIPI-SIPI sequence requires. */
void lapic_send_init_ipi(uint32_t apic_id);
/* Send a Startup IPI (SIPI) with the given startup vector (page number).  The target
 * begins executing in real mode at vector * 4 KiB, so the trampoline must be copied below
 * 1 MiB first.  Spins until delivery completes, then stalls ~200 us. */
void lapic_send_sipi(uint32_t apic_id, uint8_t vector);
/* Send an NMI to all CPUs except the caller (used by kpanic to stop the world). */
void lapic_send_nmi_allbutself(void);
/* Enable and configure the LAPIC timer on an AP (LAPIC already mapped by BSP).  Called by
 * each AP on itself once it reaches long mode; `hz` should match the BSP's rate. */
void lapic_ap_enable(uint32_t hz);
#endif
