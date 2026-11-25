/* smp_aarch64.c - secondary-CPU bring-up through PSCI CPU_ON (smp.h).
 *
 * smp_init reads the CPUs from the ACPI MADT (one enabled GICC entry each) into
 * g_cpus[]; the boot CPU keeps slot 0. smp_cpus_up starts each secondary in
 * turn with PSCI CPU_ON at entry.S's _start_secondary and waits for it to
 * publish `started`. A secondary turns its MMU on with the kernel's own tables,
 * brings up its redistributor, scheduler state, idle thread and timer
 * (smp_ap_c_entry), and joins the scheduler loop.
 *
 * As on x86_64, device interrupts and the ITS stay on the boot CPU, and every
 * CPU runs its own scheduler tick. TLB maintenance needs no shootdown: every
 * invalidation in paging.c is broadcast to the inner-shareable domain. */
#include "cpu_local.h"
#include "smp.h"

#if WASMOS_SMP

#include <stdint.h>

#include "acpi_aarch64.h"
#include "arch/arch_cpu.h"
#include "arch/vm_layout.h"
#include "gic_aarch64.h"
#include "kernel_boot_runtime.h"
#include "klog.h"
#include "paging.h"
#include "process.h"

int64_t aarch64_psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context);
void aarch64_timer_start_secondary(void);
extern const uint64_t aarch64_secondary_entry_phys; /* entry.S */

/* MPIDR_EL1 affinity fields (Aff3 at 39:32, Aff2..Aff0 at 23:0), the form
 * cpu_init records and the MADT reports. */
#define MPIDR_AFFINITY_MASK 0xFF00FFFFFFULL

/* MADT (ACPI 6.5, 5.2.12): entries follow the 36-byte header and the 8 bytes
 * of local interrupt controller address and flags. A GICC entry (type 0xB)
 * carries the CPU's MPIDR at offset 68 and its Enabled flag at bit 0 of the
 * flags word at offset 12. */
#define MADT_ENTRIES_OFFSET 44u
#define MADT_TYPE_GICC 0x0Bu
#define MADT_GICC_FLAGS_OFFSET 12u
#define MADT_GICC_MPIDR_OFFSET 68u
#define MADT_GICC_MIN_LENGTH 76u
#define MADT_GICC_ENABLED 1u

/* Boot stack of each secondary, which becomes its scheduler-loop stack.
 * Indexed by cpu_id - 1. */
#define SMP_AP_STACK_SIZE (32u * 1024u)
static uint8_t g_ap_stacks[WASMOS_MAX_CPUS - 1][SMP_AP_STACK_SIZE] __attribute__((aligned(16)));

/* What _start_secondary reads with its MMU off (layout fixed by entry.S). One
 * block serves every secondary: smp_cpus_up starts them one at a time and does
 * not rewrite it until the previous one has published `started`, by which time
 * it has read the block. Aligned to a cache line, so cleaning it touches no
 * other data. */
typedef struct {
    uint64_t ttbr0;
    uint64_t ttbr1;
    uint64_t sp;
    uint64_t cpu_id;
} smp_boot_block_t;

static smp_boot_block_t g_boot_block __attribute__((aligned(64)));

/* Bounded wait for a secondary to come online, in counter ticks: one second. */
static uint64_t counter_after_seconds(uint64_t seconds) {
    uint64_t freq;
    uint64_t now;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
    return now + freq * seconds;
}

static uint64_t counter_now(void) {
    uint64_t now;
    __asm__ volatile("isb\n"
                     "mrs %0, cntvct_el0"
                     : "=r"(now));
    return now;
}

/* Write the block back to the point of coherency: the secondary reads it with
 * its MMU and caches off, straight from memory. */
