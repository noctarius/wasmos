#ifndef WASMOS_MEMORY_SERVICE_H
#define WASMOS_MEMORY_SERVICE_H

#include <stdint.h>
#include "ipc.h"
#include "process.h"

void memory_service_register(uint32_t context_id, uint32_t endpoint, uint32_t reply_endpoint);
int memory_service_process_once(void);
int memory_service_handle_fault_ipc(uint32_t fault_context_id, uint64_t fault_addr, uint64_t error_code);
process_run_result_t memory_service_entry(process_t *process, void *arg);

#endif
