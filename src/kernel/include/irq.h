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
#include "irq_sharing.h" /* sharer limits + per-line bookkeeping */

#define IRQ_VECTOR_BASE 32 /* x86 hardware IRQs start at IDT vector 32 (after exceptions) */
#define IRQ_COUNT 16       /* legacy PIC has 16 lines (IRQ0–IRQ15) */

/* The functions below are implemented by the architecture (x86_64:
 * arch/x86_64/irq_x86_64.c). Every irq_line is a line number in [0, IRQ_COUNT);
 * anything at or above that bound is rejected. */

/* Early init on the boot CPU with interrupts masked: initialise the line table
 * and the MSI vector table, and bring the interrupt controller to a state where
 * no line is live (x86_64 PIC modes: remap the 8259 pair onto IRQ_VECTOR_BASE,
 * preserving the firmware's masks). */
void irq_init(void);

/* Second-stage init that needs the firmware tables in boot_info (x86_64: the
 * I/O APIC from the ACPI MADT, with its overrides). A no-op where there is
 * nothing to discover. */
void irq_late_init(const boot_info_t* boot_info);

/* Route `irq_line` to `endpoint`, which must be owned by context_id, so the
 * line's interrupts are delivered as IPC messages. Several contexts may
 * register the same line; the sharing layer masks it until every registered
 * handler has acknowledged. Returns 0 on success or a packed WASMOS_ERR_IRQ_*
 * code: BAD_LINE past IRQ_COUNT, BAD_ENDPOINT for IPC_ENDPOINT_NONE or an
 * endpoint the caller does not own, NOT_AUTHORIZED when policy refuses the
 * route, plus whatever the sharing layer reports. */
int irq_register(uint32_t context_id, uint32_t irq_line, uint32_t endpoint);

/* Report that this context has looked at its device. The line reopens once
 * every sharer on it has acked; failing to ack leaves the line masked and the
 * device silent. Returns 0 or a packed WASMOS_ERR_IRQ_* code. */
int irq_ack(uint32_t context_id, uint32_t irq_line);

/* Drop context_id's route on irq_line; with IPC_CONTEXT_KERNEL, drops every route on that
 * line.  Returns 0 or a packed WASMOS_ERR_IRQ_* code.  The line is re-masked once it has
 * no sharers left. */
int irq_unregister(uint32_t context_id, uint32_t irq_line);

/* Drop every route held by a dying context (called from process teardown),
 * masking or reopening each affected line as its remaining sharers require. A
 * context with no routes is a no-op. */
void irq_release_context(uint32_t context_id);

/* Mask/unmask a line at the active interrupt controller (x86_64: the 8259 pair
 * under WASMOS_IRQ_MODE <= 1, where unmasking a slave line also opens the
 * master's cascade; the I/O APIC redirection entry under mode 2).  Returns 0,
 * or -1 if irq_line >= IRQ_COUNT.  These act on the controller only and do not
 * touch routing, so a masked line keeps its registered sharers; the shared-line
 * ack path (irq_ack) is what normally reopens a line, and calling irq_unmask
 * directly on a shared line bypasses the "every sharer has acked" rule. */
int irq_mask(uint32_t irq_line);
int irq_unmask(uint32_t irq_line);

/* Set trigger/polarity for a line (flags: bit0=level, bit1=active-low). Used by
 * pci-bus to mark PCI INTx lines level/active-low. Returns 0, or -1 for a line
 * past IRQ_COUNT; a controller without per-line trigger control (x86_64 PIC
 * modes) ignores the flags and succeeds. */
int irq_configure(uint32_t irq_line, uint32_t flags);

#endif
