/* timer_x86_64.c - x86_64 scheduler tick source: the i8253 PIT channel 0 in
 * WASMOS_IRQ_MODE 0, the LAPIC periodic timer in modes 1 and 2. The tick reaches
 * timer_handle_irq() through the IRQ 0 handler (PIT) or the LAPIC timer vector. */
#include "arch/arch_timer.h"
#include "klog.h"
#if WASMOS_IRQ_MODE == 0
#include "io.h"
#include "irq.h"
#else
#include "arch/x86_64/lapic.h"
#endif

#if WASMOS_IRQ_MODE == 0
#define PIT_CMD_PORT 0x43
#define PIT_CH0_PORT 0x40
#define PIT_CMD_SQUARE_WAVE 0x36
#define PIT_BASE_HZ 1193182u
#endif

/* In PIT mode the divisor is clamped to 1..0xFFFF, so the achieved rate
 * saturates at roughly 18.2 Hz (divisor 0xFFFF) and PIT_BASE_HZ (divisor 1);
 * timer.c keeps the REQUESTED rate, so tick conversions drift if the clamp
 * engages. In LAPIC modes this maps the LAPIC MMIO, disables the 8259 PIC and
 * starts the calibrated periodic timer, which needs paging_map_4k and therefore
 * runs after mm_init(). */
void arch_timer_start(uint32_t hz) {
#if WASMOS_IRQ_MODE == 0
    uint32_t divisor = PIT_BASE_HZ / hz;
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xFFFFu) {
        divisor = 0xFFFFu;
    }

    outb(PIT_CMD_PORT, PIT_CMD_SQUARE_WAVE);
    outb(PIT_CH0_PORT, (uint8_t)(divisor & 0xFFu));
    outb(PIT_CH0_PORT, (uint8_t)((divisor >> 8) & 0xFFu));

    irq_unmask(0);
    klog_write("[timer] pit init\n");
#else
    lapic_init(hz);
    klog_write("[timer] lapic timer active\n");
#endif
}
