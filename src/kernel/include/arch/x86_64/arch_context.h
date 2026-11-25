/* arch_context.h - x86_64 implementation of the execution-context primitives.
 * Included only through arch/arch_context.h. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Generic function-pointer type for arch_call_on_stack; callers cast their
 * function to it. */
typedef void (*arch_stack_fn_t)(void);

/* Implemented in context_switch.S. */
uint64_t arch_call_on_stack(uintptr_t stack_top, arch_stack_fn_t fn, uint64_t a0, uint64_t a1,
                            uint64_t a2);

/* The register readers below are always inlined: the kernel builds without
 * optimisation, and an out-of-line reader would report its own frame instead
 * of the caller's. */

/* Current stack pointer of the calling code. */
static inline __attribute__((always_inline)) uintptr_t arch_read_sp(void) {
    uintptr_t sp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    return sp;
}

/* Address of the instruction following this read in the calling code. Tells
 * which alias of the kernel image (identity or higher-half) is executing. */
static inline __attribute__((always_inline)) uintptr_t arch_read_pc(void) {
    uintptr_t pc;
    __asm__ volatile("leaq 0f(%%rip), %0\n0:" : "=r"(pc));
    return pc;
}

/* Frame pointer (rbp) of the calling code: the head of its frame-record chain,
 * where [fp] is the caller's frame pointer and [fp + 8] its return address. */
static inline __attribute__((always_inline)) uintptr_t arch_read_fp(void) {
    uintptr_t fp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(fp));
    return fp;
}

/* Processor flags word (RFLAGS) of the calling code. Diagnostic only. */
static inline __attribute__((always_inline)) uint64_t arch_read_flags(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    return flags;
}

/* Segment selectors as they appear in a saved CS/SS: the GDT index plus the
 * requested privilege level (RPL 3 for user). The GDT itself is built in
 * cpu_x86_64.c. */
#define X86_SEL_KERNEL_CS 0x08u
#define X86_SEL_KERNEL_DS 0x10u
#define X86_SEL_USER_CS 0x1Bu
#define X86_SEL_USER_DS 0x23u
/* RFLAGS.IF: a fresh context starts with interrupts enabled. */
#define X86_RFLAGS_IF 0x200u

/* Saved execution state of one thread, written and read by context_switch /
 * context_switch_to in arch/x86_64/context_switch.S. The assembly addresses
 * every field by hard-coded byte displacement, which is why the asserts below
 * pin the layout: reordering a field silently reinterprets registers.
 *
 * `rsp` is the ring-0 (kernel) stack pointer; `user_rsp` is the ring-3 one and
 * matters only when cs says ring 3, in which case the resume goes through iretq
 * rather than the ret fast path. `root_table` is the physical CR3 value loaded
 * for this context; 0 means "not yet resolved" and the dispatcher fills it in
 * from the owning mm context. */
typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
    uint64_t cs;
    uint64_t ss;
    uint64_t user_rsp;
    uint64_t root_table;
} process_context_t;

_Static_assert(offsetof(process_context_t, r15) == 0, "process_context_t r15 offset mismatch");
_Static_assert(offsetof(process_context_t, rdi) == 64, "process_context_t rdi offset mismatch");
_Static_assert(offsetof(process_context_t, rsp) == 120, "process_context_t rsp offset mismatch");
_Static_assert(offsetof(process_context_t, rip) == 128, "process_context_t rip offset mismatch");
_Static_assert(offsetof(process_context_t, rflags) == 136,
               "process_context_t rflags offset mismatch");
_Static_assert(offsetof(process_context_t, cs) == 144, "process_context_t cs offset mismatch");
_Static_assert(offsetof(process_context_t, ss) == 152, "process_context_t ss offset mismatch");
_Static_assert(offsetof(process_context_t, user_rsp) == 160,
               "process_context_t user_rsp offset mismatch");
_Static_assert(offsetof(process_context_t, root_table) == 168,
               "process_context_t root_table offset mismatch");

/* The register block an interrupt stub builds on the kernel stack: the GPRs it
 * pushed, followed by the five words the CPU itself pushed (rip, cs, rflags,
 * and — only on a privilege change — user_rsp/user_ss). Layout is pinned by the
 * asserts below because cpu_isr.S builds it by hand.
 *
 * process_preempt_from_irq both reads this frame (to snapshot the interrupted
 * thread) and REWRITES rip/cs/user_rsp/user_ss in place to redirect the iretq
 * into the scheduler trampoline. user_rsp/user_ss carry meaningful values only
 * when (cs & 3) == 3; a ring-0 interrupt leaves them as whatever the stub
 * captured, which is why the preempt path declines kernel-mode frames. */
typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t user_rsp;
    uint64_t user_ss;
} irq_frame_t;

_Static_assert(offsetof(irq_frame_t, r15) == 0, "irq_frame_t r15 offset mismatch");
_Static_assert(offsetof(irq_frame_t, rax) == 112, "irq_frame_t rax offset mismatch");
_Static_assert(offsetof(irq_frame_t, rip) == 120, "irq_frame_t rip offset mismatch");
_Static_assert(offsetof(irq_frame_t, user_rsp) == 144, "irq_frame_t user_rsp offset mismatch");

/* Every architecture's process_context_t carries a `root_table` field (the
 * physical root of the context's address space, 0 = not yet resolved) that
 * neutral code reads and writes directly. All other fields are reached through
 * the accessors below. */

/* Zero every field, root_table included. */
static inline void arch_context_clear(process_context_t* ctx) {
    __builtin_memset(ctx, 0, sizeof(*ctx));
}

/* Prepare a context that starts executing kernel code at `pc` on the kernel
 * stack `kernel_sp`, with interrupts enabled. */
static inline void arch_context_init_kernel_entry(process_context_t* ctx, uint64_t pc,
                                                  uint64_t kernel_sp) {
    ctx->rsp = kernel_sp;
    ctx->user_rsp = kernel_sp;
    ctx->rip = pc;
    ctx->rflags = X86_RFLAGS_IF;
    ctx->cs = X86_SEL_KERNEL_CS;
    ctx->ss = X86_SEL_KERNEL_DS;
}

/* Point a context at user code: it resumes in user mode at `pc` with user
 * stack `user_sp` and interrupts enabled. The kernel stack is set separately
 * (arch_context_set_kernel_sp). */
static inline void arch_context_set_user_entry(process_context_t* ctx, uint64_t pc,
                                               uint64_t user_sp) {
    ctx->rip = pc;
    ctx->cs = X86_SEL_USER_CS;
    ctx->ss = X86_SEL_USER_DS;
    ctx->user_rsp = user_sp;
    ctx->rflags = X86_RFLAGS_IF;
}

static inline void arch_context_set_kernel_sp(process_context_t* ctx, uint64_t sp) {
    ctx->rsp = sp;
}

static inline void arch_context_set_pc(process_context_t* ctx, uint64_t pc) {
    ctx->rip = pc;
}

/* The value the resumed code finds as the return value of the call it is
 * returning from. */
static inline void arch_context_set_return(process_context_t* ctx, uint64_t value) {
    ctx->rax = value;
}

static inline uint64_t arch_context_pc(const process_context_t* ctx) {
    return ctx->rip;
}

/* Kernel stack pointer of the context (for a user context: the kernel stack it
 * enters on, not its user stack). */
static inline uint64_t arch_context_kernel_sp(const process_context_t* ctx) {
    return ctx->rsp;
}

/* Frame pointer of the saved context (rbp): the head of its frame-record
 * chain, for backtraces. Meaningful for a context saved by a context switch. */
static inline uint64_t arch_context_fp(const process_context_t* ctx) {
    return ctx->rbp;
}

/* Saved processor flags word (RFLAGS). Diagnostic only. */
static inline uint64_t arch_context_flags(const process_context_t* ctx) {
    return ctx->rflags;
}

/* Raw privilege/mode word of the context (CS). Diagnostic only; decisions use
 * arch_context_is_user. */
static inline uint64_t arch_context_mode(const process_context_t* ctx) {
    return ctx->cs;
}

/* Non-zero when the context resumes in user mode. */
static inline int arch_context_is_user(const process_context_t* ctx) {
    return (ctx->cs & 0x3u) == 0x3u;
}

/* Record the calling code's callee-saved registers, flags and stack pointer in
 * `ctx`, so that resuming `ctx` behaves like a return from the CALL that
 * immediately follows this snapshot in the caller: the saved stack pointer is
 * the current one minus the return address that call will push. The resume pc
 * and return value are set by the caller (arch_context_set_pc /
 * arch_context_set_return). Always inlined, so the registers captured are the
 * caller's own at the snapshot point. */
