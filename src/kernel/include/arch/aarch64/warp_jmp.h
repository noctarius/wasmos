/* warp_jmp.h - aarch64 jump buffer. clang implements __builtin_setjmp only on
 * some targets and aarch64 is not one, so the pair is assembly
 * (arch/aarch64/warp_jmp_aarch64.S). The buffer holds the AAPCS64
 * callee-saved state: sp, x19..x28, x29 (frame pointer), x30 (the resume
 * address) and d8..d15. Included only through arch/warp_jmp.h. */
#pragma once

#define WARP_JMP_BUF_WORDS 21

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((returns_twice)) int warp_setjmp_aarch64(void** buf);
__attribute__((noreturn)) void warp_longjmp_aarch64(void** buf);

#ifdef __cplusplus
}
#endif

#define warp_setjmp(buf) warp_setjmp_aarch64(buf)
#define warp_longjmp(buf) warp_longjmp_aarch64(buf)
