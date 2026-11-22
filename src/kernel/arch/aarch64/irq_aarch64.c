/* irq_aarch64.c - interrupt-controller bring-up and IRQ dispatch for aarch64.
 * Implements irq_init / irq_late_init of irq.h and the IRQ half of the
 * exception path.
 *
 * MSIs arrive as LPIs through the ITS (msi_aarch64.c). Line interrupts are
 * GIC SPIs, and the irq_line of irq.h is the SPI's INTID (32..1019). The SPIs
 * a driver holds occupy slots of a small table, and the slot index is the line
 * the arch-neutral sharing layer (irq_sharing.h) keys its bookkeeping on; the
 * ops below translate it back to the INTID for the distributor and for the
 * IRQ event a sharer receives. */
#include "acpi_aarch64.h"
#include "arch/arch_context.h"
#include "gic_aarch64.h"
#include "ipc.h"
#include "irq.h"
#include "klog.h"
#include "msi.h"
#include "policy.h"
#include "process.h"
#include "sync/spinlock.h"
#include "timer.h"
#include "wasmos_status.h"

/* IPC message type sent to a sharer's endpoint when its SPI fires. */
#define IPC_IRQ_EVENT_TYPE 0xFF00u

/* SPIs routed at once. Slot 0 is never used: the sharing layer treats line 0
 * as the timer line and never masks it. */
#define IRQ_SPI_SLOTS 16u

/* g_spi_intid[slot] is the INTID a slot holds, 0 for a free slot. A slot is
 * claimed by the first route to its SPI and freed when the last sharer leaves.
 * Both tables are mutated from interrupt context (dispatch, tick) and from host
 * calls, under g_spi_lock. */
static uint32_t g_spi_intid[IRQ_SPI_SLOTS];
static irq_line_t g_spi_lines[IRQ_SPI_SLOTS];
static ksync_spinlock_t g_spi_lock;

void aarch64_timer_rearm(void);
int aarch64_timer_is_tick(uint32_t intid);
void aarch64_msi_late_init(void);
void aarch64_msi_dispatch(uint32_t intid);

void irq_init(void) {
    ksync_spinlock_init(&g_spi_lock);
    irq_sharing_init(g_spi_lines, IRQ_SPI_SLOTS);
    gic_init();
    msi_init();
}

/* Record the firmware tables for the platform code that discovers devices
 * through them (MCFG for PCI configuration space), and bring the ITS up now
 * that the frame allocator can provide its tables.
 * TODO: take the GIC's addresses from the MADT here (arm64 port, Phase 3). */
void irq_late_init(const boot_info_t* boot_info) {
    acpi_init(boot_info);
    aarch64_msi_late_init();
}

/* The slot holding `intid`, or 0 when no route holds it. */
static uint32_t spi_slot_find(uint32_t intid) {
    for (uint32_t slot = 1; slot < IRQ_SPI_SLOTS; ++slot) {
        if (g_spi_intid[slot] == intid) {
            return slot;
        }
    }
    return 0;
}

/* Free every slot whose last sharer has left. The sharing layer has already
 * masked the SPI when the last sharer went. */
static void spi_slots_reclaim(void) {
    for (uint32_t slot = 1; slot < IRQ_SPI_SLOTS; ++slot) {
        if (g_spi_intid[slot] != 0 && !irq_sharing_has_sharers(g_spi_lines, slot)) {
            g_spi_intid[slot] = 0;
        }
    }
}

static void irq_ops_mask(uint32_t slot) {
    gic_spi_disable(g_spi_intid[slot]);
}

static void irq_ops_unmask(uint32_t slot) {
    gic_spi_enable(g_spi_intid[slot]);
}

static int irq_ops_deliver(uint32_t endpoint, uint32_t slot) {
    uint32_t intid = g_spi_intid[slot];
    ipc_message_t irq_msg;
    irq_msg.type = IPC_IRQ_EVENT_TYPE;
    irq_msg.request_id = (int32_t)intid;
    irq_msg.source = IPC_ENDPOINT_NONE;
    irq_msg.destination = endpoint;
    irq_msg.arg0 = (int32_t)intid;
    irq_msg.arg1 = 0;
    irq_msg.arg2 = 0;
    irq_msg.arg3 = 0;
    return ipc_send_from(IPC_CONTEXT_KERNEL, endpoint, &irq_msg) == IPC_OK ? 0 : -1;
}

