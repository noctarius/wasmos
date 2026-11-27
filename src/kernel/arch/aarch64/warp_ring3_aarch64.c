/* warp_ring3_aarch64.c - aarch64 machine code for WARP's ring-3 trampolines and
 * the ERET into a JIT wrapper. The contract is in arch/warp_ring3_arch.h; the
 * model is docs/architecture/31-warp-ring3-implementation.md.
 *
 * Instructions are little-endian 32-bit words, written through the kernel alias
 * of the page; ring3_trampolines.c syncs the instruction cache afterwards. */

#include <stddef.h>
#include <stdint.h>

#include "arch/warp_ring3_arch.h"
#include "syscall.h"
#include "warp_ring3.h"

#define A64_SVC0 0xD4000001u /* svc #0 */
#define A64_RET 0xD65F03C0u  /* ret (x30) */
#define A64_BRK0 0xD4200000u /* brk #0 */
#define A64_BR_X16 0xD61F0200u
#define A64_MOV_X17_SP 0x910003F1u    /* add x17, sp, #0 */
#define A64_STR_X17_X16_8 0xF9000611u /* str x17, [x16, #8] */
#define A64_LDR_X16_X16 0xF9400210u   /* ldr x16, [x16] */

static void put_insn(uint8_t* p, uint32_t insn) {
    p[0] = (uint8_t)(insn & 0xFFu);
    p[1] = (uint8_t)((insn >> 8) & 0xFFu);
    p[2] = (uint8_t)((insn >> 16) & 0xFFu);
    p[3] = (uint8_t)((insn >> 24) & 0xFFu);
}

/* movz x8, #imm16 */
static uint32_t a64_movz_x8(uint32_t imm16) {
    return 0xD2800000u | ((imm16 & 0xFFFFu) << 5) | 8u;
}

/* adr x16, #off (off within +-1 MiB) */
static uint32_t a64_adr_x16(int64_t off) {
    uint32_t imm = (uint32_t)off & 0x1FFFFFu;
    return 0x10000000u | ((imm & 3u) << 29) | ((imm >> 2) << 5) | 16u;
}

void arch_warp_r3_write_hc_stub(uint8_t* dst, uint32_t syscall_nr) {
    put_insn(dst, a64_movz_x8(syscall_nr));
    put_insn(dst + 4, A64_SVC0);
    put_insn(dst + 8, A64_RET);
    put_insn(dst + 12, A64_BRK0);
}

void arch_warp_r3_write_return_page(uint8_t* page) {
    /* Return stub. The wrapper's result is already in x0, syscall argument 0. */
    put_insn(page + 0x00, a64_movz_x8(WASMOS_SYSCALL_WARP_RETURN));
    put_insn(page + 0x04, A64_SVC0);
    put_insn(page + 0x08, A64_BRK0);

    /* Memory-helper stub. */
    put_insn(page + 0x10, a64_movz_x8(WASMOS_SYSCALL_WARP_MEMORY_HELPER));
    put_insn(page + 0x14, A64_SVC0);
    put_insn(page + 0x18, A64_RET);

    /* Entry stub, addressing the scratch slots PC-relatively (the ring-3 stack
     * follows this page): x16 = &entryTarget; [x16 + 8] = sp; br [x16]. x16 and
     * x17 are the intra-procedure-call scratch registers, so x0..x3 and x30 reach
     * the wrapper untouched. */
    uint8_t* et = page + (WARP_R3_ENTRY_TRAMPOLINE - WARP_R3_RET_TRAMPOLINE);
    int64_t target_off = (int64_t)(WARP_R3_STACK_BASE + 56u) - (int64_t)WARP_R3_ENTRY_TRAMPOLINE;
    put_insn(et + 0, a64_adr_x16(target_off));
    put_insn(et + 4, A64_MOV_X17_SP);
    put_insn(et + 8, A64_STR_X17_X16_8);
    put_insn(et + 12, A64_LDR_X16_X16);
    put_insn(et + 16, A64_BR_X16);
}

_Static_assert(offsetof(arch_warp_r3_call_t, entry_va) == 0, "entry_va offset");
_Static_assert(offsetof(arch_warp_r3_call_t, return_va) == 8, "return_va offset");
_Static_assert(offsetof(arch_warp_r3_call_t, stack_top_va) == 16, "stack_top_va offset");
_Static_assert(offsetof(arch_warp_r3_call_t, args) == 32, "args offset");

/* SPSR 0 is EL0t with D, A, I and F clear: the guest runs with interrupts
 * enabled. The return address goes in x30, so nothing is written to the ring-3
 * stack. Every load comes from `call` through x9, so no argument register is
 * needed for anything but its argument, and x9/x10 are cleared before the ERET
 * so no kernel address leaks to EL0 through them. */
void arch_warp_r3_enter(const arch_warp_r3_call_t* call) {
    register const arch_warp_r3_call_t* c __asm__("x9") = call;
    __asm__ volatile("msr daifset, #0xf\n\t"
                     "ldr x10, [x9, #16]\n\t"
                     "msr sp_el0, x10\n\t"
                     "ldr x10, [x9, #0]\n\t"
                     "msr elr_el1, x10\n\t"
                     "msr spsr_el1, xzr\n\t"
                     "ldr x30, [x9, #8]\n\t"
                     "ldp x0, x1, [x9, #32]\n\t"
                     "ldp x2, x3, [x9, #48]\n\t"
                     "mov x9, xzr\n\t"
                     "mov x10, xzr\n\t"
                     "eret"
                     :
                     : "r"(c)
                     : "memory");
    __builtin_unreachable();
}
