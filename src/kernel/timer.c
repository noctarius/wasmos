#include "timer.h"
#include "irq.h"
#include "io.h"
#include "serial.h"

#define PIT_CMD_PORT 0x43
#define PIT_CH0_PORT 0x40
#define PIT_CMD_SQUARE_WAVE 0x36
#define PIT_BASE_HZ 1193182u

static volatile uint64_t g_timer_ticks;
static volatile uint8_t g_timer_log_pending;
static uint64_t g_timer_log_threshold;

void timer_init(uint32_t hz) {
    if (hz == 0) {
        hz = 250;
    }
    uint32_t divisor = PIT_BASE_HZ / hz;
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xFFFFu) {
        divisor = 0xFFFFu;
    }

    g_timer_ticks = 0;
    g_timer_log_pending = 0;
    g_timer_log_threshold = 100;

    outb(PIT_CMD_PORT, PIT_CMD_SQUARE_WAVE);
    outb(PIT_CH0_PORT, (uint8_t)(divisor & 0xFFu));
    outb(PIT_CH0_PORT, (uint8_t)((divisor >> 8) & 0xFFu));

    irq_unmask(0);
    serial_write("[timer] pit init\n");
}

void timer_handle_irq(void) {
    g_timer_ticks++;
    if (g_timer_ticks == g_timer_log_threshold) {
        g_timer_log_pending = 1;
    }
}

void timer_poll(void) {
    if (!g_timer_log_pending) {
        return;
    }
    g_timer_log_pending = 0;
    serial_write("[timer] ticks\n");
}

uint64_t timer_ticks(void) {
    return g_timer_ticks;
}
