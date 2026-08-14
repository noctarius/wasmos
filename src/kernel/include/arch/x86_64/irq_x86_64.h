/* irq_x86_64.h - x86_64 IRQ subsystem: 8259 PIC + I/O APIC init and registration. */
#ifndef WASMOS_ARCH_X86_64_IRQ_X86_64_H
#define WASMOS_ARCH_X86_64_IRQ_X86_64_H

#include <stdint.h>
#include "boot.h"

/* Every irq_line below is a legacy ISA IRQ number in [0, IRQ_COUNT); anything at or above
 * that bound is rejected.  Which hardware the call reaches depends on the build's
 * WASMOS_IRQ_MODE: modes 0 and 1 drive the 8259 pair, mode 2 the I/O APIC. */

/* Mask / unmask one line at the interrupt controller.  Return 0 on success and -1 for a
 * line past IRQ_COUNT.  In PIC modes, unmasking a slave line (8..15) also clears the
 * master's cascade bit, without which no slave IRQ reaches the CPU at all. */
int x86_irq_mask(uint32_t irq_line);
int x86_irq_unmask(uint32_t irq_line);

/* Set a line's trigger mode and polarity: bit 0 of `flags` selects level-triggered, bit 1
 * active-low.  Only meaningful in I/O APIC mode; in PIC modes the flags are ignored and
 * the call still succeeds.  Returns 0, or -1 for a line past IRQ_COUNT. */
int x86_irq_configure(uint32_t irq_line, uint32_t flags);

/* Bring up interrupt routing: initialise the line table and, in PIC modes, remap the 8259
 * pair onto vectors IRQ_VECTOR_BASE.. so legacy vectors no longer collide with CPU
 * exception vectors, preserving the firmware's existing masks. */
void x86_irq_init(void);

/* Second-stage init that needs ACPI: discovers the I/O APIC from the MADT in boot_info.
 * A no-op outside I/O APIC mode. */
void x86_irq_late_init(const boot_info_t* boot_info);

/* Route line `irq_line` to `endpoint`, which must be owned by context_id, so the line's
 * interrupts are delivered as IPC messages.  Several contexts may register the same line;
 * the sharing layer masks it until every registered handler has acknowledged.  Returns 0
 * on success or a packed WASMOS_ERR_IRQ_* code: BAD_LINE past IRQ_COUNT, BAD_ENDPOINT for
 * IPC_ENDPOINT_NONE or an endpoint the caller does not own, NOT_AUTHORIZED when policy
 * refuses the route, plus whatever the sharing layer reports. */
int x86_irq_register(uint32_t context_id, uint32_t irq_line, uint32_t endpoint);

/* Acknowledge a delivered interrupt on `irq_line` for context_id; the line is unmasked
 * once every registered handler has acked.  Returns 0 or a packed WASMOS_ERR_IRQ_* code.
 * Failing to ack leaves the line masked and the device silent. */
int x86_irq_ack(uint32_t context_id, uint32_t irq_line);

/* Drop context_id's registration on `irq_line`; called with IPC_CONTEXT_KERNEL it drops
 * every registration on that line.  Returns 0 or a packed WASMOS_ERR_IRQ_* code. */
int x86_irq_unregister(uint32_t context_id, uint32_t irq_line);

/* Drop every IRQ registration held by context_id across all lines, unmasking or masking
 * each affected line as its remaining handler set requires.  Used at process teardown; a
 * context with no registrations is a no-op. */
void x86_irq_release_context(uint32_t context_id);

#endif
