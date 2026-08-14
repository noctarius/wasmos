/* cpu.c - x86_64 CPU early-init wrapper: delegates to arch/x86_64/cpu_x86_64.c.
 * Also hosts the exception/page-fault dispatch called from cpu_isr.S stubs.
 *
 * Every entry point here acts on the CALLING CPU only: the GDT, IDT pointer and
 * TSS all live in that CPU's cpu_local_t slot, so an AP must call cpu_init()
 * itself rather than inherit the BSP's tables.  These are thin forwarders; the
 * behaviour, including x86_cpu_set_kernel_stack's rsp0 == 0 no-op and the
 * BSP-only, idempotent nature of x86_cpu_relocate_tables_high, is documented at
 * the arch implementations. */
#include "cpu.h"
#include "arch/x86_64/cpu_x86_64.h"

void cpu_init(void) {
    x86_cpu_init();
}

void cpu_set_kernel_stack(uint64_t rsp0) {
    x86_cpu_set_kernel_stack(rsp0);
}

void cpu_relocate_tables_high(void) {
    x86_cpu_relocate_tables_high();
}

void cpu_enable_interrupts(void) {
    x86_cpu_enable_interrupts();
}

void cpu_disable_interrupts(void) {
    x86_cpu_disable_interrupts();
}
