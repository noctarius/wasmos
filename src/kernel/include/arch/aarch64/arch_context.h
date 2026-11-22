/* arch/aarch64/arch_context.h - aarch64 register frames and context
 * accessors. Included only through arch/arch_context.h; the contract of each
 * accessor is the one documented in the x86_64 header.
 *
 * Every frame stores the general registers x0..x30, the interrupted stack
 * pointer, ELR (the resume PC) and SPSR (the saved PSTATE). SPSR.M[3:0] is the
 * exception level and stack the context runs on: EL0t (0b0000) is user mode,
 * EL1h (0b0101) is kernel mode on SP_EL1; no other value is legitimate.
 *
 * TODO: the exception vectors, the context switch and the SVC entry that build
 * and consume these frames do not exist yet (arm64 port, Phase 2); the layouts
 * are fixed here so that code can be written against them. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void (*arch_stack_fn_t)(void);

uint64_t arch_call_on_stack(uintptr_t stack_top, arch_stack_fn_t fn, uint64_t a0, uint64_t a1,
                            uint64_t a2);

static inline __attribute__((always_inline)) uintptr_t arch_read_sp(void) {
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    return sp;
}

static inline __attribute__((always_inline)) uintptr_t arch_read_pc(void) {
    uintptr_t pc;
    __asm__ volatile("adr %0, ." : "=r"(pc));
    return pc;
}

/* x29 is the frame pointer (AAPCS64 6.2.3): [fp] holds the previous frame
 * pointer and [fp + 8] the return address, the layout arch_context.h's
 * contract names. */
static inline __attribute__((always_inline)) uintptr_t arch_read_fp(void) {
    uintptr_t fp;
    __asm__ volatile("mov %0, x29" : "=r"(fp));
    return fp;
}

/* The DAIF mask bits, the part of PSTATE readable without an exception. */
static inline __attribute__((always_inline)) uint64_t arch_read_flags(void) {
    uint64_t flags;
    __asm__ volatile("mrs %0, daif" : "=r"(flags));
    return flags;
}

#define AARCH64_SPSR_MODE_MASK 0xFu
#define AARCH64_SPSR_EL0T 0x0u
#define AARCH64_SPSR_EL1H 0x5u
/* Interrupts (IRQ) unmasked, FIQ/SError/debug masked: the PSTATE a fresh
 * context starts with. */
#define AARCH64_SPSR_DAIF_FIQ_SERR_DBG 0x340u

/* Saved state of a thread. `sp` is the kernel stack pointer (SP_EL1) to resume
 * on; `user_sp` is SP_EL0. `root_table` is the physical address of the
 * thread's TTBR0 table and is the one field neutral code uses directly. */
typedef struct {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t user_sp;
    uint64_t root_table;
} process_context_t;

_Static_assert(offsetof(process_context_t, sp) == 248, "process_context_t sp offset mismatch");
_Static_assert(offsetof(process_context_t, root_table) == 280,
               "process_context_t root_table offset mismatch");

/* Frame an exception entry builds on the kernel stack. `sp` is the interrupted
 * context's stack pointer: SP_EL0 for an EL0 frame, SP_EL1 as it was before
 * the frame was pushed for an EL1 frame; the exit restores it from here. */
typedef struct {
    uint64_t x[31];
    uint64_t sp;
    uint64_t elr;
    uint64_t spsr;
} irq_frame_t;

_Static_assert(sizeof(irq_frame_t) == 272, "irq_frame_t size mismatch");

static inline void arch_context_clear(process_context_t* ctx) {
    __builtin_memset(ctx, 0, sizeof(*ctx));
}

static inline void arch_context_init_kernel_entry(process_context_t* ctx, uint64_t pc,
                                                  uint64_t kernel_sp) {
    ctx->sp = kernel_sp;
    ctx->user_sp = kernel_sp;
    ctx->pc = pc;
    ctx->pstate = AARCH64_SPSR_EL1H | AARCH64_SPSR_DAIF_FIQ_SERR_DBG;
}

static inline void arch_context_set_user_entry(process_context_t* ctx, uint64_t pc,
                                               uint64_t user_sp) {
    ctx->pc = pc;
    ctx->user_sp = user_sp;
    ctx->pstate = AARCH64_SPSR_EL0T;
}

static inline void arch_context_set_kernel_sp(process_context_t* ctx, uint64_t sp) {
    ctx->sp = sp;
}

static inline void arch_context_set_pc(process_context_t* ctx, uint64_t pc) {
    ctx->pc = pc;
}

static inline void arch_context_set_return(process_context_t* ctx, uint64_t value) {
    ctx->x[0] = value;
}

static inline uint64_t arch_context_pc(const process_context_t* ctx) {
    return ctx->pc;
}

