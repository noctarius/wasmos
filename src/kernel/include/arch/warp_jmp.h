/* warp_jmp.h - the non-local jump WARP's checkpoints use: the C++ exception
 * checkpoint (include/warp_exception.h) and the ring-3 return landing pad
 * (thread_t::warp_r3_jbuf). Each architecture header defines:
 *
 *   WARP_JMP_BUF_WORDS    pointer-sized words of a jump buffer
 *   warp_setjmp(buf)      record the caller's resume point in `buf`; evaluates
 *                         to 0 when recording, and to 1 when warp_longjmp(buf)
 *                         resumes there
 *   warp_longjmp(buf)     resume at the point `buf` recorded; never returns
 *
 * The resume point's frame must still be live when warp_longjmp runs: a
 * jump buffer is valid only until the function that called warp_setjmp
 * returns. Callee-saved registers are restored; locals of that function that
 * changed after warp_setjmp and are not volatile hold unspecified values.
 * This header is the single selection point. */
#pragma once

#if defined(WASMOS_ARCH_AARCH64)
#include "arch/aarch64/warp_jmp.h"
#else
#include "arch/x86_64/warp_jmp.h"
#endif

typedef void* warp_jmp_buf_t[WARP_JMP_BUF_WORDS];
