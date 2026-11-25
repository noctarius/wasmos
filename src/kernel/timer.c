/* timer.c - scheduler clock bookkeeping: the monotonic tick counter, the
 * requested rate and millisecond conversion. The tick hardware is programmed by
 * the architecture (arch_timer_start), whose interrupt handler calls
 * timer_handle_irq() once per tick. */
#include "timer.h"
#include "arch/arch_timer.h"
#include "klog.h"
#include "process.h"
#include "serial.h" /* trace_write */
#if WASMOS_SMP
#include "cpu_local.h"
#endif

/*
 * The interrupt path only accounts ticks and marks deferred log milestones; the
 * heavy scheduling decision stays outside the raw IRQ context.
 */

static volatile uint64_t g_timer_ticks;
static volatile uint8_t g_timer_log_pending;
static uint64_t g_timer_log_threshold;
static uint32_t g_timer_hz = 250;

/* hz is the requested tick rate; 0 is replaced by the 250 Hz default rather than
 * refused, because a zero rate would make timer_ms_to_ticks() answer 0 for every
 * duration and turn every timed wait into a busy retry.  g_timer_hz keeps the
 * REQUESTED value even where the hardware cannot achieve it exactly (see
 * arch_timer_start), so tick conversions drift by that error. */
void timer_init(uint32_t hz) {
    if (hz == 0) {
        hz = 250;
    }
    g_timer_hz = hz;
    g_timer_ticks = 0;
    g_timer_log_pending = 0;
    g_timer_log_threshold = 100;

    arch_timer_start(hz);
}

int timer_handle_irq(void) {
    int global_tick = 1;
#if WASMOS_SMP
    /* On SMP each AP runs its own tick at the same frequency as the BSP.
     * Only the BSP (cpu_id == 0) advances g_timer_ticks so that the global
     * monotonic tick counter increases at the configured hz rate, not N*hz.
     * All CPUs still call process_tick() for their own scheduling quantum. */
    global_tick = cpu_local()->cpu_id == 0;
#endif
    if (global_tick) {
        g_timer_ticks++;
        if (g_timer_ticks == g_timer_log_threshold) {
            g_timer_log_pending = 1;
            g_timer_log_threshold += 100;
        }
    }

    /* process_tick() owns quantum accounting and reschedule triggering. */
    process_tick();
    return global_tick;
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

/* Rounds UP, so a non-zero millisecond duration never converts to a zero-tick
 * deadline that would expire on the very next sched_timeout_check().  The result
 * is still coarse: one tick is 1000/hz ms, so any ms below that rounds to a
 * single tick and the real wait can be up to one tick longer than asked. */
uint64_t timer_ms_to_ticks(uint32_t ms) {
    return ((uint64_t)ms * g_timer_hz + 999u) / 1000u;
}