static void irq_ops_log_throttle(uint32_t slot) {
    klog_printf("[irq] dispatch budget exhausted (unclaimed assertion or runaway device),"
                " throttling intid=%u\n",
                (unsigned int)g_spi_intid[slot]);
}

static const irq_sharing_ops_t g_spi_ops = {
    irq_ops_mask,
    irq_ops_unmask,
    irq_ops_deliver,
    timer_ticks,
    irq_ops_log_throttle,
};

/* Route SPI `irq_line` to `endpoint`. The first route to an SPI claims a slot
 * and programs the distributor (level-sensitive, routed to the boot CPU) before
 * the sharing layer enables it. Returns 0 or a packed WASMOS_ERR_IRQ_* code:
 * BAD_LINE for an INTID that is not an implemented SPI, BAD_ENDPOINT,
 * NOT_AUTHORIZED, and LINE_FULL when the line has no free sharer or no slot is
 * free for a new SPI. */
int irq_register(uint32_t context_id, uint32_t irq_line, uint32_t endpoint) {
    if (!gic_spi_valid(irq_line)) {
        return WASMOS_ERR_IRQ_BAD_LINE;
    }
    if (endpoint == IPC_ENDPOINT_NONE) {
        return WASMOS_ERR_IRQ_BAD_ENDPOINT;
    }
    if (policy_authorize(context_id, POLICY_ACTION_IRQ_ROUTE, irq_line) != 0) {
        return WASMOS_ERR_IRQ_NOT_AUTHORIZED;
    }
    uint32_t owner_context_id = 0;
    if (ipc_endpoint_owner(endpoint, &owner_context_id) != IPC_OK ||
        owner_context_id != context_id) {
        return WASMOS_ERR_IRQ_BAD_ENDPOINT;
    }
    ksync_spinlock_lock(&g_spi_lock);
    uint32_t slot = spi_slot_find(irq_line);
    if (slot == 0) {
        slot = spi_slot_find(0);
        if (slot == 0) {
            ksync_spinlock_unlock(&g_spi_lock);
            return WASMOS_ERR_IRQ_LINE_FULL;
        }
        g_spi_intid[slot] = irq_line;
        gic_spi_configure(irq_line, 1);
    }
    int rc = irq_sharing_register(g_spi_lines, slot, context_id, endpoint, &g_spi_ops);
    spi_slots_reclaim();
    ksync_spinlock_unlock(&g_spi_lock);
    return rc;
}

/* Report that a driver has serviced its device for one delivery of SPI
 * irq_line; the SPI is re-enabled once every notified sharer has acked.
 * Returns 0, WASMOS_ERR_IRQ_BAD_LINE for an INTID no route holds, or
 * WASMOS_ERR_IRQ_NOT_A_SHARER. */
int irq_ack(uint32_t context_id, uint32_t irq_line) {
    ksync_spinlock_lock(&g_spi_lock);
    uint32_t slot = gic_spi_valid(irq_line) ? spi_slot_find(irq_line) : 0;
    int rc = slot == 0 ? WASMOS_ERR_IRQ_BAD_LINE
                       : irq_sharing_ack(g_spi_lines, slot, context_id, &g_spi_ops);
    ksync_spinlock_unlock(&g_spi_lock);
    return rc;
}

/* Drop context_id's route on SPI irq_line, or every route on it for
 * IPC_CONTEXT_KERNEL. The SPI is disabled and its slot freed once no sharer
 * is left. Returns 0, WASMOS_ERR_IRQ_BAD_LINE for an INTID no route holds, or
 * WASMOS_ERR_IRQ_NOT_A_SHARER. */
