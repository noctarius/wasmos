#ifndef WASMOS_PROCESS_MANAGER_H
#define WASMOS_PROCESS_MANAGER_H

#include <stdint.h>
#include "boot.h"
#include "process.h"
#include "wasmos_driver_abi.h"

int process_manager_init(const boot_info_t *boot_info);
uint32_t process_manager_endpoint(void);
process_run_result_t process_manager_entry(process_t *process, void *arg);

#endif
