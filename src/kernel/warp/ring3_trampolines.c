/* warp/ring3_trampolines.c - Ring-3 trampoline pages and address-space setup.
 *
 * Creates two 4 KiB user-mode pages in the WARP ring-3 address space:
 *
 *   HC trampoline page (at WARP_R3_HC_TRAMPOLINE):
 *     128 × 8-byte stubs. Stub N:
 *       b8 <N+0x100> 00 00   mov eax, WARP_HC_SYSCALL_BASE + N   [5 bytes]
 *       cd 80                 int 0x80                             [2 bytes]
 *       c3                    ret                                  [1 byte]
 *     When ring-3 JIT calls stub N via DYNAMIC_LINK, `int 0x80` fires with
 *     RAX = 0x100 + N. The kernel syscall handler calls warp_ring3_dispatch(N).
 *
 *   Return trampoline page (at WARP_R3_RET_TRAMPOLINE):
 *     When the JIT wrapper for the exported function executes `ret`, the CPU
 *     pops WARP_R3_RET_TRAMPOLINE (pre-pushed on the ring-3 stack) and jumps
 *     here. The trampoline fires WASMOS_SYSCALL_WARP_RETURN (16), passing the
 *     JIT wrapper's RAX as RDI so the kernel can record the exit value.
 *       50                    push rax              [1 byte]
 *       b8 10 00 00 00        mov eax, 16           [5 bytes]
 *       5f                    pop rdi               [1 byte]
 *       cd 80                 int 0x80              [2 bytes]
 *       0f 0b                 ud2                   [2 bytes]
 *
 *     The same RX page also carries a tiny memory-helper stub at
 *     WARP_R3_MEMHELPER_TRAMPOLINE:
 *       b8 11 00 00 00        mov eax, 17           [5 bytes]
 *       cd 80                 int 0x80              [2 bytes]
 *       c3                    ret                   [1 byte]
 *
 *     A debug entry stub also lives in this page at
 *     WARP_R3_ENTRY_TRAMPOLINE:
 *       movabs r10, WARP_R3_STACK_BASE + 64
 *       mov [r10], rsp
 *       movabs r11, WARP_R3_STACK_BASE + 56
 *       mov r11, [r11]
 *       jmp r11
 *
 * warp_r3_setup() also allocates the ring-3 user stack and creates the
 * per-module user address space (user CR3) via paging_create_address_space.
 */

#include <stdint.h>
#include <stddef.h>

#include "physmem.h"
#include "paging.h"
#include "memory.h"
#include "klog.h"
#include "warp_ring3.h"

#define PAGE_SIZE 4096ULL
#define KBASE 0xFFFFFFFF80000000ULL

/* Kernel alias of a physical page. */
static inline uint8_t* kptr(uint64_t phys) {
    return ptr_cast(uint8_t, (phys | KBASE));
}

/* Map one 4 KiB page at user VA in the given root, with specified flags. */
static int map_user_page(uint64_t root, uint64_t user_va, uint64_t phys, uint64_t flags) {
    return paging_map_4k_in_root(root, user_va, phys, flags);
}

/* Build one WARP guest's ring-3 address space: a fresh user root (which inherits the
 * kernel higher half), the host-call trampoline page, the return/memory-helper/entry
 * trampoline page, and the WARP_R3_STACK_PAGES user stack.  On success writes the root
 * and the stack's physical base and returns 0; on any failure everything allocated so
 * far is released and -1 is returned with the outputs untouched.  The two outputs are
 * the whole handle — the caller stores them per driver and passes them back to
 * warp_r3_teardown; no global state is created, so concurrent setups do not interfere.
 * The JIT code and the linear memory are NOT mapped here (see warp_mem_ring3_map_jit /
 * warp_mem_ring3_map_linmem); the root returned is not yet runnable. */
