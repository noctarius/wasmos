/* cpu_aarch64.c - aarch64 per-CPU setup and exception reporting. Implements
 * cpu.h and the per-CPU array of cpu_local.h. */
#include <stdint.h>

#include "arch/arch_context.h"
#include "arch/arch_cpu.h"
#include "cpu.h"
#include "cpu_local.h"
#include "irq.h"
#include "klog.h"
#include "kpanic.h"
#include "memory.h"
#include "memory_service.h"
#include "process.h"

void aarch64_irq(irq_frame_t* frame);

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
 * build, and bring the interrupt controller up with every interrupt disabled.
 * entry.S installed the exception vectors before kmain. */
void cpu_init(void) {
    cpu_local_t* bsp = &g_cpus[0];
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    bsp->self = bsp;
    bsp->cpu_id = 0;
    bsp->started = 1;
    bsp->arch.mpidr = mpidr & 0xFF00FFFFFFULL;
    __asm__ volatile("msr tpidr_el1, %0" ::"r"(bsp) : "memory");
    irq_init();
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

/* Vector indices (vectors.S): synchronous and IRQ, taken from EL1 on SP_EL1
 * and from EL0. */
#define VECTOR_EL1_SYNC 4u
#define VECTOR_EL1_IRQ 5u
#define VECTOR_EL0_SYNC 8u
#define VECTOR_EL0_IRQ 9u

/* ESR_EL1 exception classes and abort fields (ARM ARM D17.2.37). */
#define ESR_EC(esr) (((esr) >> 26) & 0x3Fu)
#define ESR_EC_IABT_LOWER 0x20u
#define ESR_EC_IABT_SAME 0x21u
#define ESR_EC_DABT_LOWER 0x24u
#define ESR_EC_DABT_SAME 0x25u
#define ESR_ISS_WNR (1u << 6)
#define ESR_ISS_FSC(esr) ((esr) & 0x3Fu)
/* Fault status codes 0b0010LL (access flag) and 0b0011LL (permission): a
 * translation existed. 0b0001LL (translation) means none did. */
#define FSC_IS_ACCESS_FLAG(fsc) (((fsc) & 0x3Cu) == 0x08u)
#define FSC_IS_PERMISSION(fsc) (((fsc) & 0x3Cu) == 0x0Cu)

/* An instruction or data abort as an MM_FAULT_* mask. */
static uint32_t abort_access(uint64_t esr) {
    uint32_t ec = (uint32_t)ESR_EC(esr);
    uint32_t fsc = (uint32_t)ESR_ISS_FSC(esr);
    uint32_t access = 0;
    if (FSC_IS_ACCESS_FLAG(fsc) || FSC_IS_PERMISSION(fsc)) {
        access |= MM_FAULT_PRESENT;
    }
    if (ec == ESR_EC_IABT_LOWER || ec == ESR_EC_DABT_LOWER) {
        access |= MM_FAULT_USER;
    }
    if (ec == ESR_EC_IABT_LOWER || ec == ESR_EC_IABT_SAME) {
        access |= MM_FAULT_EXEC;
    } else if (esr & ESR_ISS_WNR) {
        access |= MM_FAULT_WRITE;
    }
    return access;
}

/* Service an instruction or data abort through the memory service, the
 * neutral fault path (memory.h MM_FAULT_*). Returns 0 when the fault was
 * resolved or the faulting user process was terminated, -1 when a kernel
 * access could not be resolved. Unlike x86_64 there is no stale-TLB retry:
 * every invalidation here is broadcast (paging.c). */
static int handle_abort(uint64_t esr, uint64_t far, const irq_frame_t* frame) {
    uint32_t pid = process_current_pid();
    process_t* proc = process_get(pid);
    if (!proc) {
        return -1;
    }
    uint32_t access = abort_access(esr);
    if (memory_service_handle_fault_ipc(proc->context_id, far, access) == 0) {
        return 0;
    }
    if (access & MM_FAULT_USER) {
        klog_printf("[cpu] user page fault terminate pid=%u esr=%016llx far=%016llx elr=%016llx\n",
                    (unsigned int)pid,
                    (unsigned long long)esr,
                    (unsigned long long)far,
                    (unsigned long long)frame->elr);
        process_set_exit(proc, -11, PROC_EXIT_REASON_FAULTED);
        process_yield(PROCESS_RUN_EXITED);
        return 0;
    }
    klog_write("[cpu] page fault not handled\n");
    return -1;
}

/* Called from vectors.S for every exception. `kind` is the vector index. IRQs
 * go to the GIC dispatch; instruction and data aborts to the memory service.
 * TODO: SVC into syscall_dispatch (arm64 port, Phase 2). Every other
 * exception is fatal. */
void aarch64_exception(irq_frame_t* frame, uint64_t kind) {
    if (kind == VECTOR_EL1_IRQ || kind == VECTOR_EL0_IRQ) {
        aarch64_irq(frame);
        return;
    }
    uint64_t esr;
    uint64_t far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    if (kind == VECTOR_EL1_SYNC || kind == VECTOR_EL0_SYNC) {
        uint32_t ec = (uint32_t)ESR_EC(esr);
        if ((ec == ESR_EC_IABT_LOWER || ec == ESR_EC_IABT_SAME || ec == ESR_EC_DABT_LOWER ||
             ec == ESR_EC_DABT_SAME) &&
            handle_abort(esr, far, frame) == 0) {
            return;
        }
    }
    klog_printf("[cpu] unhandled exception kind=%llu esr=%016llx far=%016llx\n",
                (unsigned long long)kind,
                (unsigned long long)esr,
                (unsigned long long)far);
    arch_irq_frame_log(frame);
    kpanic("aarch64_unhandled_exception", esr, frame->elr);
}
