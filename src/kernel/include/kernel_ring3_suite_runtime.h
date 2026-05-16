#ifndef WASMOS_KERNEL_RING3_SUITE_RUNTIME_H
#define WASMOS_KERNEL_RING3_SUITE_RUNTIME_H

#include <stdint.h>

int kernel_ring3_spawn_suite(uint32_t init_pid,
                             uint8_t ring3_thread_lifecycle_smoke_enabled,
                             uint8_t ring3_fault_churn_rounds);

#endif