static inline __attribute__((always_inline)) void
arch_context_snapshot_callee_saved(process_context_t* ctx) {
    uintptr_t sp;
    __asm__ volatile("mov %%rsp, %[rsp]\n"
                     "mov %%r15, %[r15]\n"
                     "mov %%r14, %[r14]\n"
                     "mov %%r13, %[r13]\n"
                     "mov %%r12, %[r12]\n"
                     "mov %%rbp, %[rbp]\n"
                     "mov %%rbx, %[rbx]\n"
                     "pushfq; pop %[rf]"
                     : [rsp] "=r"(sp),
                       [r15] "=m"(ctx->r15),
                       [r14] "=m"(ctx->r14),
                       [r13] "=m"(ctx->r13),
                       [r12] "=m"(ctx->r12),
                       [rbp] "=m"(ctx->rbp),
                       [rbx] "=m"(ctx->rbx),
                       [rf] "=m"(ctx->rflags)
                     :
                     : "memory");
    ctx->rsp = sp - 8u;
}

/* arch_call_on_stack_resumable (contract in arch/arch_context.h). x86_64 records
 * `resume` with arch_context_snapshot_callee_saved, whose saved rsp is the slot
 * the call below pushes its return address into; the context switch resumes
 * with `ret` from that slot. The call must therefore follow the snapshot
 * directly, in the same frame, which always-inlining both guarantees. */
static inline __attribute__((always_inline)) uint64_t
arch_call_on_stack_resumable(process_context_t* resume, uintptr_t stack_top, arch_stack_fn_t fn,
                             uint64_t a0, uint64_t a1, uint64_t a2) {
    arch_context_snapshot_callee_saved(resume);
    return arch_call_on_stack(stack_top, fn, a0, a1, a2);
}

/* How an interrupt frame was entered. */
typedef enum {
    ARCH_IRQ_FRAME_INVALID = 0, /* malformed: neither a kernel nor a user frame */
    ARCH_IRQ_FRAME_KERNEL,
    ARCH_IRQ_FRAME_USER,
} arch_irq_frame_origin_t;

/* Classify an interrupt frame. A kernel frame must carry the kernel code
 * selector; a user frame must carry a user SS and a non-zero user stack
 * pointer; any frame with a zero rip, or a CPL other than 0 or 3, is invalid. */
static inline arch_irq_frame_origin_t arch_irq_frame_origin(const irq_frame_t* frame) {
    uint64_t cpl = frame->cs & 0x3u;
    if (frame->rip == 0 || (cpl != 0x0u && cpl != 0x3u)) {
        return ARCH_IRQ_FRAME_INVALID;
    }
    if (cpl == 0x0u) {
        return frame->cs == X86_SEL_KERNEL_CS ? ARCH_IRQ_FRAME_KERNEL : ARCH_IRQ_FRAME_INVALID;
    }
    if ((frame->user_ss & 0x3u) != 0x3u || frame->user_rsp == 0) {
        return ARCH_IRQ_FRAME_INVALID;
    }
    return ARCH_IRQ_FRAME_USER;
}

/* Print the frame's privilege-return words (cs, rip, user_ss, user_rsp) as
 * `[watchdog]` lines, for the invalid-frame report. Implemented in
 * cpu_x86_64.c. */
void arch_irq_frame_log(const irq_frame_t* frame);

/* Copy the interrupted register state from `frame` into `ctx`. The context's
 * kernel stack pointer and root_table are left as they were. */
static inline void arch_context_save_irq_frame(process_context_t* ctx, const irq_frame_t* frame) {
    ctx->rax = frame->rax;
    ctx->rbx = frame->rbx;
    ctx->rcx = frame->rcx;
    ctx->rdx = frame->rdx;
    ctx->rbp = frame->rbp;
    ctx->rsi = frame->rsi;
    ctx->rdi = frame->rdi;
    ctx->r8 = frame->r8;
    ctx->r9 = frame->r9;
    ctx->r10 = frame->r10;
    ctx->r11 = frame->r11;
    ctx->r12 = frame->r12;
    ctx->r13 = frame->r13;
    ctx->r14 = frame->r14;
    ctx->r15 = frame->r15;
    ctx->cs = frame->cs;
    ctx->user_rsp = frame->user_rsp;
    ctx->ss = frame->user_ss;
    ctx->rip = frame->rip;
    ctx->rflags = frame->rflags;
}

