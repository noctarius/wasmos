/* timer_aarch64.c - the scheduler tick from the EL1 virtual timer (ARM ARM
 * D11.2). Implements arch/arch_timer.h. The counter frequency comes from
 * CNTFRQ_EL0, so no calibration is needed. The tick is periodic by advancing
 * the compare value one interval at a time, which keeps the period from
 * drifting by however late each interrupt was taken. */
#include "arch/arch_timer.h"

#include "gic_aarch64.h"
#include "klog.h"

/* PPI of the EL1 virtual timer (ARM BSA; QEMU virt's ACPI GTDT agrees). */
#define TIMER_VIRT_PPI 27u
#define CNTV_CTL_ENABLE (1u << 0)

/* Counter ticks per scheduler tick, the same on every CPU; written once by
 * arch_timer_start on the boot CPU before any secondary starts. */
static uint64_t g_timer_interval;

/* Start the calling CPU's virtual timer one interval from now and enable its
 * PPI on this CPU's redistributor. The timer and the PPI are per CPU. */
static void timer_start_this_cpu(void) {
    uint64_t now;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
    __asm__ volatile("msr cntv_cval_el0, %0\n"
                     "msr cntv_ctl_el0, %1\n"
                     "isb" ::"r"(now + g_timer_interval),
                     "r"((uint64_t)CNTV_CTL_ENABLE)
                     : "memory");
    gic_enable_ppi(TIMER_VIRT_PPI);
}

void arch_timer_start(uint32_t hz) {
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    g_timer_interval = freq / hz;
    timer_start_this_cpu();
    klog_printf("[timer] generic timer freq=%llu hz=%u interval=%llu\n",
                (unsigned long long)freq,
                (unsigned int)hz,
                (unsigned long long)g_timer_interval);
}

/* A secondary CPU's tick, at the rate arch_timer_start chose. */
void aarch64_timer_start_secondary(void) {
    timer_start_this_cpu();
}

/* Arm the next tick, one interval after the previous deadline; a deadline
 * already in the past (ticks were missed) restarts the period from now. */
void aarch64_timer_rearm(void) {
    uint64_t cval;
    uint64_t now;
    __asm__ volatile("mrs %0, cntv_cval_el0" : "=r"(cval));
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
    cval += g_timer_interval;
    if (cval <= now) {
        cval = now + g_timer_interval;
    }
    __asm__ volatile("msr cntv_cval_el0, %0\n"
                     "isb" ::"r"(cval)
                     : "memory");
}

/* Whether intid is the scheduler tick. */
int aarch64_timer_is_tick(uint32_t intid) {
    return intid == TIMER_VIRT_PPI;
}
