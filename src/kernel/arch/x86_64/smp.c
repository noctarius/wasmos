#include "arch/x86_64/smp.h"
#include "serial.h"

#include <stdint.h>

/* Per-CPU data array.  g_cpus[0] is the BSP.  g_cpus[1..g_cpu_count-1] are
 * APs populated by MADT discovery (WASMOS_SMP builds only). */
cpu_local_t g_cpus[WASMOS_MAX_CPUS];

/* Number of CPUs found in the MADT.  Always at least 1 (BSP). */
uint32_t g_cpu_count = 1;

#if WASMOS_SMP

void
smp_init(void)
{
    /* TODO(smp): AP discovery (MADT type-0 scan) and BSP APIC ID recording.
     * For now this is a stub; g_cpu_count remains 1 until MADT parsing is
     * wired in. */
    serial_write("[smp] init (stub)\n");
}

void
smp_cpus_up(void)
{
    /* TODO(smp): INIT-SIPI-SIPI sequence for each AP in g_cpus[1..g_cpu_count-1]. */
    if (g_cpu_count <= 1) {
        serial_write("[smp] no APs to bring up\n");
        return;
    }
    serial_write("[smp] cpus_up (stub)\n");
}

#endif /* WASMOS_SMP */
