/* test_warp_r3_hostcall_args.c - where a ring-3 host call's arguments come
 * from (arch_warp_r3_hostcall_arg) and the check that guards the stack ones
 * (arch_warp_r3_hostcall_stack_ok), both in arch/warp_ring3_arch.h.
 *
 * Built twice, once against each architecture's header (-DWASMOS_ARCH_AARCH64
 * selects aarch64): the frame layout and the register/stack split differ, the
 * contract does not.
 *
 * Regression: 2026-11-28-warp-r3-stack-args -- arguments past the registers
 * were read through the trapping user stack pointer with no check, so a guest
 * that trapped with sp aimed at kernel memory, or at the edge of its stack, had
 * the kernel read wherever it pointed: a kernel fault, or kernel memory handed
 * to the host call as an argument. The generated dispatch now refuses the call
 * unless this check passes for the widest host call; the dispatch itself
 * cannot be linked on the host, so the check is what is tested here. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arch/warp_ring3_arch.h"
#include "test_shuffle.h"

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                          \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

/* A syscall frame with the user stack pointer where the architecture keeps it:
 * x86_64 right after the frame (the CPU-pushed rsp), aarch64 in the frame. */
typedef struct {
    syscall_frame_t frame;
    uint64_t user_sp;
    uint64_t user_ss;
} test_frame_t;

static void set_sp(test_frame_t* t, uint64_t sp) {
#if defined(WASMOS_ARCH_AARCH64)
    t->frame.sp = sp;
#else
    t->user_sp = sp;
#endif
}

/* The widest host call today takes nine arguments (ipc_send's eight plus the
 * computed context); the generated dispatch checks against its own maximum. */
#define WIDEST 9u

static int ok_at(uint64_t sp, unsigned nargs) {
    test_frame_t t;
    memset(&t, 0, sizeof(t));
    set_sp(&t, sp);
    return arch_warp_r3_hostcall_stack_ok(&t.frame, nargs);
}

static uint64_t span(unsigned nargs) {
    return WARP_R3_HC_STACK_ARGS_AT + (uint64_t)(nargs - WARP_R3_HC_REG_ARGS) * 8u;
}

static void test_a_stack_pointer_inside_the_ring3_stack_passes(void) {
    CHECK(ok_at(WARP_R3_STACK_BASE + WARP_R3_STACK_SIZE / 2u, WIDEST), "mid-stack sp");
    CHECK(ok_at(WARP_R3_STACK_BASE, WIDEST), "sp at the stack base");
    CHECK(ok_at(WARP_R3_STACK_TOP - span(WIDEST), WIDEST), "the last slot read is the top one");
}

static void test_a_stack_pointer_outside_the_ring3_stack_is_refused(void) {
    CHECK(!ok_at(0xFFFFFFFF80100000ULL, WIDEST), "a kernel address");
    CHECK(!ok_at(0xFFFFFF8040100000ULL, WIDEST), "an aarch64 direct-map address");
    CHECK(!ok_at(WARP_R3_STACK_BASE - 8u, WIDEST), "just below the stack");
    CHECK(!ok_at(WARP_R3_LINMEM_BASE, WIDEST), "guest linear memory");
    CHECK(!ok_at(0, WIDEST), "null");
    CHECK(!ok_at(UINT64_MAX - 4u, WIDEST), "an sp whose slots would wrap");
}

static void test_a_stack_pointer_too_close_to_the_top_is_refused(void) {
    CHECK(!ok_at(WARP_R3_STACK_TOP - span(WIDEST) + 8u, WIDEST), "the last slot past the top");
    CHECK(!ok_at(WARP_R3_STACK_TOP, WIDEST), "sp at the top");
}

/* A call whose arguments all fit in registers reads no stack slot, so its stack
 * pointer is not the kernel's concern. */
static void test_register_only_calls_need_no_stack(void) {
    CHECK(ok_at(0xFFFFFFFF80100000ULL, WARP_R3_HC_REG_ARGS), "register-only call");
}

/* Arguments come from the registers first, then from consecutive stack slots
 * starting WARP_R3_HC_STACK_ARGS_AT past the stack pointer. */
static void test_arguments_come_from_registers_then_the_stack(void) {
    uint64_t stack[8];
    for (unsigned i = 0; i < 8u; ++i) {
        stack[i] = 0x5100u + i;
    }
    test_frame_t t;
    memset(&t, 0, sizeof(t));
    set_sp(&t, (uint64_t)(uintptr_t)stack);
#if defined(WASMOS_ARCH_AARCH64)
    for (unsigned i = 0; i < WARP_R3_HC_REG_ARGS; ++i) {
        t.frame.x[i] = 0xA000u + i;
    }
#else
    t.frame.rdi = 0xA000u;
    t.frame.rsi = 0xA001u;
    t.frame.rdx = 0xA002u;
    t.frame.rcx = 0xA003u;
    t.frame.r8 = 0xA004u;
    t.frame.r9 = 0xA005u;
#endif
    for (unsigned i = 0; i < WARP_R3_HC_REG_ARGS; ++i) {
        CHECK(arch_warp_r3_hostcall_arg(&t.frame, i) == 0xA000u + i, "register argument");
    }
    uint64_t first = (uint64_t)WARP_R3_HC_STACK_ARGS_AT / 8u;
    CHECK(arch_warp_r3_hostcall_arg(&t.frame, WARP_R3_HC_REG_ARGS) == stack[first],
          "the first stack argument");
    CHECK(arch_warp_r3_hostcall_arg(&t.frame, WARP_R3_HC_REG_ARGS + 1u) == stack[first + 1u],
          "the second stack argument");
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_stack_pointer_inside_the_ring3_stack_passes),
        WASMOS_TEST_CASE(test_a_stack_pointer_outside_the_ring3_stack_is_refused),
        WASMOS_TEST_CASE(test_a_stack_pointer_too_close_to_the_top_is_refused),
        WASMOS_TEST_CASE(test_register_only_calls_need_no_stack),
        WASMOS_TEST_CASE(test_arguments_come_from_registers_then_the_stack),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_warp_r3_hostcall_args: ok\n");
    return 0;
}
