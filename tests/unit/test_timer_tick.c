/* Host unit test for timer.c's tick ownership on SMP: every CPU's tick runs its
 * own quantum accounting, but only the boot CPU's advances the global tick
 * counter and reports a global tick to the IRQ path.
 *
 * Regression: 2026-11-25-irq-sharing-tick-on-every-cpu -- both architectures'
 * IRQ paths ran irq_sharing_tick on every CPU's timer tick, so with N CPUs a
 * throttled shared interrupt line was reopened and its dispatch budget reset N
 * times per global tick, defeating the storm limit. The defect is at the arch
 * call sites, which no host suite links; this pins the contract they now rely
 * on (timer_handle_irq returns 1 only for the global tick). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu_local.h"
#include "timer.h"

cpu_local_t g_cpus[WASMOS_MAX_CPUS];
uint32_t g_cpu_count = WASMOS_MAX_CPUS;
_Thread_local cpu_local_t* g_host_cpu_local;

static uint32_t g_process_ticks;

void process_tick(void) {
    g_process_ticks++;
}
void arch_timer_start(uint32_t hz) {
    (void)hz;
}
void trace_write(const char* s) {
    (void)s;
}

static int g_failures;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("  [FAIL] %s (%s:%d)\n", (msg), __FILE__, __LINE__);                            \
        }                                                                                          \
    } while (0)

static void be_cpu(uint32_t id) {
    g_cpus[id].cpu_id = id;
    g_host_cpu_local = &g_cpus[id];
}

int main(void) {
    memset(g_cpus, 0, sizeof(g_cpus));
    be_cpu(0);
    timer_init(250);
    uint64_t start = timer_ticks();

    be_cpu(1);
    CHECK(timer_handle_irq() == 0, "a secondary CPU's tick is not a global tick");
    CHECK(timer_ticks() == start, "and does not advance the counter");
    CHECK(g_process_ticks == 1, "but runs that CPU's quantum accounting");

    be_cpu(3);
    CHECK(timer_handle_irq() == 0, "nor does any other secondary's");

    be_cpu(0);
    CHECK(timer_handle_irq() == 1, "the boot CPU's tick is the global tick");
    CHECK(timer_ticks() == start + 1, "and advances the counter by one");
    CHECK(g_process_ticks == 3, "every tick ran quantum accounting");

    printf("test_timer_tick: %s\n", g_failures ? "FAILED" : "ok");
    return g_failures ? 1 : 0;
}
