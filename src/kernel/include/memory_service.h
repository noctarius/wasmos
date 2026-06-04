/* memory_service.h - Kernel IPC service for user-space memory allocation requests.
 * Runs as a kernel process alongside the main scheduler; handles IPC_MEM_FAULT
 * messages from page-fault paths and allocates/maps pages on demand. */
#ifndef WASMOS_MEMORY_SERVICE_H
#define WASMOS_MEMORY_SERVICE_H

#include <stdint.h>
#include "ipc.h"
#include "process.h"

/* Register context_id's endpoint pair with the memory service. */
void memory_service_register(uint32_t context_id, uint32_t endpoint, uint32_t reply_endpoint);

/* Drain one pending memory service IPC message; returns 0 if nothing was processed. */
int memory_service_process_once(void);

/* Handle an IPC_MEM_FAULT from a page fault for fault_context_id at fault_addr. */
int memory_service_handle_fault_ipc(uint32_t fault_context_id, uint64_t fault_addr, uint64_t error_code);

/* Kernel process entry point for the memory service loop. */
process_run_result_t memory_service_entry(process_t *process, void *arg);

#endif
