#ifndef WASMOS_PROCESS_MANAGER_H
#define WASMOS_PROCESS_MANAGER_H

#include <stdint.h>
#include "boot.h"
#include "process.h"
#include "wasmos_driver_abi.h"

#define PM_BUFFER_KIND_FS 1u
#define PM_BUFFER_KIND_FRAMEBUFFER 2u

#define PM_BUFFER_BORROW_READ  0x1u
#define PM_BUFFER_BORROW_WRITE 0x2u

int process_manager_init(const boot_info_t *boot_info);
uint32_t process_manager_endpoint(void);
uint32_t process_manager_fs_endpoint(void);
uint32_t process_manager_block_endpoint(void);
uint32_t process_manager_vt_endpoint(void);
uint32_t process_manager_framebuffer_endpoint(void);
void process_manager_set_framebuffer_endpoint(uint32_t endpoint);
void *process_manager_buffer_for_context(uint32_t kind, uint32_t context_id);
uint32_t process_manager_buffer_size(uint32_t kind);
int process_manager_buffer_borrow_context(uint32_t kind,
                                          uint32_t borrower_context_id,
                                          uint32_t source_context_id,
                                          uint32_t flags);
int process_manager_buffer_release_context(uint32_t kind, uint32_t borrower_context_id);
uint32_t process_manager_buffer_borrow_flags(uint32_t kind, uint32_t context_id);
uint32_t process_manager_buffer_borrow_source_context(uint32_t kind, uint32_t borrower_context_id);
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
void process_manager_inject_wait_owner_mismatch_test(uint32_t expected_owner_context_id);
void process_manager_inject_kill_owner_deny_test(void);
void process_manager_inject_status_owner_deny_test(void);
void process_manager_inject_spawn_owner_deny_test(void);
process_run_result_t process_manager_entry(process_t *process, void *arg);

#endif
