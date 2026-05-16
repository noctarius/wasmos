#ifndef WASMOS_ARCH_X86_64_CPU_X86_64_H
#define WASMOS_ARCH_X86_64_CPU_X86_64_H

#include <stdint.h>

void x86_cpu_init(void);
void x86_cpu_set_kernel_stack(uint64_t rsp0);
void x86_cpu_relocate_tables_high(void);
void x86_cpu_enable_interrupts(void);
void x86_cpu_disable_interrupts(void);

#endif
