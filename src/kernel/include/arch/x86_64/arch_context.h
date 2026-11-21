/* arch_context.h - x86_64 implementation of the execution-context primitives.
 * Included only through arch/arch_context.h. */
#pragma once

#include <stdint.h>

/* Generic function-pointer type for arch_call_on_stack; callers cast their
 * function to it. */
typedef void (*arch_stack_fn_t)(void);

/* Implemented in context_switch.S. */
uint64_t arch_call_on_stack(uintptr_t stack_top, arch_stack_fn_t fn, uint64_t a0, uint64_t a1,
                            uint64_t a2);

/* Current stack pointer of the calling code. */
static inline uintptr_t arch_read_sp(void) {
    uintptr_t sp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    return sp;
}

/* Address of the instruction following this read in the calling code. Tells
 * which alias of the kernel image (identity or higher-half) is executing. */
static inline uintptr_t arch_read_pc(void) {
    uintptr_t pc;
    __asm__ volatile("leaq 0f(%%rip), %0\n0:" : "=r"(pc));
    return pc;
}
