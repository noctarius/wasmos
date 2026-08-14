/* process_manager.h - The WASMOS process manager (PM) public API.
 *
 * The PM is a kernel process spawned by init during bring-up; it owns
 * WASMOS-APP lifecycle -- spawn, kill, wait -- and the service registry.
 * Transfer-buffer APIs are provided separately via xfer_buffer.h. */
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

/* Prepare PM state from the boot info (which is borrowed for the life of the
 * kernel, not copied) and create the app/wait/service lists. Returns 0, or -1
 * if any list could not be created. Does NOT create endpoints or spawn a
 * process: the PM's own first dispatch does that. Call once, before the PM
 * process is spawned. */
int process_manager_init(const boot_info_t* boot_info);

/* Return the IPC endpoint number for the PM's main message queue. */
uint32_t process_manager_endpoint(void);

/* Endpoints for specific PM service channels. Each returns IPC_ENDPOINT_NONE
 * until the corresponding service registers (or, for the proc endpoint, until
 * the PM's first dispatch creates it), so every caller must handle that value.
 * Safe to call from any CPU: the fields are read with acquire semantics. */
uint32_t process_manager_fs_endpoint(void);
uint32_t process_manager_block_endpoint(void);
uint32_t process_manager_vt_endpoint(void);
uint32_t process_manager_framebuffer_endpoint(void);
/* Publish a framebuffer endpoint and register it under the service name "fb" as
 * kernel-owned. Used by in-kernel framebuffer providers, which have no context
 * of their own to register from. */
void process_manager_set_framebuffer_endpoint(uint32_t endpoint);

/* Test injection hooks (no-ops unless WASMOS_PM_TEST_HOOKS is set). Each
 * fabricates the one request that drives a PM denial path -- an ownership
 * check that no ordinary caller can fail on purpose -- and returns without
 * waiting; the effect shows up on a later PM dispatch. */
void process_manager_inject_wait_owner_mismatch_test(uint32_t expected_owner_context_id);
void process_manager_inject_kill_owner_deny_test(void);
void process_manager_inject_status_owner_deny_test(void);
void process_manager_inject_spawn_owner_deny_test(void);

/* PM kernel process entry point; runs the PM IPC event loop.
 *
 * One iteration per dispatch: it creates its endpoints on the first call, runs
 * the periodic work (waits, app reaping, class reaping, spawn polling), then
 * handles AT MOST ONE queued request before returning PROCESS_RUN_YIELDED. With
 * nothing queued it blocks on its select set for up to
 * WASMOS_PM_POLL_INTERVAL_MS, so the exit-driven periodic work still runs when
 * no IPC arrives. Returns PROCESS_RUN_EXITED only if endpoint or select-set
 * setup fails. Unknown message types are dropped silently -- never
 * error-replied, since a peer that does the same would ping-pong forever. */
process_run_result_t process_manager_entry(process_t* process, void* arg);

/* Called by a child process once it considers itself ready; unblocks waiting parent.
 * Only latches the child's ready flag -- the reply to a parent blocked in
 * PROC_IPC_SPAWN_SYNC is sent by the PM's own pm_poll_spawn on a later
 * dispatch. Callable from any CPU for exactly that reason: it must not touch
 * the PM's in-flight spawn state. No-op for an unknown pid. */
void process_manager_on_child_ready(uint32_t pid);

#ifdef __cplusplus
}
#endif

#endif
