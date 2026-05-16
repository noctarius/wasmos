#ifndef WASMOS_KERNEL_THREADING_SELFTEST_RUNTIME_H
#define WASMOS_KERNEL_THREADING_SELFTEST_RUNTIME_H

#include <stdint.h>

int kernel_threading_selftest_spawn(uint32_t init_pid, uint8_t ring3_thread_lifecycle_smoke_enabled);

#endif