/* Rewrite `frame` so the interrupt returns into kernel mode at `pc` on stack
 * `sp`. A frame entered from ring 3 carries a five-word privilege-return frame
 * (rip, cs, rflags, rsp, ss); all of it is rewritten, because iretq would
 * otherwise restore the stale user SS:RSP and fault. */
static inline void arch_irq_frame_redirect_kernel(irq_frame_t* frame, uint64_t pc, uint64_t sp) {
    frame->cs = X86_SEL_KERNEL_CS;
    frame->rip = pc;
    frame->user_ss = X86_SEL_KERNEL_DS;
    frame->user_rsp = sp;
}

/* int 0x80 syscall register convention (the slots syscall.h names):
 *   number  RAX
 *   arg0..5 RDI, RSI, RDX, RCX, R8, R9
 *   result  RAX (the handler's return value; the stub stores it over the saved
 *           RAX before restoring the frame)
 *   result2 RDX, written into the frame and restored with it
 * RDX is both arg2 and result2: a handler that sets result2 reads arg2 first. */
/* The register frame isr_syscall_128 builds on the kernel stack: the GPRs it
 * pushed, then the rip/cs/rflags the CPU pushed. Layout is fixed by that stub.
 * The handler MUTATES this frame -- it writes rdx for the calls with a
 * secondary return value -- and the stub restores from it, so a caller must not
 * assume the frame it passed is unchanged. On entry from ring 3 the CPU-pushed
 * user rsp and ss follow the struct directly (see arch_syscall_user_sp). */
typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
} syscall_frame_t;

static inline uint64_t arch_syscall_number(const syscall_frame_t* frame) {
    return frame->rax;
}

/* Syscall argument `index` (0..5). An out-of-range index reads as 0. */
static inline uint64_t arch_syscall_arg(const syscall_frame_t* frame, unsigned index) {
    switch (index) {
    case 0:
        return frame->rdi;
    case 1:
        return frame->rsi;
    case 2:
        return frame->rdx;
    case 3:
        return frame->rcx;
    case 4:
        return frame->r8;
    case 5:
        return frame->r9;
    default:
        return 0;
    }
}

/* Primary result slot in the frame. The dispatcher's return value is what the
 * entry stub finally loads, so writing it here matters only on paths that
 * return it from the frame. */
static inline void arch_syscall_set_result(syscall_frame_t* frame, uint64_t value) {
    frame->rax = value;
}

static inline uint64_t arch_syscall_result(const syscall_frame_t* frame) {
    return frame->rax;
}

/* Secondary result slot; aliases arg2 (see the convention above). */
static inline void arch_syscall_set_result2(syscall_frame_t* frame, uint64_t value) {
    frame->rdx = value;
}

static inline uint64_t arch_syscall_result2(const syscall_frame_t* frame) {
    return frame->rdx;
}

/* Non-zero when the syscall was entered from user mode. */
static inline int arch_syscall_from_user(const syscall_frame_t* frame) {
    return (frame->cs & 0x3u) == 0x3u;
}

/* The caller's user stack pointer: the rsp the CPU pushed directly after the
 * frame on the ring-3 -> ring-0 transition. Valid only when
 * arch_syscall_from_user(frame). */
static inline uint64_t arch_syscall_user_sp(const syscall_frame_t* frame) {
    return *(const uint64_t*)(const void*)(frame + 1);
}

/* Copy a user-mode syscall frame into `ctx`, so a syscall that blocks resumes
 * at the instruction after the trap. Copies the GPRs, cs, rip and rflags; the
 * context's stacks, ss and root_table are left as they were. */
static inline void arch_context_save_syscall_frame(process_context_t* ctx,
                                                   const syscall_frame_t* frame) {
    ctx->rax = frame->rax;
    ctx->rbx = frame->rbx;
    ctx->rcx = frame->rcx;
    ctx->rdx = frame->rdx;
    ctx->rbp = frame->rbp;
    ctx->rsi = frame->rsi;
    ctx->rdi = frame->rdi;
    ctx->r8 = frame->r8;
    ctx->r9 = frame->r9;
    ctx->r10 = frame->r10;
    ctx->r11 = frame->r11;
    ctx->r12 = frame->r12;
    ctx->r13 = frame->r13;
    ctx->r14 = frame->r14;
    ctx->r15 = frame->r15;
    ctx->cs = frame->cs;
    ctx->rip = frame->rip;
    ctx->rflags = frame->rflags;
}