int warp_r3_setup(uint64_t* out_user_root, uint64_t* out_stack_phys) {
    uint64_t root = 0;

    /* Create user address space (inherits kernel higher-half mappings). */
    if (paging_create_address_space(&root) != 0) {
        klog_write("[warp-r3] paging_create_address_space failed\n");
        return -1;
    }

    /* --- HC trampoline page --- */
    uint64_t hc_phys = pfa_alloc_pages_above(1, WARP_JIT_PHYS_MIN);
    if (!hc_phys) {
        klog_write("[warp-r3] failed to alloc HC trampoline page\n");
        paging_destroy_address_space(root);
        return -1;
    }

    uint8_t* hc = kptr(hc_phys);
    /* Zero the page first. */
    for (uint32_t i = 0; i < PAGE_SIZE; i++)
        hc[i] = 0;

    /* Write 8-byte stubs. Stub for HC id N: */
    for (uint32_t n = 0; n < WARP_HC_MAX; n++) {
        uint8_t* s = hc + n * 8;
        uint32_t eax_val = WARP_HC_SYSCALL_BASE + n;
        /* mov eax, imm32 = b8 <4 bytes LE> */
        s[0] = 0xb8;
        s[1] = (uint8_t)(eax_val & 0xFF);
        s[2] = (uint8_t)((eax_val >> 8) & 0xFF);
        s[3] = (uint8_t)((eax_val >> 16) & 0xFF);
        s[4] = (uint8_t)((eax_val >> 24) & 0xFF);
        /* int 0x80 = cd 80 */
        s[5] = 0xcd;
        s[6] = 0x80;
        /* ret = c3 */
        s[7] = 0xc3;
    }

    uint64_t rx_flags = MEM_REGION_FLAG_READ | MEM_REGION_FLAG_EXEC | MEM_REGION_FLAG_USER;
    if (map_user_page(root, WARP_R3_HC_TRAMPOLINE, hc_phys, rx_flags) != 0) {
        klog_write("[warp-r3] failed to map HC trampoline\n");
        pfa_free_pages(hc_phys, 1);
        paging_destroy_address_space(root);
        return -1;
    }

    /* --- Return trampoline page --- */
    uint64_t ret_phys = pfa_alloc_pages_above(1, WARP_JIT_PHYS_MIN);
    if (!ret_phys) {
        klog_write("[warp-r3] failed to alloc return trampoline page\n");
        pfa_free_pages(hc_phys, 1);
        paging_destroy_address_space(root);
        return -1;
    }

    uint8_t* rp = kptr(ret_phys);
    for (uint32_t i = 0; i < PAGE_SIZE; i++)
        rp[i] = 0;

    /* Return trampoline code:
     *   push rax          50
     *   mov eax, 16       b8 10 00 00 00
     *   pop rdi           5f
     *   int 0x80          cd 80
     *   ud2               0f 0b           (safety: should never reach) */
    rp[0] = 0x50;
    rp[1] = 0xb8;
    rp[2] = (uint8_t)(WASMOS_SYSCALL_WARP_RETURN & 0xFF);
    rp[3] = (uint8_t)((WASMOS_SYSCALL_WARP_RETURN >> 8) & 0xFF);
    rp[4] = 0x00;
    rp[5] = 0x00;
    rp[6] = 0x5f;
    rp[7] = 0xcd;
    rp[8] = 0x80;
    rp[9] = 0x0f;
    rp[10] = 0x0b;

    /* Memory helper trampoline:
     *   mov eax, WASMOS_SYSCALL_WARP_MEMORY_HELPER
     *   int 0x80
     *   ret
     */
    uint8_t* mh = rp + 0x10;
    mh[0] = 0xb8;
    mh[1] = (uint8_t)(WASMOS_SYSCALL_WARP_MEMORY_HELPER & 0xFF);
    mh[2] = (uint8_t)((WASMOS_SYSCALL_WARP_MEMORY_HELPER >> 8) & 0xFF);
    mh[3] = 0x00;
    mh[4] = 0x00;
    mh[5] = 0xcd;
    mh[6] = 0x80;
    mh[7] = 0xc3;

    /* Entry debug trampoline:
     *   movabs r10, WARP_R3_STACK_BASE + 64   ; capture RSP here
     *   mov [r10], rsp
     *   movabs r11, WARP_R3_STACK_BASE + 56   ; load real target RIP
     *   mov r11, [r11]
     *   jmp r11
     *
     * Do not clobber RCX here: it carries the wrapper's fourth argument
     * (results pointer) under the SysV ABI. */
    /* TODO(smp-tlb): replace entry-stub diagnostics with a cleaner per-call
     * capture path once WARP ring-3 bring-up is stable again. */

    uint8_t* et = rp + 0x20;
    uint64_t capture_va = WARP_R3_STACK_BASE + 64ULL;
    uint64_t target_va = WARP_R3_STACK_BASE + 56ULL;

    et[0] = 0x49;
    et[1] = 0xba;
    *(uint64_t*)(void*)&et[2] = capture_va;
    et[10] = 0x49;
    et[11] = 0x89;
    et[12] = 0x22;
    et[13] = 0x49;
    et[14] = 0xbb;
    *(uint64_t*)(void*)&et[15] = target_va;
    et[23] = 0x4d;
    et[24] = 0x8b;
    et[25] = 0x1b;
    et[26] = 0x41;
    et[27] = 0xff;
    et[28] = 0xe3;

    if (map_user_page(root, WARP_R3_RET_TRAMPOLINE, ret_phys, rx_flags) != 0) {
        klog_write("[warp-r3] failed to map return trampoline\n");
        pfa_free_pages(hc_phys, 1);
        pfa_free_pages(ret_phys, 1);
        paging_destroy_address_space(root);
        return -1;
    }

    /* --- Ring-3 user stack --- */
    uint64_t stack_phys = pfa_alloc_pages_above(WARP_R3_STACK_PAGES, WARP_JIT_PHYS_MIN);
    if (!stack_phys) {
        klog_write("[warp-r3] failed to alloc ring-3 stack\n");
        pfa_free_pages(hc_phys, 1);
        pfa_free_pages(ret_phys, 1);
        paging_destroy_address_space(root);
        return -1;
    }

    uint64_t rw_flags = MEM_REGION_FLAG_READ | MEM_REGION_FLAG_WRITE | MEM_REGION_FLAG_USER;
    for (uint64_t i = 0; i < WARP_R3_STACK_PAGES; i++) {
        uint64_t va = WARP_R3_STACK_BASE + i * PAGE_SIZE;
        uint64_t ph = stack_phys + i * PAGE_SIZE;
        if (map_user_page(root, va, ph, rw_flags) != 0) {
            klog_write("[warp-r3] failed to map ring-3 stack page\n");
            pfa_free_pages(hc_phys, 1);
            pfa_free_pages(ret_phys, 1);
            pfa_free_pages(stack_phys, WARP_R3_STACK_PAGES);
            paging_destroy_address_space(root);
            return -1;
        }
    }

    /* Return the freshly-created root and stack to the caller, which stores them
     * per-process on the wasm_driver.  No global state is touched, so concurrent
     * setup/teardown on other CPUs cannot clobber this call. */
    *out_user_root = root;
    *out_stack_phys = stack_phys;
    return 0;
}

/* Release what warp_r3_setup produced.  Destroying the address space also releases the
 * two trampoline pages mapped into it.  Either argument may be 0, in which case that
 * half is skipped, so a partially-set-up driver can be torn down with the same call.
 * The caller must ensure no CPU still has `user_root` loaded. */
void warp_r3_teardown(uint64_t user_root, uint64_t stack_phys) {
    if (user_root) {
        paging_destroy_address_space(user_root);
    }
    if (stack_phys) {
        pfa_free_pages(stack_phys, WARP_R3_STACK_PAGES);
    }
}
