/* test_warp_jmp.c - warp_setjmp/warp_longjmp (include/arch/warp_jmp.h), the
 * non-local jump WARP's exception checkpoint and ring-3 return pad use.
 *
 * Built for the host's architecture: on an arm64 host it links the kernel's
 * assembly pair (src/kernel/arch/aarch64/warp_jmp_aarch64.S), on x86_64 the
 * clang builtins the x86_64 header maps to. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "arch/warp_jmp.h"
#include "test_shuffle.h"

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                          \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static warp_jmp_buf_t g_buf;
static volatile int g_depth_reached;

static __attribute__((noinline)) void jump_from(int depth) {
    volatile char pad[256];
    pad[0] = (char)depth;
    if (depth == 0) {
        g_depth_reached = 1;
        warp_longjmp(g_buf);
    }
    jump_from(depth - 1);
    pad[1] = pad[0];
}

/* Clobbers every callee-saved register the jump buffer must restore, as a
 * deep WARP frame would, then jumps. */
static __attribute__((noinline)) void clobber_and_jump(void) {
#if defined(__aarch64__)
    __asm__ volatile("mov x19, #0x1919\n\tmov x20, #0x2020\n\tmov x21, #0x2121\n\t"
                     "mov x22, #0x2222\n\tmov x23, #0x2323\n\tmov x24, #0x2424\n\t"
                     "mov x25, #0x2525\n\tmov x26, #0x2626\n\tmov x27, #0x2727\n\t"
                     "mov x28, #0x2828\n\tfmov d8, xzr\n\tfmov d9, xzr\n\tfmov d10, xzr\n\t"
                     "fmov d11, xzr\n\tfmov d12, xzr\n\tfmov d13, xzr\n\tfmov d14, xzr\n\t"
                     "fmov d15, xzr"
                     :
                     :
                     : "x19",
                       "x20",
                       "x21",
                       "x22",
                       "x23",
                       "x24",
                       "x25",
                       "x26",
                       "x27",
                       "x28",
                       "d8",
                       "d9",
                       "d10",
                       "d11",
                       "d12",
                       "d13",
                       "d14",
                       "d15");
#elif defined(__x86_64__)
    __asm__ volatile("movq $0x1313, %%r13\n\tmovq $0x1414, %%r14\n\tmovq $0x1515, %%r15\n\t"
                     "movq $0xbbbb, %%rbx"
                     :
                     :
                     : "rbx", "r13", "r14", "r15");
#endif
    warp_longjmp(g_buf);
}

/* A jump from a frame several calls deep resumes at the recording point with
 * 1, once. */
static void test_a_jump_resumes_at_the_recording_point(void) {
    volatile int resumed = 0;
    g_depth_reached = 0;
    if (warp_setjmp(g_buf) == 0) {
        jump_from(8);
        CHECK(0, "warp_longjmp returned");
    } else {
        resumed++;
    }
    CHECK(g_depth_reached == 1, "the jump never ran");
    CHECK(resumed == 1, "resumed more or less than once");
}

/* Records, lets a deeper frame clobber the callee-saved registers and jump
 * back, then returns normally. */
static __attribute__((noinline)) void record_and_jump(void) {
    if (warp_setjmp(g_buf) == 0) {
        clobber_and_jump();
    }
}

static volatile long g_ints[10] = {3, 15, 21, 33, 39, 51, 57, 69, 87, 93};
static volatile double g_fps[8] = {1.25, 2.5, 3.75, 6.25, 8.75, 11.25, 13.75, 16.25};

/* The recording function's caller keeps values in callee-saved registers across
 * the call; after the jump the recording function returns into it, so those
 * registers must hold what they held when warp_setjmp ran. Every value is
 * compared against a volatile re-read after the call, so the compiler has to
 * keep all eighteen live across it: ten in x19..x28, eight in d8..d15. */
static __attribute__((noinline)) int caller_values_survive(void) {
    long a = g_ints[0], b = g_ints[1], c = g_ints[2], d = g_ints[3], e = g_ints[4], f = g_ints[5],
         g = g_ints[6], h = g_ints[7], i = g_ints[8], j = g_ints[9];
    double p = g_fps[0], q = g_fps[1], r = g_fps[2], t = g_fps[3], u = g_fps[4], v = g_fps[5],
           w = g_fps[6], z = g_fps[7];
    record_and_jump();
    return a == g_ints[0] && b == g_ints[1] && c == g_ints[2] && d == g_ints[3] && e == g_ints[4] &&
           f == g_ints[5] && g == g_ints[6] && h == g_ints[7] && i == g_ints[8] && j == g_ints[9] &&
           p == g_fps[0] && q == g_fps[1] && r == g_fps[2] && t == g_fps[3] && u == g_fps[4] &&
           v == g_fps[5] && w == g_fps[6] && z == g_fps[7];
}

static void test_callee_saved_state_survives_the_jump(void) {
    CHECK(caller_values_survive(), "a callee-saved value changed across the jump");
}

/* Jumping back to the same point many times does not grow or shrink the stack. */
static void test_repeated_jumps_keep_the_stack_pointer(void) {
    volatile uintptr_t first = 0;
    volatile int rounds = 0;
    char marker;
    if (warp_setjmp(g_buf) != 0) {
        rounds++;
    }
    if (first == 0) {
        first = (uintptr_t)&marker;
    }
    CHECK((uintptr_t)&marker == first, "the stack moved across a jump");
    if (rounds < 1000) {
        jump_from(2);
    }
    CHECK(rounds == 1000, "wrong number of resumes");
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_jump_resumes_at_the_recording_point),
        WASMOS_TEST_CASE(test_callee_saved_state_survives_the_jump),
        WASMOS_TEST_CASE(test_repeated_jumps_keep_the_stack_pointer),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_warp_jmp: ok\n");
    return 0;
}
