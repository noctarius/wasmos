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
 * negative WASMOS_ERR_MSI_* code: UNSUPPORTED in a build with no LAPIC to receive the
 * message write, BAD_ENDPOINT for a NULL out pointer, IPC_ENDPOINT_NONE, or an endpoint
 * the caller does not own, NO_VECTORS when the table is full.
 *
 * The encoding follows the Intel SDM's "Message Signalled Interrupts" section:
 * address_lo is 0xFEE00000 with the destination APIC id in bits [19:12], physical
 * destination mode with no redirection hint; address_hi is 0 (no extended destination);
 * data reduces to the bare IDT vector number because delivery is fixed and edge-
 * triggered.  Destination is always LAPIC 0, matching the I/O APIC redirection entries.
 * out_vector is the absolute IDT vector (MSI_VECTOR_BASE + slot index), which is what
 * msi_free expects back; the index the driver sees in an event's arg0 is that vector
 * minus MSI_VECTOR_BASE. */
int msi_alloc(uint32_t context_id, uint32_t endpoint, uint32_t* out_address_lo,
              uint32_t* out_address_hi, uint32_t* out_data, uint32_t* out_vector);

/* Release a vector previously allocated by context_id.  `vector` is the absolute IDT
 * vector msi_alloc reported.  Returns 0, WASMOS_ERR_MSI_BAD_VECTOR for a vector outside
 * the MSI range or an unallocated slot, or WASMOS_ERR_MSI_NOT_OWNER.  Does not stop the
 * device from emitting messages — that is the bus driver's job, and a message arriving
 * after the release is dropped. */
int msi_free(uint32_t context_id, uint32_t vector);

/* Drop every vector held by a dying context (called from process teardown). */
void msi_release_context(uint32_t context_id);

/* Called from the cpu_isr.S MSI stubs with the absolute IDT vector.  Dispatches the
 * event to the owning endpoint and issues the LAPIC EOI; a vector outside the MSI range
 * is ignored.  Runs in interrupt context and takes the vector-table spinlock. */
void x86_msi_handler(uint64_t vector);

#endif
