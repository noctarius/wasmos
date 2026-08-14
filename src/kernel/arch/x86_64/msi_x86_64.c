/* msi_x86_64.c - x86_64 side of message-signalled interrupts: the LAPIC
 * address/data encoding, the vector table's home, and dispatch from the ISR
 * stubs. The allocation rules live in the arch-neutral msi_vectors.c. */
#include "msi.h"
#include "ipc.h"
#include "serial.h"
#include "sync/spinlock.h"
#include "paging.h"
#if WASMOS_IRQ_MODE >= 1
#include "arch/x86_64/lapic.h"
#endif

/*
 * x86 MSI message format (Intel SDM Vol 3A §10.11).
 *
 *   address = 0xFEE00000 | (destination_id << 12) | (RH << 3) | (DM << 2)
 *   data    = (trigger_mode << 15) | (level << 14) | (delivery_mode << 8) | vector
 *
 * Physical destination mode is used with no redirection hint (RH = DM = 0),
 * fixed delivery and edge trigger — so `data` reduces to the bare vector number.
 *
 * Destination is LAPIC 0 (the BSP), matching the IOAPIC redirection entries in
 * ioapic.c. Steering interrupts at other CPUs is a scheduling decision the system
 * does not make yet; when it does, both this and ioapic_program_rtes() change
 * together.
 */
#define MSI_ADDRESS_BASE 0xFEE00000u
#define MSI_DEST_APIC_ID 0u

/* The table is mutated from interrupt context on any CPU (dispatch) and from
 * hostcalls on any other (alloc/free/release), so every entry is serialised by
 * this lock. spinlock_lock() disables interrupts, so it is safe from an ISR. */
static msi_vector_t g_msi_vectors[MSI_VECTOR_COUNT];
static ksync_spinlock_t g_msi_lock;

static inline msi_vector_t* msi_vectors_ptr(void) {
    uintptr_t p = (uintptr_t)&g_msi_vectors[0];
    if (serial_high_alias_enabled() && (uint64_t)p < KERNEL_HIGHER_HALF_BASE) {
        p = (uintptr_t)((uint64_t)p + KERNEL_HIGHER_HALF_BASE);
    }
    return (msi_vector_t*)(void*)p;
}

static int msi_ops_deliver(uint32_t endpoint, uint32_t index) {
    ipc_message_t msg;
    msg.type = IPC_MSI_EVENT_TYPE;
    msg.request_id = (int32_t)index;
    msg.source = IPC_ENDPOINT_NONE;
    msg.destination = endpoint;
    /* arg0 is the vector index the driver chose when it programmed the device,
     * i.e. which of its own interrupt sources fired. */
    msg.arg0 = (int32_t)index;
    msg.arg1 = 0;
    msg.arg2 = 0;
    msg.arg3 = 0;
    return ipc_send_from(IPC_CONTEXT_KERNEL, endpoint, &msg) == IPC_OK ? 0 : -1;
}

static const msi_vector_ops_t g_msi_ops = {msi_ops_deliver};

/* Reset the MSI vector table to "all free" and initialise its lock. Called once
 * during kernel bring-up, before any context can allocate; it does not touch the
 * IDT, whose MSI gates cpu_x86_64.c installs separately. */
void msi_init(void) {
    ksync_spinlock_init(&g_msi_lock);
    msi_vectors_init(msi_vectors_ptr(), MSI_VECTOR_COUNT);
}

/* Reserve one MSI vector for a driver context and return the message the driver
 * must program into its device's MSI/MSI-X capability.
 *
 * On success returns 0 and fills all four out-parameters: *out_address_lo /
 * *out_address_hi are the message address (high half is always 0 here) and
 * *out_data the message data, which reduces to the bare vector number under the
 * fixed/edge/physical encoding this file uses. *out_vector is the absolute IDT
 * vector, MSI_VECTOR_BASE + index; the arg0 a driver later receives in the event
 * IPC is the index, not this vector.
 *
 * Returns a packed abi/errors.yaml code on failure and writes nothing:
 * WASMOS_ERR_MSI_UNSUPPORTED in IRQ mode 0, which has no LAPIC to receive the
 * message write; WASMOS_ERR_MSI_BAD_ENDPOINT for a NULL out-pointer,
 * IPC_ENDPOINT_NONE, or an endpoint the calling context does not own; or
 * whatever msi_vectors_alloc() returns when the table is full.
 *
 * Every vector is delivered to LAPIC 0 (the BSP). Takes the MSI lock only around
 * the table update. */
