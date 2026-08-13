/* cpu.h - x86_64 CPU setup and exception dispatch.
 * Covers GDT/TSS/IDT initialization, syscall gate, and exception handlers. */
#ifndef WASMOS_CPU_H
#define WASMOS_CPU_H

#include <stdint.h>

#define X86_VECTOR_SYSCALL 0x80u /* int 0x80 is the WASMOS syscall vector */

/* Program GDT, TSS, and IDT; called once on the BSP during kernel startup. */
void cpu_init(void);

/* Relocate GDT/TSS pointers to their higher-half VA aliases after paging is active. */
void cpu_relocate_tables_high(void);

/* Handle a CPU exception raised in ring-3. `frame` is the CPU's interrupt frame
 * (rip at [1], cs at [2]). There is no signal delivery: a claimed fault
 * terminates the process with exit status -11 and never returns to the faulting
 * instruction. Returns 0 when the process was terminated, -1 when the fault is
 * declined -- no current process, cs says the fault came from ring 0, or the
 * vector is not one of the classified user vectors -- leaving the caller to
 * escalate to a kernel panic. */
int x86_user_exception_handler(uint64_t vector, const uint64_t* frame);

/* Handle a page fault; may satisfy it by demand paging, by retrying a stale TLB
 * entry in the shared linmem window, or by killing the faulting process.
 * Returns 0 when the fault was resolved or absorbed, -1 when it was not and the
 * caller must panic. */
int x86_page_fault_handler(uint64_t error_code, const uint64_t* frame);

/* Update TSS.RSP0 so the next ring-3 → ring-0 transition lands on the correct stack. */
void cpu_set_kernel_stack(uint64_t rsp0);

void cpu_enable_interrupts(void);
void cpu_disable_interrupts(void);

#endif
