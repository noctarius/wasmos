#ifndef WASMOS_KERNEL_BOOT_RUNTIME_H
#define WASMOS_KERNEL_BOOT_RUNTIME_H

#include "boot.h"

int kernel_boot_build_bootinfo_shadow(const boot_info_t *src, boot_info_t *dst);
void kernel_boot_run_low_slot_sweep_diagnostic(void);
void kernel_boot_run_scheduler_loop(void);

#endif
