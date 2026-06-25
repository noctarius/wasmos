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
 *       movabs rcx, WARP_R3_STACK_BASE + 64
 *       mov [rcx], rsp
 *       movabs rax, WARP_R3_STACK_BASE + 56
 *       mov rax, [rax]
 *       jmp rax
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
#define KBASE     0xFFFFFFFF80000000ULL

/* Kernel alias of a physical page. */
static inline uint8_t *kptr(uint64_t phys)
{
    return (uint8_t *)(uintptr_t)(phys | KBASE);
}

/* Map one 4 KiB page at user VA in the given root, with specified flags. */
static int map_user_page(uint64_t root, uint64_t user_va, uint64_t phys, uint64_t flags)
{
    return paging_map_4k_in_root(root, user_va, phys, flags);
}

int
warp_r3_setup(uint64_t *out_user_root, uint64_t *out_stack_phys)
{
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

    uint8_t *hc = kptr(hc_phys);
    /* Zero the page first. */
    for (uint32_t i = 0; i < PAGE_SIZE; i++) hc[i] = 0;

    /* Write 8-byte stubs. Stub for HC id N: */
    for (uint32_t n = 0; n < WARP_HC_MAX; n++) {
        uint8_t *s = hc + n * 8;
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

    uint8_t *rp = kptr(ret_phys);
    for (uint32_t i = 0; i < PAGE_SIZE; i++) rp[i] = 0;

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
    uint8_t *mh = rp + 0x10;
    mh[0] = 0xb8;
    mh[1] = (uint8_t)(WASMOS_SYSCALL_WARP_MEMORY_HELPER & 0xFF);
    mh[2] = (uint8_t)((WASMOS_SYSCALL_WARP_MEMORY_HELPER >> 8) & 0xFF);
    mh[3] = 0x00;
    mh[4] = 0x00;
    mh[5] = 0xcd;
    mh[6] = 0x80;
    mh[7] = 0xc3;

    /* Entry debug trampoline:
     *   movabs rcx, WARP_R3_STACK_BASE + 64   ; capture RSP here
     *   mov [rcx], rsp
     *   movabs rax, WARP_R3_STACK_BASE + 56   ; load real target RIP
     *   mov rax, [rax]
     *   jmp rax
     */
    {
        uint8_t *et = rp + 0x20;
        uint64_t capture_va = WARP_R3_STACK_BASE + 64ULL;
        uint64_t target_va  = WARP_R3_STACK_BASE + 56ULL;

        et[0] = 0x48; et[1] = 0xb9;
        *(uint64_t *)(void *)&et[2] = capture_va;
        et[10] = 0x48; et[11] = 0x89; et[12] = 0x21;
        et[13] = 0x48; et[14] = 0xb8;
        *(uint64_t *)(void *)&et[15] = target_va;
        et[23] = 0x48; et[24] = 0x8b; et[25] = 0x00;
        et[26] = 0xff; et[27] = 0xe0;
    }

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
    *out_user_root  = root;
    *out_stack_phys = stack_phys;
    return 0;
}

void
warp_r3_teardown(uint64_t user_root, uint64_t stack_phys)
{
    if (user_root) {
        paging_destroy_address_space(user_root);
    }
    if (stack_phys) {
        pfa_free_pages(stack_phys, WARP_R3_STACK_PAGES);
    }
}
