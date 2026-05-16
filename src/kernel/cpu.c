#include "cpu.h"
#include "arch/x86_64/cpu_x86_64.h"

void
cpu_init(void)
{
    x86_cpu_init();
}

void
cpu_set_kernel_stack(uint64_t rsp0)
{
    x86_cpu_set_kernel_stack(rsp0);
}

void
cpu_relocate_tables_high(void)
{
    x86_cpu_relocate_tables_high();
}

void
cpu_enable_interrupts(void)
{
    x86_cpu_enable_interrupts();
}

void
cpu_disable_interrupts(void)
{
    x86_cpu_disable_interrupts();
}
