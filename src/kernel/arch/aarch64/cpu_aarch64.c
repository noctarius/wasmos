/* cpu_aarch64.c - aarch64 per-CPU setup and exception reporting. Implements
 * cpu.h and the per-CPU array of cpu_local.h. */
#include <stdint.h>

#include "arch/arch_context.h"
#include "arch/arch_cpu.h"
#include "cpu.h"
#include "cpu_local.h"
#include "klog.h"
#include "kpanic.h"

/* g_cpus[0] is the boot CPU. Only one CPU runs until PSCI CPU_ON bring-up
 * exists. */
cpu_local_t g_cpus[WASMOS_MAX_CPUS];
uint32_t g_cpu_count = 1;

/* Exception level _start was entered at (1 or 2), stored by entry.S. It
 * selects the PSCI conduit: firmware below an EL2 entry answers SMC, a
 * hypervisor below an EL1 entry answers HVC.
 * TODO: read the conduit from the FADT ARM boot flags instead. */
uint32_t aarch64_boot_el;

/* Point TPIDR_EL1 at the boot CPU's slot, so cpu_local() resolves on an SMP
 * build. entry.S installed the exception vectors before kmain. */
void cpu_init(void) {
    cpu_local_t* bsp = &g_cpus[0];
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    bsp->self = bsp;
    bsp->cpu_id = 0;
    bsp->started = 1;
    bsp->arch.mpidr = mpidr & 0xFF00FFFFFFULL;
    __asm__ volatile("msr tpidr_el1, %0" ::"r"(bsp) : "memory");
}

/* The kernel runs at its higher-half addresses from entry.S on, and the vector
 * table and per-CPU pointer already hold higher-half addresses: nothing to
 * relocate. */
void cpu_relocate_tables_high(void) {}

/* The stack an exception from EL0 enters on.
 * TODO: record it for the EL0 entry path; until EL0 exists every exception is
 * taken on the current SP_EL1 (arm64 port, Phase 2). */
void cpu_set_kernel_stack(uint64_t rsp0) {
    (void)rsp0;
}

void arch_irq_frame_log(const irq_frame_t* frame) {
    klog_printf("[irq] frame elr=%016llx spsr=%016llx sp=%016llx x0=%016llx x30=%016llx\n",
                (unsigned long long)frame->elr,
                (unsigned long long)frame->spsr,
                (unsigned long long)frame->sp,
                (unsigned long long)frame->x[0],
                (unsigned long long)frame->x[30]);
}

/* Called from vectors.S for every exception. `kind` is the vector index.
 * TODO: synchronous-exception decoding (data/instruction aborts into the
 * MM_FAULT_* path, SVC into syscall_dispatch) and IRQ dispatch through the GIC
 * (arm64 port, Phase 2). Until then every exception is fatal. */
void aarch64_exception(irq_frame_t* frame, uint64_t kind) {
    uint64_t esr;
    uint64_t far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    klog_printf("[cpu] unhandled exception kind=%llu esr=%016llx far=%016llx\n",
                (unsigned long long)kind,
                (unsigned long long)esr,
                (unsigned long long)far);
    arch_irq_frame_log(frame);
    kpanic("aarch64_unhandled_exception", esr, frame->elr);
}
