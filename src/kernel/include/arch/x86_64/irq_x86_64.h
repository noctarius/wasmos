/* irq_x86_64.h - x86_64 IRQ subsystem: 8259 PIC + I/O APIC init and registration. */
#ifndef WASMOS_ARCH_X86_64_IRQ_X86_64_H
#define WASMOS_ARCH_X86_64_IRQ_X86_64_H

#include <stdint.h>
#include "boot.h"

/* The neutral IRQ API (irq.h) is implemented directly in irq_x86_64.c; this
 * header declares only the x86_64 interrupt-entry handlers, called from the
 * cpu_isr.S stubs. */

#include "process.h" /* irq_frame_t */

/* Non-timer hardware IRQ: dispatches the line's IPC notification and EOIs. */
void x86_irq_handler(uint64_t vector);

/* Timer IRQ; advances the scheduler tick and may preempt via the frame. */
void x86_timer_irq_handler(irq_frame_t* frame);

/* MSI vectors: dispatches the event to the owning endpoint and issues the LAPIC
 * EOI; a vector outside the MSI range is ignored. Runs in interrupt context and
 * takes the vector-table spinlock. Implemented in msi_x86_64.c. */
void x86_msi_handler(uint64_t vector);

/* Diagnostic: called when IRET frame registers differ from saved state. */
void x86_irq_iret_corrupt(const uint64_t* saved, const uint64_t* current);

/* Diagnostic: called on IST stack canary corruption. */
void x86_irq_ist_corrupt(void);

#endif
