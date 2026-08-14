/* memory_service.h - Kernel IPC service for user-space memory allocation requests.
 * Runs as a kernel process alongside the main scheduler; handles IPC_MEM_FAULT
 * messages from page-fault paths and allocates/maps pages on demand. */
#ifndef WASMOS_MEMORY_SERVICE_H
#define WASMOS_MEMORY_SERVICE_H

#include <stdint.h>
#include "ipc.h"
#include "process.h"

/* Record the context the service thread runs as, the endpoint it receives IPC_MEM_FAULT
 * requests on, and the endpoint fault paths expect replies on.  Stores them in module
 * globals, so there is exactly one memory service per kernel; a second call retargets it
 * and resets the request-id counter.  Must precede memory_service_serve_one, which fails
 * until an endpoint and a non-zero context are registered. */
void memory_service_register(uint32_t context_id, uint32_t endpoint, uint32_t reply_endpoint);

/* Block on the memory-service select set until a request arrives, handle it,
 * and reply. Returns 0 on success, 1 on a spurious wake (caller loops), -1 on
 * error. */
int memory_service_serve_one(void);

/* Resolve a page fault for fault_context_id at fault_addr. Despite the name no
 * IPC is involved: it calls mm_handle_page_fault() inline. Round-tripping
 * through the service endpoint would race the mem-service worker thread, which
 * drains the same endpoint on another CPU and would swallow the request,
 * turning a recoverable demand fault into a panic. Returns 0 when the fault was
 * resolved, non-zero otherwise. */
int memory_service_handle_fault_ipc(uint32_t fault_context_id, uint64_t fault_addr,
                                    uint64_t error_code);

/* Kernel process entry point for the memory service loop.  Serves at most one request
 * per dispatch and returns PROCESS_RUN_YIELDED so the scheduler re-runs it; blocks
 * inside memory_service_serve_one between requests rather than spinning.  `arg` is
 * unused.  Returns PROCESS_RUN_IDLE for a NULL process. */
process_run_result_t memory_service_entry(process_t* process, void* arg);

#endif
