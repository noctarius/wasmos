/* msi_aarch64.c - message-signalled interrupts through the GICv3 ITS.
 * Implements msi.h on the neutral vector table (msi_vectors.h).
 *
 * Slot `index` of the vector table is EventID `index` of the caller's PCI
 * function and LPI GIC_LPI_BASE + index. The device identity the ITS needs
 * comes from the kernel's record of which function the calling context drives
 * (pci_function_of_context, from its spawn profile), so the host call carries
 * no device argument. The vector number handed back keeps the msi.h numbering
 * (MSI_VECTOR_BASE + index): drivers see the index in an event's arg0 on every
 * architecture.
 *
 * g_msi_lock serialises the vector table and every ITS command. */
#include "msi.h"

#include "gic_aarch64.h"
#include "ipc.h"
#include "its_aarch64.h"
#include "pci_config.h"
#include "sync/spinlock.h"
#include "wasmos_status.h"

static msi_vector_t g_msi_vectors[MSI_VECTOR_COUNT];
/* DeviceID (requester id) each allocated slot was mapped for. */
static uint16_t g_msi_bdf[MSI_VECTOR_COUNT];
static ksync_spinlock_t g_msi_lock;
static uint8_t g_its_ready;

static int msi_ops_deliver(uint32_t endpoint, uint32_t index) {
    ipc_message_t msg;
    msg.type = IPC_MSI_EVENT_TYPE;
    msg.request_id = (int32_t)index;
    msg.source = IPC_ENDPOINT_NONE;
    msg.destination = endpoint;
    msg.arg0 = (int32_t)index;
    msg.arg1 = 0;
    msg.arg2 = 0;
    msg.arg3 = 0;
    return ipc_send_from(IPC_CONTEXT_KERNEL, endpoint, &msg) == IPC_OK ? 0 : -1;
}

static const msi_vector_ops_t g_msi_ops = {msi_ops_deliver};

void msi_init(void) {
    ksync_spinlock_init(&g_msi_lock);
    msi_vectors_init(g_msi_vectors, MSI_VECTOR_COUNT);
}

/* Bring the ITS up; called from irq_late_init, once the frame allocator can
 * provide its tables. Until it succeeds msi_alloc reports UNSUPPORTED. */
void aarch64_msi_late_init(void) {
    g_its_ready = its_init() == 0 ? 1u : 0u;
}

int msi_alloc(uint32_t context_id, uint32_t endpoint, uint32_t* out_address_lo,
              uint32_t* out_address_hi, uint32_t* out_data, uint32_t* out_vector) {
    if (!out_address_lo || !out_address_hi || !out_data || !out_vector ||
        endpoint == IPC_ENDPOINT_NONE) {
        return WASMOS_ERR_MSI_BAD_ENDPOINT;
    }
    uint32_t owner_context_id = 0;
    if (ipc_endpoint_owner(endpoint, &owner_context_id) != IPC_OK ||
        owner_context_id != context_id) {
        return WASMOS_ERR_MSI_BAD_ENDPOINT;
    }
    if (!g_its_ready) {
        return WASMOS_ERR_MSI_UNSUPPORTED;
    }
    uint16_t bdf = 0;
    if (pci_function_of_context(context_id, &bdf) != 0) {
        return WASMOS_ERR_MSI_NO_FUNCTION;
    }
    uint32_t index = 0;
    ksync_spinlock_lock(&g_msi_lock);
    int rc = msi_vectors_alloc(g_msi_vectors, MSI_VECTOR_COUNT, context_id, endpoint, &index);
    if (rc == 0 && its_map_event(bdf, index, GIC_LPI_BASE + index) != 0) {
        /* The ITS has no room for the device or its translation table. */
        (void)msi_vectors_free(g_msi_vectors, MSI_VECTOR_COUNT, index, context_id);
        rc = WASMOS_ERR_MSI_NO_VECTORS;
    }
    if (rc == 0) {
        g_msi_bdf[index] = bdf;
    }
    ksync_spinlock_unlock(&g_msi_lock);
    if (rc != 0) {
        return rc;
    }
    uint64_t doorbell = its_translater_phys();
    *out_address_lo = (uint32_t)doorbell;
    *out_address_hi = (uint32_t)(doorbell >> 32);
    *out_data = index;
    *out_vector = MSI_VECTOR_BASE + index;
    return 0;
}

int msi_free(uint32_t context_id, uint32_t vector) {
    if (vector < MSI_VECTOR_BASE || vector >= (MSI_VECTOR_BASE + MSI_VECTOR_COUNT)) {
        return WASMOS_ERR_MSI_BAD_VECTOR;
    }
    uint32_t index = vector - MSI_VECTOR_BASE;
    ksync_spinlock_lock(&g_msi_lock);
    if (msi_vectors_in_use(g_msi_vectors, MSI_VECTOR_COUNT, index) &&
        g_msi_vectors[index].owner_context_id == context_id) {
        its_unmap_event(g_msi_bdf[index], index, GIC_LPI_BASE + index);
    }
    int rc = msi_vectors_free(g_msi_vectors, MSI_VECTOR_COUNT, index, context_id);
    ksync_spinlock_unlock(&g_msi_lock);
    return rc;
}

void msi_release_context(uint32_t context_id) {
    ksync_spinlock_lock(&g_msi_lock);
    for (uint32_t index = 0; index < MSI_VECTOR_COUNT; ++index) {
        if (msi_vectors_in_use(g_msi_vectors, MSI_VECTOR_COUNT, index) &&
            g_msi_vectors[index].owner_context_id == context_id) {
            its_unmap_event(g_msi_bdf[index], index, GIC_LPI_BASE + index);
        }
    }
    msi_vectors_release_context(g_msi_vectors, MSI_VECTOR_COUNT, context_id);
    ksync_spinlock_unlock(&g_msi_lock);
}

/* Deliver LPI `intid` to the endpoint its slot is bound to. */
void aarch64_msi_dispatch(uint32_t intid) {
    uint32_t index = intid - GIC_LPI_BASE;
    ksync_spinlock_lock(&g_msi_lock);
    (void)msi_vectors_dispatch(g_msi_vectors, MSI_VECTOR_COUNT, index, &g_msi_ops);
    ksync_spinlock_unlock(&g_msi_lock);
}
