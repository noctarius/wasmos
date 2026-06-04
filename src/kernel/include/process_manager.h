/* process_manager.h - The WASMOS process manager (PM) public API.
 *
 * The PM is a kernel process (PID 1-equivalent) that owns all WASMOS-APP
 * lifecycle: spawn, kill, wait, service registration, and shared buffer management.
 * It runs as a cooperative kernel thread processing IPC requests on its endpoint.
 *
 * Buffer management: the PM maintains per-kind (filesystem/framebuffer) shared
 * DMA buffers.  Drivers and services "borrow" a buffer context (read or write)
 * for the duration of a DMA transfer, then release it.  Only one active borrower
 * per buffer kind per context is allowed at a time. */
#ifndef WASMOS_PROCESS_MANAGER_H
#define WASMOS_PROCESS_MANAGER_H

#include <stdint.h>
#include "boot.h"
#include "process.h"
#include "wasmos_driver_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PM_BUFFER_KIND_FILESYSTEM  1u  /* shared FS DMA transfer buffer */
#define PM_BUFFER_KIND_FRAMEBUFFER 2u  /* shared framebuffer display buffer */

#define PM_BUFFER_BORROW_READ  0x1u
#define PM_BUFFER_BORROW_WRITE 0x2u

/* Initialize the process manager and start the PM kernel process. */
int process_manager_init(const boot_info_t *boot_info);

/* Return the IPC endpoint number for the PM's main message queue. */
uint32_t process_manager_endpoint(void);

/* Endpoints for specific PM service channels. */
uint32_t process_manager_fs_endpoint(void);
uint32_t process_manager_block_endpoint(void);
uint32_t process_manager_vt_endpoint(void);
uint32_t process_manager_framebuffer_endpoint(void);
void process_manager_set_framebuffer_endpoint(uint32_t endpoint);

/* Return a kernel virtual pointer to the shared buffer for context_id. */
void *process_manager_buffer_for_context(uint32_t kind, uint32_t context_id);

/* Return the physical address of the shared buffer for DMA mapping. */
uint64_t process_manager_buffer_phys_for_context(uint32_t kind, uint32_t context_id);

/* Return the total size in bytes of the shared buffer. */
uint32_t process_manager_buffer_size(uint32_t kind);

/* Grant borrower_context_id access to source_context_id's buffer with flags.
 * Fails if borrower_context_id already holds an active borrow. */
int process_manager_buffer_borrow_context(uint32_t kind,
                                          uint32_t borrower_context_id,
                                          uint32_t source_context_id,
                                          uint32_t flags);

/* Release an active buffer borrow for borrower_context_id. */
int process_manager_buffer_release_context(uint32_t kind, uint32_t borrower_context_id);

uint32_t process_manager_buffer_borrow_flags(uint32_t kind, uint32_t context_id);
uint32_t process_manager_buffer_borrow_source_context(uint32_t kind, uint32_t borrower_context_id);

/* Release all buffer borrows associated with context_id (called on process exit). */
void process_manager_buffer_drop_context(uint32_t context_id);

/* DMA buffer map/sync/unmap operations for drivers with DMA capability. */
int process_manager_buffer_dma_map(uint32_t kind,
                                   uint32_t borrower_context_id,
                                   uint32_t source_context_id,
                                   uint32_t offset,
                                   uint32_t length,
                                   uint32_t direction_flags,
                                   uint64_t *out_device_addr);
int process_manager_buffer_dma_sync(uint32_t kind,
                                    uint32_t borrower_context_id,
                                    uint32_t offset,
                                    uint32_t length,
                                    uint32_t sync_op);
int process_manager_buffer_dma_unmap(uint32_t kind,
                                     uint32_t borrower_context_id,
                                     uint32_t source_context_id);

/* Test injection hooks (no-ops unless WASMOS_PM_TEST_HOOKS is set). */
void process_manager_inject_wait_owner_mismatch_test(uint32_t expected_owner_context_id);
void process_manager_inject_kill_owner_deny_test(void);
void process_manager_inject_status_owner_deny_test(void);
void process_manager_inject_spawn_owner_deny_test(void);

/* PM kernel process entry point; runs the PM IPC event loop. */
process_run_result_t process_manager_entry(process_t *process, void *arg);

/* Called by a child process once it considers itself ready; unblocks waiting parent. */
void process_manager_on_child_ready(uint32_t pid);

#ifdef __cplusplus
}
#endif

#endif
