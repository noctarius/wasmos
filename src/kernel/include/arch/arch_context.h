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
 * This header is the single selection point; x86_64 is the only target. */
#pragma once

#include "arch/x86_64/arch_context.h"
