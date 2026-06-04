/* cpu.h - x86_64 CPU setup and exception dispatch.
 * Covers GDT/TSS/IDT initialization, syscall gate, and exception handlers. */
#ifndef WASMOS_CPU_H
#define WASMOS_CPU_H

#include <stdint.h>

#define X86_VECTOR_SYSCALL 0x80u  /* int 0x80 is the WASMOS syscall vector */

/* Program GDT, TSS, and IDT; called once on the BSP during kernel startup. */
void cpu_init(void);

/* Relocate GDT/TSS pointers to their higher-half VA aliases after paging is active. */
void cpu_relocate_tables_high(void);

/* Handle a CPU exception from ring-3; delivers a SIGFPE-equivalent or kills the process. */
int x86_user_exception_handler(uint64_t vector, const uint64_t *frame);

/* Handle a page fault; may invoke demand paging or kill the faulting process. */
int x86_page_fault_handler(uint64_t error_code, const uint64_t *frame);

/* Update TSS.RSP0 so the next ring-3 → ring-0 transition lands on the correct stack. */
void cpu_set_kernel_stack(uint64_t rsp0);

void cpu_enable_interrupts(void);
void cpu_disable_interrupts(void);

#endif
