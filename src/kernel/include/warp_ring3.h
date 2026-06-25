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
#define USER_VA_MIN              0x0000008000000000ULL

/* VA regions. */
#define WARP_R3_JIT_BASE         (USER_VA_MIN)
#define WARP_R3_LINMEM_BASE      (USER_VA_MIN + 0x2000000000ULL)
#define WARP_R3_HC_TRAMPOLINE    (WARP_R3_LINMEM_BASE + 0x80000000ULL)
#define WARP_R3_RET_TRAMPOLINE   (WARP_R3_HC_TRAMPOLINE + 0x1000ULL)
#define WARP_R3_MEMHELPER_TRAMPOLINE (WARP_R3_RET_TRAMPOLINE + 0x10ULL)
#define WARP_R3_ENTRY_TRAMPOLINE (WARP_R3_RET_TRAMPOLINE + 0x20ULL)
#define WARP_R3_STACK_BASE       (WARP_R3_RET_TRAMPOLINE + 0x1000ULL)
#define WARP_R3_STACK_SIZE       (256ULL * 1024ULL)
#define WARP_R3_STACK_PAGES      (WARP_R3_STACK_SIZE / 4096ULL)
#define WARP_R3_STACK_TOP        (WARP_R3_STACK_BASE + WARP_R3_STACK_SIZE)

/* User VA of HC stub N (8 bytes per stub). */
#define WARP_R3_HC_VA(n)         (WARP_R3_HC_TRAMPOLINE + (uint64_t)(n) * 8ULL)

/* Syscall IDs. WASMOS_SYSCALL_WARP_RETURN is defined in syscall.h enum. */
#define WARP_HC_SYSCALL_BASE        0x100U
#define WARP_HC_MAX                 128U

/* Physical zone floor for JIT allocations (separate from linmem zone). */
#define WARP_JIT_PHYS_MIN        (256ULL * 1024ULL * 1024ULL)

