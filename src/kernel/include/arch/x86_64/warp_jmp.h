/* warp_jmp.h - x86_64 jump buffer: clang's __builtin_setjmp/__builtin_longjmp,
 * which keep five words (frame pointer, resume address, stack pointer and two
 * reserved) and expand inline in the caller, so warp_setjmp must be a macro.
 * Included only through arch/warp_jmp.h. */
#pragma once

#define WARP_JMP_BUF_WORDS 5
#define warp_setjmp(buf) __builtin_setjmp(buf)
#define warp_longjmp(buf) __builtin_longjmp((buf), 1)