int msi_alloc(uint32_t context_id, uint32_t endpoint, uint32_t* out_address_lo,
              uint32_t* out_address_hi, uint32_t* out_data, uint32_t* out_vector) {
#if WASMOS_IRQ_MODE == 0
    /* Pure 8259 mode has no LAPIC, so nothing can receive a message write. */
    (void)context_id;
    (void)endpoint;
    (void)out_address_lo;
    (void)out_address_hi;
    (void)out_data;
    (void)out_vector;
    return WASMOS_ERR_MSI_UNSUPPORTED;
#else
    if (!out_address_lo || !out_address_hi || !out_data || !out_vector) {
        return WASMOS_ERR_MSI_BAD_ENDPOINT;
    }
    if (endpoint == IPC_ENDPOINT_NONE) {
        return WASMOS_ERR_MSI_BAD_ENDPOINT;
    }
    uint32_t owner_context_id = 0;
    if (ipc_endpoint_owner(endpoint, &owner_context_id) != IPC_OK ||
        owner_context_id != context_id) {
        return WASMOS_ERR_MSI_BAD_ENDPOINT;
    }

    uint32_t index = 0;
    ksync_spinlock_lock(&g_msi_lock);
    int rc = msi_vectors_alloc(msi_vectors_ptr(), MSI_VECTOR_COUNT, context_id, endpoint, &index);
    ksync_spinlock_unlock(&g_msi_lock);
    if (rc != 0) {
        return rc;
    }

    *out_vector = MSI_VECTOR_BASE + index;
    *out_address_lo = MSI_ADDRESS_BASE | (MSI_DEST_APIC_ID << 12);
    *out_address_hi = 0u;
    *out_data = *out_vector;
    return 0;
#endif
}

/* Release a vector previously returned by msi_alloc(). vector is the absolute IDT
 * vector, not the index. Returns 0 on success, WASMOS_ERR_MSI_BAD_VECTOR when it
 * falls outside MSI_VECTOR_BASE..+MSI_VECTOR_COUNT, or whatever
 * msi_vectors_free() returns when the slot is not owned by context_id. Frees only
 * the kernel-side reservation: the device keeps writing the message until its
 * driver disables the capability, so a freed vector can still be raised. */
int msi_free(uint32_t context_id, uint32_t vector) {
    if (vector < MSI_VECTOR_BASE || vector >= (MSI_VECTOR_BASE + MSI_VECTOR_COUNT)) {
        return WASMOS_ERR_MSI_BAD_VECTOR;
    }
    ksync_spinlock_lock(&g_msi_lock);
    int rc =
        msi_vectors_free(msi_vectors_ptr(), MSI_VECTOR_COUNT, vector - MSI_VECTOR_BASE, context_id);
    ksync_spinlock_unlock(&g_msi_lock);
    return rc;
}

/* Teardown hook for a dying context: frees every MSI vector it held. Idempotent
 * and silent when it held none. Takes the MSI lock. */
void msi_release_context(uint32_t context_id) {
    ksync_spinlock_lock(&g_msi_lock);
    msi_vectors_release_context(msi_vectors_ptr(), MSI_VECTOR_COUNT, context_id);
    ksync_spinlock_unlock(&g_msi_lock);
}

/* Dispatch body for the isr_msi_* stubs, called with the raw vector number in
 * interrupt context with IF clear. A vector outside the MSI range returns without
 * an EOI. Otherwise it delivers one IPC to the owning endpoint and issues the
 * LAPIC EOI; a delivery failure (no owner, full queue) is swallowed, since there
 * is no line to leave masked and nothing to retry. Takes the MSI lock around the
 * dispatch, and the EOI is issued unlocked. */
void x86_msi_handler(uint64_t vector) {
    if (vector < MSI_VECTOR_BASE || vector >= (MSI_VECTOR_BASE + MSI_VECTOR_COUNT)) {
        return;
    }
    uint32_t index = (uint32_t)(vector - MSI_VECTOR_BASE);
    ksync_spinlock_lock(&g_msi_lock);
    (void)msi_vectors_dispatch(msi_vectors_ptr(), MSI_VECTOR_COUNT, index, &g_msi_ops);
    ksync_spinlock_unlock(&g_msi_lock);
    /* Edge-triggered and exclusively owned: no line to unmask, no ack to wait
     * for. Clearing the LAPIC ISR bit is the whole of the epilogue. */
#if WASMOS_IRQ_MODE >= 1
    lapic_eoi();
#endif
}
