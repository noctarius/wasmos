#ifndef WASMOS_CPU_H
#define WASMOS_CPU_H

#include <stdint.h>

void cpu_init(void);
int x86_page_fault_handler(uint64_t error_code);
void cpu_enable_interrupts(void);
void cpu_disable_interrupts(void);

#endif
