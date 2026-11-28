/* warp_ring3_x86_64.c - x86_64 machine code for WARP's ring-3 trampolines and
 * the iretq into a JIT wrapper. The contract is in arch/warp_ring3_arch.h; the
 * model is docs/architecture/31-warp-ring3-implementation.md. */

#include <stdint.h>

#include "arch/warp_ring3_arch.h"
#include "syscall.h"
#include "warp_ring3.h"

/* The user code and stack selectors process.c installs for ring 3. */
#define WARP_R3_USER_CS 0x1Bu
#define WARP_R3_USER_SS 0x23u
/* RFLAGS for the guest: IF set, so a timer can preempt it, DF clear as the
 * SysV ABI requires on entry; bit 1 is reserved-one. */
#define WARP_R3_RFLAGS 0x202u

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_u64(uint8_t* p, uint64_t v) {
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}

void arch_warp_r3_write_hc_stub(uint8_t* dst, uint32_t syscall_nr) {
    dst[0] = 0xb8; /* mov eax, imm32 */
    put_u32(dst + 1, syscall_nr);
    dst[5] = 0xcd; /* int 0x80 */
    dst[6] = 0x80;
    dst[7] = 0xc3; /* ret */
}

void arch_warp_r3_write_return_page(uint8_t* page) {
    /* Return stub. The wrapper's result is in rax; the syscall takes it in rdi.
     *   push rax ; mov eax, WARP_RETURN ; pop rdi ; int 0x80 ; ud2 */
    uint8_t* rp = page;
    rp[0] = 0x50;
    rp[1] = 0xb8;
    put_u32(rp + 2, WASMOS_SYSCALL_WARP_RETURN);
    rp[6] = 0x5f;
    rp[7] = 0xcd;
    rp[8] = 0x80;
    rp[9] = 0x0f;
    rp[10] = 0x0b;

    /* Memory-helper stub: mov eax, MEMORY_HELPER ; int 0x80 ; ret */
    uint8_t* mh = page + (WARP_R3_MEMHELPER_TRAMPOLINE - WARP_R3_RET_TRAMPOLINE);
    mh[0] = 0xb8;
    put_u32(mh + 1, WASMOS_SYSCALL_WARP_MEMORY_HELPER);
    mh[5] = 0xcd;
    mh[6] = 0x80;
    mh[7] = 0xc3;

    /* Entry stub. rcx carries the wrapper's fourth argument, so only r10 and
     * r11 are used:
     *   movabs r10, &ENTRY_SP ; mov [r10], rsp
     *   movabs r11, &ENTRY_TARGET ; mov r11, [r11] ; jmp r11 */
    uint8_t* et = page + (WARP_R3_ENTRY_TRAMPOLINE - WARP_R3_RET_TRAMPOLINE);
    et[0] = 0x49;
    et[1] = 0xba;
    put_u64(et + 2, WARP_R3_STACK_BASE + WARP_R3_FRAME_ENTRY_SP);
    et[10] = 0x49;
    et[11] = 0x89;
    et[12] = 0x22;
    et[13] = 0x49;
    et[14] = 0xbb;
    put_u64(et + 15, WARP_R3_STACK_BASE + WARP_R3_FRAME_ENTRY_TARGET);
    et[23] = 0x4d;
    et[24] = 0x8b;
    et[25] = 0x1b;
    et[26] = 0x41;
    et[27] = 0xff;
    et[28] = 0xe3;
}

/* The return address is the top stack slot, as a call would leave it, so the
 * wrapper's ret pops call->return_va. The four argument registers are loaded
 * from memory operands so the compiler cannot allocate the iretq frame values
 * to them. Once the iretq frame is pushed, every register the guest does not
 * receive -- rax, rbx, rbp, r8..r15 and xmm0..xmm15 -- is cleared, so no kernel
 * value (a stack or heap address, a callee-saved value) reaches ring 3. */
void arch_warp_r3_enter(const arch_warp_r3_call_t* call) {
    *(uint64_t*)(void*)(call->stack_top_kernel - 8) = call->return_va;
    uint64_t rsp = call->stack_top_va - 8u;
    uint64_t rip = call->entry_va;
    uint64_t rflags = WARP_R3_RFLAGS;
    uint64_t cs = WARP_R3_USER_CS;
    uint64_t ss = WARP_R3_USER_SS;
    uint64_t a0 = call->args[0];
    uint64_t a1 = call->args[1];
    uint64_t a2 = call->args[2];
    uint64_t a3 = call->args[3];
    __asm__ volatile("movq %[a0], %%rdi\n\t"
                     "movq %[a1], %%rsi\n\t"
                     "movq %[a2], %%rdx\n\t"
                     "movq %[a3], %%rcx\n\t"
                     "pushq %[ss]\n\t"
                     "pushq %[rsp_v]\n\t"
                     "pushq %[rf]\n\t"
                     "pushq %[cs]\n\t"
                     "pushq %[rip_v]\n\t"
                     "xorl %%eax, %%eax\n\t"
                     "xorl %%ebx, %%ebx\n\t"
                     "xorl %%ebp, %%ebp\n\t"
                     "xorl %%r8d, %%r8d\n\t"
                     "xorl %%r9d, %%r9d\n\t"
                     "xorl %%r10d, %%r10d\n\t"
                     "xorl %%r11d, %%r11d\n\t"
                     "xorl %%r12d, %%r12d\n\t"
                     "xorl %%r13d, %%r13d\n\t"
                     "xorl %%r14d, %%r14d\n\t"
                     "xorl %%r15d, %%r15d\n\t"
                     "pxor %%xmm0, %%xmm0\n\t"
                     "pxor %%xmm1, %%xmm1\n\t"
                     "pxor %%xmm2, %%xmm2\n\t"
                     "pxor %%xmm3, %%xmm3\n\t"
                     "pxor %%xmm4, %%xmm4\n\t"
                     "pxor %%xmm5, %%xmm5\n\t"
                     "pxor %%xmm6, %%xmm6\n\t"
                     "pxor %%xmm7, %%xmm7\n\t"
                     "pxor %%xmm8, %%xmm8\n\t"
                     "pxor %%xmm9, %%xmm9\n\t"
                     "pxor %%xmm10, %%xmm10\n\t"
                     "pxor %%xmm11, %%xmm11\n\t"
                     "pxor %%xmm12, %%xmm12\n\t"
                     "pxor %%xmm13, %%xmm13\n\t"
                     "pxor %%xmm14, %%xmm14\n\t"
                     "pxor %%xmm15, %%xmm15\n\t"
                     "iretq"
                     :
                     : [a0] "m"(a0),
                       [a1] "m"(a1),
                       [a2] "m"(a2),
                       [a3] "m"(a3),
                       [ss] "r"(ss),
                       [rsp_v] "r"(rsp),
                       [rf] "r"(rflags),
                       [cs] "r"(cs),
                       [rip_v] "r"(rip)
                     : "memory", "rdi", "rsi", "rdx", "rcx");
    __builtin_unreachable();
}