static inline uint64_t arch_context_kernel_sp(const process_context_t* ctx) {
    return ctx->sp;
}

static inline uint64_t arch_context_fp(const process_context_t* ctx) {
    return ctx->x[29];
}

static inline uint64_t arch_context_flags(const process_context_t* ctx) {
    return ctx->pstate;
}

static inline uint64_t arch_context_mode(const process_context_t* ctx) {
    return ctx->pstate & AARCH64_SPSR_MODE_MASK;
}

static inline int arch_context_is_user(const process_context_t* ctx) {
    return (ctx->pstate & AARCH64_SPSR_MODE_MASK) == AARCH64_SPSR_EL0T;
}

/* Record the caller's callee-saved registers (x19..x29), link register, stack
 * pointer and DAIF into ctx. */
static inline __attribute__((always_inline)) void
arch_context_snapshot_callee_saved(process_context_t* ctx) {
    uint64_t sp;
    uint64_t daif;
    __asm__ volatile("stp x19, x20, [%2, #152]\n"
                     "stp x21, x22, [%2, #168]\n"
                     "stp x23, x24, [%2, #184]\n"
                     "stp x25, x26, [%2, #200]\n"
                     "stp x27, x28, [%2, #216]\n"
                     "stp x29, x30, [%2, #232]\n"
                     "mov %0, sp\n"
                     "mrs %1, daif"
                     : "=&r"(sp), "=&r"(daif)
                     : "r"(ctx)
                     : "memory");
    ctx->sp = sp;
    ctx->pstate = AARCH64_SPSR_EL1H | daif;
}

typedef enum {
    ARCH_IRQ_FRAME_INVALID = 0, /* malformed: neither a kernel nor a user frame */
    ARCH_IRQ_FRAME_KERNEL,
    ARCH_IRQ_FRAME_USER,
} arch_irq_frame_origin_t;

static inline arch_irq_frame_origin_t arch_irq_frame_origin(const irq_frame_t* frame) {
    uint64_t mode = frame->spsr & AARCH64_SPSR_MODE_MASK;
    if (frame->elr == 0) {
        return ARCH_IRQ_FRAME_INVALID;
    }
    if (mode == AARCH64_SPSR_EL1H) {
        return ARCH_IRQ_FRAME_KERNEL;
    }
    if (mode == AARCH64_SPSR_EL0T && frame->sp != 0) {
        return ARCH_IRQ_FRAME_USER;
    }
    return ARCH_IRQ_FRAME_INVALID;
}

void arch_irq_frame_log(const irq_frame_t* frame);

static inline void arch_context_save_irq_frame(process_context_t* ctx, const irq_frame_t* frame) {
    for (unsigned i = 0; i < 31u; ++i) {
        ctx->x[i] = frame->x[i];
    }
    ctx->user_sp = frame->sp;
    ctx->pc = frame->elr;
    ctx->pstate = frame->spsr;
}

static inline void arch_irq_frame_redirect_kernel(irq_frame_t* frame, uint64_t pc, uint64_t sp) {
    frame->spsr = (frame->spsr & ~(uint64_t)AARCH64_SPSR_MODE_MASK) | AARCH64_SPSR_EL1H;
    frame->elr = pc;
    frame->sp = sp;
}

/* An SVC is a synchronous exception, so the syscall frame is the exception
 * frame. Convention: number in x8, arguments in x0..x5, results in x0 and x1
 * (the Linux arm64 register assignment). */
typedef irq_frame_t syscall_frame_t;

static inline uint64_t arch_syscall_number(const syscall_frame_t* frame) {
    return frame->x[8];
}

static inline uint64_t arch_syscall_arg(const syscall_frame_t* frame, unsigned index) {
    return index < 6u ? frame->x[index] : 0;
}

static inline void arch_syscall_set_result(syscall_frame_t* frame, uint64_t value) {
    frame->x[0] = value;
}

static inline uint64_t arch_syscall_result(const syscall_frame_t* frame) {
    return frame->x[0];
}

static inline void arch_syscall_set_result2(syscall_frame_t* frame, uint64_t value) {
    frame->x[1] = value;
}

static inline uint64_t arch_syscall_result2(const syscall_frame_t* frame) {
    return frame->x[1];
}

static inline int arch_syscall_from_user(const syscall_frame_t* frame) {
    return (frame->spsr & AARCH64_SPSR_MODE_MASK) == AARCH64_SPSR_EL0T;
}

static inline uint64_t arch_syscall_user_sp(const syscall_frame_t* frame) {
    return frame->sp;
}

static inline void arch_context_save_syscall_frame(process_context_t* ctx,
                                                   const syscall_frame_t* frame) {
    arch_context_save_irq_frame(ctx, frame);
}
