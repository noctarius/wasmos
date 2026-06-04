/* irq.h - Hardware IRQ routing and dispatch.
 *
 * Drivers register an IPC endpoint to receive a fixed-format IRQ notification
 * message (type IPC_IRQ_EVENT_TYPE) when their IRQ line fires.  The kernel does
 * the minimal safe work in interrupt context and delivers via IPC. */
#ifndef WASMOS_IRQ_H
#define WASMOS_IRQ_H

#include <stdint.h>
#include "boot.h"
#include "process.h"

#define IRQ_VECTOR_BASE 32  /* x86 hardware IRQs start at IDT vector 32 (after exceptions) */
#define IRQ_COUNT 16        /* legacy PIC has 16 lines (IRQ0–IRQ15) */

/* Early IRQ init: mask all PIC lines, set up dispatch table. */
void irq_init(void);

/* Late init (after paging): apply ACPI MADT overrides, optionally switch to IOAPIC. */
void irq_late_init(const boot_info_t *boot_info);

/* Route irq_line to endpoint owned by context_id.  IRQ fires an IPC message. */
int irq_register(uint32_t context_id, uint32_t irq_line, uint32_t endpoint);

/* Send EOI for irq_line (call after handling the IRQ message). */
int irq_ack(uint32_t context_id, uint32_t irq_line);

int irq_unregister(uint32_t context_id, uint32_t irq_line);
int irq_mask(uint32_t irq_line);
int irq_unmask(uint32_t irq_line);

/* Called from cpu_isr.S stubs for non-timer hardware IRQs. */
void x86_irq_handler(uint64_t vector);

/* Called from the timer ISR; advances the scheduler tick and may preempt. */
void x86_timer_irq_handler(irq_frame_t *frame);

/* Diagnostic: called when IRET frame registers differ from saved state. */
void x86_irq_iret_corrupt(const uint64_t *saved, const uint64_t *current);

/* Diagnostic: called on IST stack canary corruption. */
void x86_irq_ist_corrupt(void);

#endif
