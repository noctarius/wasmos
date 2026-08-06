/* msi.h - Message-signalled interrupt vector allocation.
 *
 * A driver allocates a vector, receives back the address/data pair that makes a
 * device raise it, and hands that pair to the bus driver owning config space
 * (pci-bus). The kernel owns the vector namespace and the delivery path; it never
 * touches a device. Events arrive as IPC_MSI_EVENT_TYPE and need no ack — see
 * msi_vectors.h for why the INTx mask/ack dance is absent here. */
#ifndef WASMOS_MSI_H
#define WASMOS_MSI_H

#include <stdint.h>
#include "msi_vectors.h"

/* First IDT vector used for MSI delivery: directly above the 16 IRQ lines
 * (IRQ_VECTOR_BASE 32 + IRQ_COUNT 16). */
#define MSI_VECTOR_BASE 48

/* IPC message type for an MSI event. Distinct from IPC_IRQ_EVENT_TYPE so a
 * driver that has switched to MSI can tell a stray INTx from a real message, and
 * so arg0 can carry the vector INDEX (which device source fired) rather than a
 * line number.
 * TODO: both event types are hand-mirrored into driver sources; move the kernel
 * notify-type space into abi/opcodes.yaml so they are generated like everything
 * else. */
#define IPC_MSI_EVENT_TYPE 0xFF01u

/* Reset the vector table. Called from irq_init. */
void msi_init(void);

/* Allocate a vector bound to `endpoint` (must be owned by context_id) and fill
 * the controller address/data pair a device writes to raise it. Returns 0, or a
 * negative WASMOS_ERR_MSI_* code. */
int msi_alloc(uint32_t context_id, uint32_t endpoint, uint32_t* out_address_lo,
              uint32_t* out_address_hi, uint32_t* out_data, uint32_t* out_vector);

/* Release a vector previously allocated by context_id. */
int msi_free(uint32_t context_id, uint32_t vector);

/* Drop every vector held by a dying context (called from process teardown). */
void msi_release_context(uint32_t context_id);

/* Called from the cpu_isr.S MSI stubs. */
void x86_msi_handler(uint64_t vector);

#endif
