/* irq_aarch64.c - interrupt-controller bring-up and IRQ dispatch for aarch64.
 * Implements irq_init / irq_late_init of irq.h and the IRQ half of the
 * exception path.
 *
 * MSIs arrive as LPIs through the ITS (msi_aarch64.c).
 * TODO: line interrupt routing (irq_register / irq_ack / irq_configure) waits
 * for the IRQ grant ABI to become a list of interrupt identifiers instead of
 * 16 ISA lines (arm64 port, step 10 and Phase 3). */
#include "acpi_aarch64.h"
#include "arch/arch_context.h"
#include "gic_aarch64.h"
#include "irq.h"
#include "klog.h"
#include "msi.h"
#include "process.h"
#include "timer.h"

void aarch64_timer_rearm(void);
int aarch64_timer_is_tick(uint32_t intid);
void aarch64_msi_late_init(void);
void aarch64_msi_dispatch(uint32_t intid);

void irq_init(void) {
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

/* No interrupt line route can be held yet (irq_register is unimplemented),
 * so a dying context has none to release. */
void irq_release_context(uint32_t context_id) {
    (void)context_id;
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
        gic_eoi(intid);
        process_preempt_from_irq(frame);
        return;
    }
    klog_printf("[irq] unexpected intid=%u\n", (unsigned int)intid);
    gic_eoi(intid);
}