/* Ordered hostcall IDs — must match WASMOS_SYMBOLS expansion order. */
typedef enum {
    HC_IPC_CREATE_ENDPOINT = 0,
    HC_IPC_ENDPOINT_OWNER  = 1,
    HC_IPC_SEND            = 2,
    HC_IPC_SELECT_ONE      = 3,
    HC_IPC_RECV            = 4,   /* alias: ipc_recv → warp_ipc_select_one */
    HC_IPC_DRAIN           = 5,
    HC_IPC_TRY_RECV        = 6,   /* alias: ipc_try_recv → warp_ipc_drain */
    HC_IPC_NOTIFY          = 7,
    HC_IPC_LAST_FIELD      = 8,
    HC_CONSOLE_READ        = 9,
    HC_CONSOLE_WRITE       = 10,
    HC_PROC_EXIT           = 11,
    HC_PROC_NOTIFY_READY   = 12,
    HC_SCHED_YIELD         = 13,
    HC_SCHED_CURRENT_PID   = 14,
    HC_THREAD_GETTID       = 15,
    HC_FUTEX_WAIT          = 16,
    HC_FUTEX_WAKE          = 17,
    HC_IPC_SELECT_CREATE   = 18,
    HC_IPC_SELECT_ADD      = 19,
    HC_IPC_SELECT_WAIT     = 20,
    HC_IPC_SELECT_DESTROY  = 21,
    HC_SYS_SELECT_CREATE   = 22,  /* alias: sys_select_create → warp_ipc_select_create */
    HC_SYS_SELECT_ADD      = 23,  /* alias: sys_select_add → warp_ipc_select_add */
    HC_SYS_SELECT_WAIT     = 24,  /* alias: sys_select_wait → warp_ipc_select_wait */
    HC_SYS_SELECT_DESTROY  = 25,  /* alias: sys_select_destroy → warp_ipc_select_destroy */
    HC_XFER_BUFFER_SIZE      = 26,
    HC_FS_ENDPOINT         = 27,
    HC_XFER_BUFFER_READ      = 28,
    HC_XFER_BUFFER_WRITE     = 29,
    HC_BUFFER_BORROW       = 30,
    HC_BUFFER_RELEASE      = 31,
    HC_BLOCK_BUFFER_PHYS   = 32,
    HC_BLOCK_BUFFER_COPY   = 33,
    HC_BLOCK_BUFFER_WRITE  = 34,
    HC_IO_IN8              = 35,
    HC_IO_IN16             = 36,
    HC_IO_IN32             = 37,
    HC_IO_OUT8             = 38,
    HC_IO_OUT16            = 39,
    HC_IO_OUT32            = 40,
    HC_IO_WAIT             = 41,
    HC_ACPI_RSDP_INFO      = 42,
    HC_BOOT_MODULE_NAME    = 43,
    HC_SYNC_USER_READ      = 44,
    HC_SYSTEM_HALT         = 45,
    HC_SYSTEM_REBOOT       = 46,
    HC_SCHED_TICKS         = 47,
    HC_PROC_COUNT          = 48,
    HC_SCHED_READY_COUNT   = 49,
    HC_SCHED_CPU_COUNT     = 50,
    HC_PHYSMEM_STATS       = 51,
    HC_KERNEL_RUNTIME      = 52,
    HC_DEBUG_MARK          = 53,
    HC_KMAP_DUMP           = 54,
    HC_KMAP_DUMP_ALL       = 55,
    HC_INITFS_ENTRY_COUNT  = 56,
    HC_INITFS_ENTRY_NAME   = 57,
    HC_INITFS_ENTRY_SIZE   = 58,
    HC_INITFS_ENTRY_COPY   = 59,
    HC_DMA_MAP_BORROW      = 60,
    HC_DMA_SYNC_BORROW     = 61,
    HC_DMA_UNMAP_BORROW    = 62,
    HC_PHYS_MAP            = 63,
    HC_PROC_INFO           = 64,
    HC_PROC_INFO_EX        = 65,
    HC_PROC_INFO_STATS     = 66,
    HC_XFER_BUFFER_BORROW    = 67,
    HC_XFER_BUFFER_RELEASE   = 68,
    HC_SCHED_CPU_STATS     = 69,
    HC_THREAD_CREATE       = 70,
    HC_THREAD_YIELD        = 71,
    HC_THREAD_EXIT         = 72,
    HC_THREAD_JOIN         = 73,
    HC_THREAD_DETACH       = 74,
    HC_SHMEM_CREATE        = 75,
    HC_SHMEM_GRANT         = 76,
    HC_SHMEM_REVOKE        = 77,
    HC_SHMEM_MAP           = 78,
    HC_SHMEM_MAP_AUTO      = 79,
    HC_SHMEM_FLUSH         = 80,
    HC_SHMEM_REFRESH       = 81,
    HC_SHMEM_UNMAP         = 82,
    HC_IRQ_ROUTE_IPC       = 83,
    HC_IRQ_ACK             = 84,
    HC_IRQ_UNROUTE         = 85,
    HC_SERIAL_REGISTER     = 86,
    HC_INPUT_PUSH          = 87,
    HC_INPUT_READ          = 88,
    HC_FRAMEBUFFER_INFO    = 89,
    HC_FRAMEBUFFER_MAP     = 90,
    HC_FRAMEBUFFER_PIXEL   = 91,
    HC_BOOT_CONFIG_SIZE    = 92,
    HC_BOOT_CONFIG_COPY    = 93,
    HC_INITFS_FIND_PATH    = 94,
    HC_EARLY_LOG_SIZE      = 95,
    HC_EARLY_LOG_COPY      = 96,
    HC_ENV_GET             = 97,
    HC_ENV_SET             = 98,
    HC_ENV_UNSET           = 99,
    HC_ENV_ABORT           = 100,
    HC_COUNT               = 101,
} warp_hostcall_id_t;

/* ring3_trampolines.c
 *
 * Ring-3 execution state is per-process (stored on the wasm_driver: r3_user_root,
 * r3_stack_phys) and per-thread (warp_r3_old_cr3 / _active / _jbuf on thread_t).
 * setup/teardown therefore take the root and stack as parameters rather than a
 * global singleton — a global was racy under SMP, where a concurrent
 * spawn/teardown could destroy a different, live process's address space. */
int  warp_r3_setup(uint64_t *out_user_root, uint64_t *out_stack_phys);
void warp_r3_teardown(uint64_t user_root, uint64_t stack_phys);

/* link.cpp (exposed as C for syscall.c).
 * frame is syscall_frame_t* but declared void* to avoid circular include. */
#ifdef __cplusplus
extern "C" {
#endif
uint32_t warp_ring3_dispatch(uint32_t hc_id, void *frame);
#ifdef __cplusplus
}
#endif

#endif /* WASMOS_WARP_RING3_H */
