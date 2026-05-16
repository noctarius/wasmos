#ifndef WASMOS_KERNEL_SELFTEST_RUNTIME_H
#define WASMOS_KERNEL_SELFTEST_RUNTIME_H

#include <stdint.h>

int kernel_selftest_spawn_baseline(uint32_t init_pid, uint8_t preempt_test_enabled);

#endif
