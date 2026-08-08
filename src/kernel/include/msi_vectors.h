/* msi_vectors.h - Message-signalled interrupt vector bookkeeping, independent of
 * the interrupt controller.
 *
 * An MSI is a device-initiated memory write carrying a vector number, not a wire.
 * That removes every problem the INTx path is built around (see irq_sharing.h):
 *
 *   - A vector has exactly ONE owner. Nothing is wire-OR'd, so the kernel always
 *     knows which device raised it and no sharer list is needed.
 *   - Delivery is edge-triggered by construction. There is no line to mask before
 *     dispatch and no ack to reopen it afterwards, so there is no ack deadline, no
 *     dispatch budget, and no way for one wedged driver to stall another device.
 *   - A device with several vectors reports WHICH source fired by raising a
 *     different vector, so a driver can tell its RX queue from its config change
 *     without reading a status register.
 *
 * What is left is a vector namespace: allocate one, remember the endpoint it
 * belongs to, deliver to it, free it. That is this module.
 *
 * Delivery is injected as an op and the table is passed in by the caller, so no
 * state or platform dependency is hidden here. Programming the *device* to emit
 * the message is deliberately absent: the kernel owns the vector namespace and
 * never touches PCI config space — pci-bus does that (PCI_IPC_MSI_BIND). See
 * docs/architecture/05-x86-cpu-architecture.md §Message-Signalled Interrupts. */
#ifndef WASMOS_MSI_VECTORS_H
#define WASMOS_MSI_VECTORS_H

#include <stdint.h>
#include "wasmos_status.h" /* packed WASMOS_ERR_MSI_* returns */

/* Vectors reserved for MSI delivery. Sized for the devices in tree (a handful of
 * virtqueue vectors each) rather than for per-CPU queue fan-out; growing it means
 * adding stubs to x86_msi_stub_table, nothing more. */
#define MSI_VECTOR_COUNT 16u

typedef struct {
    uint8_t in_use;
    uint32_t owner_context_id;
    uint32_t endpoint;
} msi_vector_t;

typedef struct {
    /* Deliver the MSI event to its owner; returns 0 when it was queued. */
    int (*deliver)(uint32_t endpoint, uint32_t index);
} msi_vector_ops_t;

/* `count` slots are reset to unallocated. */
void msi_vectors_init(msi_vector_t* vectors, uint32_t count);

/* Bind the lowest free slot to `endpoint` and write its index to *out_index.
 * Returns 0, or WASMOS_ERR_MSI_NO_VECTORS. */
int msi_vectors_alloc(msi_vector_t* vectors, uint32_t count, uint32_t context_id, uint32_t endpoint,
                      uint32_t* out_index);

/* Release slot `index`, which must be allocated to `context_id`. Returns 0,
 * WASMOS_ERR_MSI_BAD_VECTOR, or WASMOS_ERR_MSI_NOT_OWNER. */
int msi_vectors_free(msi_vector_t* vectors, uint32_t count, uint32_t index, uint32_t context_id);

/* Release every slot held by a dying context. */
void msi_vectors_release_context(msi_vector_t* vectors, uint32_t count, uint32_t context_id);

/* Deliver slot `index` to its owner. A vector that fires while unallocated is a
 * device still emitting messages after its driver released it; it is dropped, and
 * this returns 0 so the caller can still count it. Returns 1 when an event was
 * queued, 0 when there was nothing to deliver to or delivery failed. */
int msi_vectors_dispatch(msi_vector_t* vectors, uint32_t count, uint32_t index,
                         const msi_vector_ops_t* ops);

/* True when `index` is allocated. */
int msi_vectors_in_use(const msi_vector_t* vectors, uint32_t count, uint32_t index);

#endif
