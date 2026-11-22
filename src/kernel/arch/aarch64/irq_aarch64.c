/* irq_aarch64.c - interrupt-controller bring-up and IRQ dispatch for aarch64.
 * Implements irq_init / irq_late_init of irq.h and the IRQ half of the
 * exception path.
 *
 * TODO: device interrupt routing (irq_register / irq_ack / irq_configure,
 * MSI through the ITS or GICv2m) waits for the IRQ grant ABI to become a list
 * of interrupt identifiers instead of 16 ISA lines (arm64 port, step 10 and
 * Phase 3); until then the only interrupt taken is the scheduler tick. */
#include "arch/arch_context.h"
#include "gic_aarch64.h"
#include "irq.h"
#include "klog.h"
#include "msi.h"
#include "process.h"
#include "timer.h"

void aarch64_timer_rearm(void);
int aarch64_timer_is_tick(uint32_t intid);

void irq_init(void) {
    gic_init();
}

/* Nothing to discover yet: the GIC is at QEMU virt's fixed addresses. */
void irq_late_init(const boot_info_t* boot_info) {
    (void)boot_info;
}

/* No interrupt route or MSI vector can be held yet (irq_register and
 * msi_alloc are unimplemented), so a dying context has nothing to release. */
void irq_release_context(uint32_t context_id) {
    (void)context_id;
}

void msi_release_context(uint32_t context_id) {
    (void)context_id;
}

/* Called from aarch64_exception for an IRQ from EL1 or EL0. The tick rearms
 * the timer, runs the scheduler accounting, ends the interrupt and then lets
 * the scheduler preempt the interrupted thread through the frame, in the
 * order the x86_64 IRQ 0 path uses. */
void aarch64_irq(irq_frame_t* frame) {
    uint32_t intid = gic_ack();
    if (intid >= 1020u) {
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