int irq_unregister(uint32_t context_id, uint32_t irq_line) {
    ksync_spinlock_lock(&g_spi_lock);
    uint32_t slot = gic_spi_valid(irq_line) ? spi_slot_find(irq_line) : 0;
    int rc = WASMOS_ERR_IRQ_BAD_LINE;
    if (slot != 0) {
        rc = (context_id == IPC_CONTEXT_KERNEL)
                 ? irq_sharing_unregister_all(g_spi_lines, slot, &g_spi_ops)
                 : irq_sharing_unregister(g_spi_lines, slot, context_id, &g_spi_ops);
        spi_slots_reclaim();
    }
    ksync_spinlock_unlock(&g_spi_lock);
    return rc;
}

/* Drop every SPI route a dying context holds and free the slots it leaves
 * empty. Idempotent. */
void irq_release_context(uint32_t context_id) {
    ksync_spinlock_lock(&g_spi_lock);
    irq_sharing_release_context(g_spi_lines, IRQ_SPI_SLOTS, context_id, &g_spi_ops);
    spi_slots_reclaim();
    ksync_spinlock_unlock(&g_spi_lock);
}

/* Set the trigger of SPI irq_line: flags bit 0 selects level-sensitive, clear
 * selects edge-triggered. Bit 1 (active-low) has no GIC counterpart, because
 * an SPI is active-high or rising-edge at the distributor and an inverted
 * wire is the platform's concern, so it is ignored. The SPI is disabled
 * across the change (GICv3 12.9.9 leaves a trigger change on an enabled SPI
 * UNPREDICTABLE) and re-enabled when it was enabled. Returns 0 or
 * WASMOS_ERR_IRQ_BAD_LINE. */
int irq_configure(uint32_t irq_line, uint32_t flags) {
    if (!gic_spi_valid(irq_line)) {
        return WASMOS_ERR_IRQ_BAD_LINE;
    }
    ksync_spinlock_lock(&g_spi_lock);
    int enabled = gic_spi_enabled(irq_line);
    gic_spi_disable(irq_line);
    gic_spi_configure(irq_line, (flags & 1u) != 0);
    if (enabled) {
        gic_spi_enable(irq_line);
    }
    ksync_spinlock_unlock(&g_spi_lock);
    return 0;
}

/* Called from aarch64_exception for an IRQ from EL1 or EL0. The tick rearms
 * the timer, runs the scheduler accounting, ends the interrupt and then lets
 * the scheduler preempt the interrupted thread through the frame, in the
 * order the x86_64 IRQ 0 path uses. */
void aarch64_irq(irq_frame_t* frame) {
    uint32_t intid = gic_ack();
    /* 1020-1023 are the special INTIDs (spurious, among others) and carry no
     * interrupt to end; LPIs start above them, at GIC_LPI_BASE. */
    if (intid >= 1020u && intid < GIC_LPI_BASE) {
        return;
    }
    if (intid >= GIC_LPI_BASE) {
        aarch64_msi_dispatch(intid);
        gic_eoi(intid);
        return;
    }
    if (aarch64_timer_is_tick(intid)) {
        aarch64_timer_rearm();
        timer_handle_irq();
        ksync_spinlock_lock(&g_spi_lock);
        irq_sharing_tick(g_spi_lines, IRQ_SPI_SLOTS, &g_spi_ops);
        ksync_spinlock_unlock(&g_spi_lock);
        gic_eoi(intid);
        process_preempt_from_irq(frame);
        return;
    }
    /* The sharing layer disables the SPI before the EOI deactivates it, so a
     * level-sensitive SPI stays quiet until its sharers have acked. An SPI no
     * route holds is disabled so it cannot fire again. */
    ksync_spinlock_lock(&g_spi_lock);
    uint32_t slot = spi_slot_find(intid);
    if (slot != 0) {
        irq_sharing_dispatch(g_spi_lines, slot, &g_spi_ops);
    }
    ksync_spinlock_unlock(&g_spi_lock);
    if (slot == 0) {
        klog_printf("[irq] unrouted intid=%u\n", (unsigned int)intid);
        if (gic_spi_valid(intid)) {
            gic_spi_disable(intid);
        }
    }
    gic_eoi(intid);
}
