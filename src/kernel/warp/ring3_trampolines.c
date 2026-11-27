/* warp/ring3_trampolines.c - Ring-3 trampoline pages and address-space setup.
 *
 * Creates two 4 KiB user-mode pages in the WARP ring-3 address space:
 *
 *   HC trampoline page (at WARP_R3_HC_TRAMPOLINE): WARP_HC_MAX stubs of
 *     WARP_R3_HC_STUB_BYTES each. When ring-3 JIT code calls stub N via
 *     DYNAMIC_LINK, the stub raises syscall WARP_HC_SYSCALL_BASE + N and the
 *     kernel's syscall dispatch calls warp_ring3_dispatch(N).
 *
 *   Return trampoline page (at WARP_R3_RET_TRAMPOLINE): the return stub the
 *     JIT wrapper returns into (WASMOS_SYSCALL_WARP_RETURN with its result),
 *     the memory-helper stub at WARP_R3_MEMHELPER_TRAMPOLINE, and the entry
 *     stub at WARP_R3_ENTRY_TRAMPOLINE.
 *
 * The stubs' machine code is the architecture's (arch/warp_ring3_arch.h).
 * warp_r3_setup() also allocates the ring-3 user stack and creates the
 * per-module user address space (user root) via paging_create_address_space.
 */

#include <stdint.h>
#include <stddef.h>

#include "physmem.h"
#include "paging.h"
#include "memory.h"
#include "klog.h"
#include "warp_ring3.h"
#include "arch/vm_layout.h"
#include "arch/warp_ring3_arch.h"

#define PAGE_SIZE 4096ULL

/* Kernel alias of a physical page. */
static inline uint8_t* kptr(uint64_t phys) {
    return ptr_cast(uint8_t, (phys | KERNEL_HIGHER_HALF_BASE));
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

    for (uint32_t n = 0; n < WARP_HC_MAX; n++) {
        arch_warp_r3_write_hc_stub(hc + n * WARP_R3_HC_STUB_BYTES, WARP_HC_SYSCALL_BASE + n);
    }
    paging_sync_instruction_cache(hc, PAGE_SIZE);

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

    arch_warp_r3_write_return_page(rp);
    paging_sync_instruction_cache(rp, PAGE_SIZE);
    /* TODO(smp-tlb): replace the entry stub's stack-pointer capture with a cleaner
     * per-call capture path once WARP ring-3 bring-up is stable again. */

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

/* Release what warp_r3_setup produced.  paging_destroy_address_space frees page-table
 * frames only, never the leaves mapped through them, so the two trampoline pages are
 * looked up in the root and freed here first, as the stack is below.  Either argument
 * may be 0, in which case that half is skipped, so a partially-set-up driver can be
 * torn down with the same call.  The caller must ensure no CPU still has `user_root`
 * loaded. */
void warp_r3_teardown(uint64_t user_root, uint64_t stack_phys) {
    if (user_root) {
        const uint64_t trampolines[] = {WARP_R3_HC_TRAMPOLINE, WARP_R3_RET_TRAMPOLINE};
        for (uint32_t i = 0; i < sizeof(trampolines) / sizeof(trampolines[0]); ++i) {
            uint64_t phys = paging_virt_to_phys_in_root(user_root, trampolines[i]);
            if (phys) {
                pfa_free_pages(phys & ~(PAGE_SIZE - 1u), 1);
            }
        }
        paging_destroy_address_space(user_root);
    }
    if (stack_phys) {
        pfa_free_pages(stack_phys, WARP_R3_STACK_PAGES);
    }
}
