#include "timer.h"
#include "klog.h"
#include "irq.h"
#include "io.h"
#include "serial.h"
#include "process.h"
#if WASMOS_IRQ_MODE >= 1
#include "arch/x86_64/lapic.h"
#endif

/*
 * PIT channel 0 is the current scheduler clock. The timer path only accounts
 * ticks and marks deferred log milestones; the heavy scheduling decision stays
 * outside the raw IRQ context.
 */

#define PIT_CMD_PORT 0x43
#define PIT_CH0_PORT 0x40
#define PIT_CMD_SQUARE_WAVE 0x36
#define PIT_BASE_HZ 1193182u

static volatile uint64_t g_timer_ticks;
static volatile uint8_t g_timer_log_pending;
static uint64_t g_timer_log_threshold;
static uint32_t g_timer_hz = 250;

void timer_init(uint32_t hz) {
    if (hz == 0) {
        hz = 250;
    }
    g_timer_hz = hz;
    g_timer_ticks = 0;
    g_timer_log_pending = 0;
    g_timer_log_threshold = 100;

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
    /* LAPIC/IOAPIC mode: map the LAPIC MMIO, disable the 8259 PIC, and
     * configure the LAPIC periodic timer.  This must happen after mm_init()
     * so that paging_map_4k() can allocate the intermediate page table. */
    lapic_init(hz);
    klog_write("[timer] lapic timer active\n");
#endif
}

void timer_handle_irq(void) {
    g_timer_ticks++;
    /* process_tick() owns quantum accounting and reschedule triggering. */
    process_tick();
    if (g_timer_ticks == g_timer_log_threshold) {
        g_timer_log_pending = 1;
        g_timer_log_threshold += 100;
    }
}

void timer_poll(void) {
    if (!g_timer_log_pending) {
        return;
    }
    g_timer_log_pending = 0;
    /* Periodic tick markers are useful while debugging scheduler liveness, but
     * they are intentionally hidden from normal boots behind WASMOS_TRACE. */
    trace_write("[timer] ticks\n");
}

uint64_t timer_ticks(void) {
    return g_timer_ticks;
}

uint64_t timer_ms_to_ticks(uint32_t ms) {
    return ((uint64_t)ms * g_timer_hz + 999u) / 1000u;
}
