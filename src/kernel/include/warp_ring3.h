/* warp_ring3.h - Ring-3 (user-mode) execution model for WARP WASM modules.
 *
 * Places JIT code and linear memory in separate user-space VA ranges to
 * prevent commitVirtualMemory from zeroing JIT pages (the aliasing crash).
 *
 * VA layout (USER_VA_MIN = 0x8000000000):
 *   WARP_R3_JIT_BASE       = 0x008000000000  JIT code,     R-X, up to 512 MiB
 *   WARP_R3_LINMEM_BASE    = 0x00A000000000  linear memory, RW-, up to 2 GiB
 *   WARP_R3_HC_TRAMPOLINE  = 0x00A080000000  HC stub page,  R-X, 1 × 4 KiB
 *   WARP_R3_RET_TRAMPOLINE = 0x00A080001000  ret stub page, R-X, 1 × 4 KiB
 *   WARP_R3_MEMHELPER_TRAMPOLINE = RET page + 0x10, R-X, 1 stub
 *   WARP_R3_ENTRY_TRAMPOLINE = RET page + 0x20, R-X, 1 stub
 *   WARP_R3_STACK_BASE     = 0x00A080002000  user stack,    RW-, 256 KiB
 *
 * Physical zone separation (prevents commitVirtualMemory from zeroing JIT):
 *   linmem: pfa_alloc_pages_above(WASMOS_SHMEM_PHYS_LIMIT = 64 MiB)
 *   JIT:    pfa_alloc_pages_above(WARP_JIT_PHYS_MIN       = 256 MiB)
 */
#ifndef WASMOS_WARP_RING3_H
#define WASMOS_WARP_RING3_H

#include <stdint.h>
#include "syscall.h"

/* User-mode base address (matches USER_VA_MIN in cpu_x86_64.c). */
#define USER_VA_MIN 0x0000008000000000ULL

/* VA regions. */
#define WARP_R3_JIT_BASE (USER_VA_MIN)
#define WARP_R3_LINMEM_BASE (USER_VA_MIN + 0x2000000000ULL)
#define WARP_R3_HC_TRAMPOLINE (WARP_R3_LINMEM_BASE + 0x80000000ULL)
#define WARP_R3_RET_TRAMPOLINE (WARP_R3_HC_TRAMPOLINE + 0x1000ULL)
#define WARP_R3_MEMHELPER_TRAMPOLINE (WARP_R3_RET_TRAMPOLINE + 0x10ULL)
#define WARP_R3_ENTRY_TRAMPOLINE (WARP_R3_RET_TRAMPOLINE + 0x20ULL)
#define WARP_R3_STACK_BASE (WARP_R3_RET_TRAMPOLINE + 0x1000ULL)
#define WARP_R3_STACK_SIZE (256ULL * 1024ULL)
#define WARP_R3_STACK_PAGES (WARP_R3_STACK_SIZE / 4096ULL)
#define WARP_R3_STACK_TOP (WARP_R3_STACK_BASE + WARP_R3_STACK_SIZE)

/* User VA of HC stub N (8 bytes per stub). */
#define WARP_R3_HC_VA(n) (WARP_R3_HC_TRAMPOLINE + (uint64_t)(n) * 8ULL)

/* Syscall IDs. WASMOS_SYSCALL_WARP_RETURN is defined in syscall.h enum. */
#define WARP_HC_SYSCALL_BASE 0x100U
#define WARP_HC_MAX 128U

/* Physical zone floor for JIT allocations (separate from linmem zone). */
#define WARP_JIT_PHYS_MIN (256ULL * 1024ULL * 1024ULL)

/* Ordered hostcall IDs — generated from abi/hostcalls.yaml
 * (scripts/gen_abi_hostcalls.py); position == ring-3 syscall id. */
#include "wasmos_hostcall_ids.h"

/* ring3_trampolines.c
 *
 * Ring-3 execution state is per-process (stored on the wasm_driver: r3_user_root,
 * r3_stack_phys) and per-thread (warp_r3_old_cr3 / _active / _jbuf on thread_t).
 * setup/teardown therefore take the root and stack as parameters rather than a
 * global singleton — a global was racy under SMP, where a concurrent
 * spawn/teardown could destroy a different, live process's address space. */
int warp_r3_setup(uint64_t* out_user_root, uint64_t* out_stack_phys);
void warp_r3_teardown(uint64_t user_root, uint64_t stack_phys);

/* link.cpp (exposed as C for syscall.c).
 * frame is syscall_frame_t* but declared void* to avoid circular include. */
#ifdef __cplusplus
extern "C" {
#endif
uint32_t warp_ring3_dispatch(uint32_t hc_id, void* frame);
#ifdef __cplusplus
}
#endif

#endif /* WASMOS_WARP_RING3_H */