static void clean_to_poc(const void* addr, uint64_t len) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ULL << ((ctr >> 16) & 0xFu);
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(line - 1u);
    uintptr_t end = (uintptr_t)addr + len;
    for (uintptr_t p = start; p < end; p += line) {
        __asm__ volatile("dc cvac, %0" ::"r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Record every enabled CPU the MADT lists. The boot CPU is matched by its
 * MPIDR and keeps slot 0; the others take slots 1.. in table order, up to
 * WASMOS_MAX_CPUS. Without a MADT the machine stays single-core. */
void smp_init(void) {
    const acpi_sdt_header_t* madt = acpi_find_table("APIC");
    uint32_t count = 1;
    if (madt && madt->length > MADT_ENTRIES_OFFSET) {
        const uint8_t* p = (const uint8_t*)madt + MADT_ENTRIES_OFFSET;
        const uint8_t* end = (const uint8_t*)madt + madt->length;
        while (p + 2 <= end && p[1] >= 2 && p + p[1] <= end) {
            if (p[0] == MADT_TYPE_GICC && p[1] >= MADT_GICC_MIN_LENGTH) {
                uint32_t flags;
                uint64_t mpidr;
                __builtin_memcpy(&flags, p + MADT_GICC_FLAGS_OFFSET, sizeof(flags));
                __builtin_memcpy(&mpidr, p + MADT_GICC_MPIDR_OFFSET, sizeof(mpidr));
                mpidr &= MPIDR_AFFINITY_MASK;
                if ((flags & MADT_GICC_ENABLED) && mpidr != g_cpus[0].arch.mpidr &&
                    count < WASMOS_MAX_CPUS) {
                    g_cpus[count].cpu_id = count;
                    g_cpus[count].arch.mpidr = mpidr;
                    count++;
                }
            }
            p += p[1];
        }
    }
    g_cpu_count = count;
    klog_printf("[smp] boot mpidr=%llx cpu_count=%u\n",
                (unsigned long long)g_cpus[0].arch.mpidr,
                (unsigned)g_cpu_count);
}

/* C entry of a secondary, from _secondary_high in entry.S: MMU on with the
 * kernel tables, IRQs masked, on its own boot stack. Never returns.
 *
 * Same order as x86_64's: the redistributor, scheduler state and idle thread
 * exist before the timer can tick, and `started` is published last, with
 * release ordering, because the boot CPU and the scheduler take it to mean
 * "safe to schedule on". A CPU whose redistributor cannot be found never
 * publishes it and parks. */
void smp_ap_c_entry(uint32_t cpu_id) {
    cpu_local_t* cpu = &g_cpus[cpu_id];
    cpu->self = cpu;
    __asm__ volatile("msr tpidr_el1, %0" ::"r"(cpu) : "memory");

    if (gic_cpu_init() != 0) {
        klog_printf("[smp] cpu %u: no redistributor for mpidr %llx\n",
                    (unsigned)cpu_id,
                    (unsigned long long)cpu->arch.mpidr);
        arch_halt_forever();
    }
    process_ap_init();
    if (process_spawn_idle_ap(cpu_id) != 0) {
        klog_printf("[smp] idle-ap %u install failed\n", (unsigned)cpu_id);
    }
    aarch64_timer_start_secondary();

    __atomic_store_n(&cpu->started, 1u, __ATOMIC_RELEASE);
    kernel_boot_run_scheduler_loop();
}

/* Start each recorded secondary in turn and wait up to a second for it to
 * publish `started`. A CPU that PSCI refuses, or that never arrives, is
 * reported and left out: it stays counted with started == 0, which the
 * scheduler and kpanic already treat as offline. Boot CPU only, once, after the
 * scheduler is ready. */
void smp_cpus_up(void) {
    if (g_cpu_count <= 1u) {
        klog_write("[smp] no secondary CPUs to bring up\n");
        return;
    }
    uint64_t ttbr1;
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
    uint64_t block_phys = (uint64_t)((uintptr_t)&g_boot_block - (uintptr_t)KERNEL_HIGHER_HALF_BASE);
    uint64_t entry_phys = aarch64_secondary_entry_phys;

    for (uint32_t i = 1; i < g_cpu_count; ++i) {
        cpu_local_t* ap = &g_cpus[i];
        ap->started = 0;
        g_boot_block.ttbr0 = paging_get_root_table();
        g_boot_block.ttbr1 = ttbr1 & ~(0xFFFFULL << 48); /* drop the ASID field */
        g_boot_block.sp = (uint64_t)(uintptr_t)(g_ap_stacks[i - 1u] + SMP_AP_STACK_SIZE);
        g_boot_block.cpu_id = i;
        clean_to_poc(&g_boot_block, sizeof(g_boot_block));

        int64_t rc = aarch64_psci_cpu_on(ap->arch.mpidr, entry_phys, block_phys);
        if (rc != 0) {
            klog_printf("[smp] cpu %u (mpidr %llx): CPU_ON failed (%lld)\n",
                        (unsigned)i,
                        (unsigned long long)ap->arch.mpidr,
                        (long long)rc);
            continue;
        }
        uint64_t deadline = counter_after_seconds(1);
        while (!__atomic_load_n(&ap->started, __ATOMIC_ACQUIRE) && counter_now() < deadline) {
            arch_cpu_relax();
        }
        if (__atomic_load_n(&ap->started, __ATOMIC_ACQUIRE)) {
            klog_printf("[smp] cpu %u online (mpidr %llx)\n",
                        (unsigned)i,
                        (unsigned long long)ap->arch.mpidr);
        } else {
            klog_printf("[smp] cpu %u TIMEOUT -- not responding\n", (unsigned)i);
        }
    }
}

/* kpanic's stop request: an SGI to every other CPU. Unlike x86_64's NMI it is
 * an ordinary interrupt, so a CPU spinning with IRQs masked does not take it and
 * kpanic reports that CPU as uncaptured.
 * TODO: deliver the stop as a GICv3 pseudo-NMI (priority masking through
 * ICC_PMR_EL1 instead of DAIF.I) so a CPU spinning with IRQs masked is stopped
 * too. */
void arch_stop_other_cpus(void) {
    gic_send_sgi_others(GIC_SGI_STOP);
}

#endif /* WASMOS_SMP */
