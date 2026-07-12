/* process_manager.h - The WASMOS process manager (PM) public API.
 *
 * The PM is a kernel process (PID 1-equivalent) that owns WASMOS-APP
 * lifecycle: spawn, kill, wait, and service registration. Transfer-buffer
 * APIs are provided separately via xfer_buffer.h. */
#ifndef WASMOS_PROCESS_MANAGER_H
#define WASMOS_PROCESS_MANAGER_H

#include <stdint.h>
#include "boot.h"
#include "process.h"
#include "xfer_buffer.h"
#include "wasmos_driver_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

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
