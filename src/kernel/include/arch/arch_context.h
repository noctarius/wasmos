/* arch_context.h - selects the target architecture's execution-context
 * primitives. Each architecture provides:
 *
 *   uint64_t arch_call_on_stack(stack_top, fn, a0, a1, a2)
 *       Switch to the stack whose top is stack_top (16-byte aligned), call
 *       fn(a0, a1, a2) as an ordinary C call, switch back and return fn's
 *       integer result widened to 64 bits. fn is cast to arch_stack_fn_t and
 *       may take up to three integer or pointer arguments; unused ones are
 *       ignored. fn must return on the
 *       new stack (it must not longjmp or context-switch away for good).
 *   uintptr_t arch_read_sp(void)   the caller's current stack pointer
 *   uintptr_t arch_read_pc(void)   an address inside the caller's code
 *
 * Register state:
 *   process_context_t   saved state of a thread; resumed by the arch context
 *                       switch. Has a `root_table` field neutral code uses
 *                       directly; everything else goes through accessors:
 *                       arch_context_clear, _init_kernel_entry,
 *                       _set_user_entry, _set_kernel_sp, _set_pc, _set_return,
 *                       _pc, _kernel_sp, _flags, _mode, _is_user,
 *                       _snapshot_callee_saved, _save_irq_frame.
 *   irq_frame_t         register frame an interrupt stub builds; classified by
 *                       arch_irq_frame_origin, logged by arch_irq_frame_log,
 *                       redirected by arch_irq_frame_redirect_kernel.
 *   syscall_frame_t     register frame the syscall entry builds; read through
 *                       arch_syscall_number, _arg, _result, _result2,
 *                       _from_user, _user_sp; written through
 *                       arch_syscall_set_result, _set_result2; copied into a
 *                       thread by arch_context_save_syscall_frame. The slot
 *                       names are the ones syscall.h's contract uses.
 *
 * Accessors are static inline unless documented otherwise; their contracts are
 * documented at the x86_64 implementation.
 *
 * This header is the single selection point; x86_64 is the only target. */
#pragma once

#include "arch/x86_64/arch_context.h"
