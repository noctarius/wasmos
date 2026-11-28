/* warp_ring3_arch.h - the architecture half of WARP's ring-3 execution model
 * (docs/architecture/31-warp-ring3-implementation.md): the machine code in the
 * trampoline pages, the transition into user mode, and where a host call's
 * arguments are found. Everything else in the model -- the user address space,
 * the VA layout in warp_ring3.h, the syscall numbers and the dispatch -- is
 * neutral.
 *
 * Each architecture header defines WARP_R3_HC_STUB_BYTES, WARP_R3_HC_REG_ARGS,
 * WARP_R3_HC_STACK_ARGS_AT and the inline arch_warp_r3_hostcall_arg; the
 * functions declared below are implemented in
 * src/kernel/arch/<arch>/warp_ring3_<arch>.c. This header is the single
 * selection point. */
#pragma once

#include <stdint.h>

#include "arch/arch_context.h"
#include "warp_ring3.h"

#if defined(WASMOS_ARCH_AARCH64)
#include "arch/aarch64/warp_ring3_arch.h"
#else
#include "arch/x86_64/warp_ring3_arch.h"
#endif

#ifdef __cplusplus
static_assert(WARP_HC_MAX * WARP_R3_HC_STUB_BYTES <= 4096u,
              "the host-call stubs must fit the one WARP_R3_HC_TRAMPOLINE page");
#else
_Static_assert(WARP_HC_MAX* WARP_R3_HC_STUB_BYTES <= 4096u,
               "the host-call stubs must fit the one WARP_R3_HC_TRAMPOLINE page");
#endif

/* Whether every stack slot a host call of `nargs` arguments reads lies inside
 * the ring-3 stack, [WARP_R3_STACK_BASE, WARP_R3_STACK_TOP). The stack pointer
 * is the guest's to set, so it is checked before any argument is read through
 * it; a call that needs no stack argument needs no check. */
static inline int arch_warp_r3_hostcall_stack_ok(const syscall_frame_t* frame, unsigned nargs) {
    if (nargs <= WARP_R3_HC_REG_ARGS) {
        return 1;
    }
    uint64_t span = WARP_R3_HC_STACK_ARGS_AT + (uint64_t)(nargs - WARP_R3_HC_REG_ARGS) * 8u;
    uint64_t sp = arch_syscall_user_sp(frame);
    return sp >= WARP_R3_STACK_BASE && sp <= WARP_R3_STACK_TOP - span;
}

#ifdef __cplusplus
extern "C" {
#endif

/* Write the host-call stub for syscall number `syscall_nr` at `dst`, exactly
 * WARP_R3_HC_STUB_BYTES bytes. The JIT calls the stub with its native calling
 * convention; the stub raises the syscall with `syscall_nr` as the number and
 * the caller's argument registers untouched, and returns to the JIT with the
 * syscall result in the native return register. */
void arch_warp_r3_write_hc_stub(uint8_t* dst, uint32_t syscall_nr);

/* Write the return-trampoline page at `page` (one zeroed 4 KiB page, the
 * kernel alias of the page mapped at WARP_R3_RET_TRAMPOLINE):
 *   +0x00  return stub: raises WASMOS_SYSCALL_WARP_RETURN with the JIT
 *          wrapper's return value as syscall argument 0; never returns.
 *   +0x10  memory-helper stub (WARP_R3_MEMHELPER_TRAMPOLINE): raises
 *          WASMOS_SYSCALL_WARP_MEMORY_HELPER with the caller's argument
 *          registers untouched and returns the result.
 *   +0x20  entry stub (WARP_R3_ENTRY_TRAMPOLINE): records the user stack
 *          pointer at WARP_R3_STACK_BASE + 64 and jumps to the address stored
 *          at WARP_R3_STACK_BASE + 56, preserving the argument registers.
 * Each stub fits in its 16 bytes. */
void arch_warp_r3_write_return_page(uint8_t* page);

/* One ring-3 call into a JIT wrapper. */
typedef struct {
    uint64_t entry_va;         /* user VA execution starts at */
    uint64_t return_va;        /* user VA the wrapper returns to */
    uint64_t stack_top_va;     /* user VA one past the top of the ring-3 stack */
    uint8_t* stack_top_kernel; /* kernel alias of stack_top_va */
    uint64_t args[4];          /* the wrapper's first four integer arguments */
} arch_warp_r3_call_t;

/* Leave the kernel for user mode at call->entry_va, with interrupts enabled,
 * call->args in the first four argument registers, and the stack pointer and
 * return address arranged so that the wrapper's return lands on
 * call->return_va. Writes to the top of the ring-3 stack through
 * call->stack_top_kernel where the architecture keeps the return address
 * there. The current page-table root must already be the guest's. Never
 * returns: the guest comes back through a syscall. */
__attribute__((noreturn)) void arch_warp_r3_enter(const arch_warp_r3_call_t* call);

#ifdef __cplusplus
}
#endif
