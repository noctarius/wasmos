/* warp_ring3_aarch64.c - aarch64 machine code for WARP's ring-3 trampolines and
 * the entry into a JIT wrapper. The contract is in arch/warp_ring3_arch.h.
 *
 * FIXME: not implemented yet. Each hook panics by name, so a WARP aarch64 kernel
 * builds and boots until the first ring-3 WARP module is set up, and stops there
 * with the missing piece named. */

#include <stdint.h>

#include "arch/warp_ring3_arch.h"
#include "kpanic.h"

void arch_warp_r3_write_hc_stub(uint8_t* dst, uint32_t syscall_nr) {
    kpanic("warp_r3_aarch64_hc_stub_unimplemented", (uint64_t)(uintptr_t)dst, syscall_nr);
}

void arch_warp_r3_write_return_page(uint8_t* page) {
    kpanic("warp_r3_aarch64_return_page_unimplemented", (uint64_t)(uintptr_t)page, 0);
}

void arch_warp_r3_enter(const arch_warp_r3_call_t* call) {
    kpanic("warp_r3_aarch64_enter_unimplemented", call->entry_va, call->stack_top_va);
}
