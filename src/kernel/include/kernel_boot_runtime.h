/* kernel_boot_runtime.h - Boot-time integration test entry point. */
#ifndef WASMOS_KERNEL_BOOT_RUNTIME_H
#define WASMOS_KERNEL_BOOT_RUNTIME_H

#include "boot.h"

/* Re-home the firmware's boot_info and its blobs into kernel-owned memory addressed
 * through the higher-half alias, so no later kernel code depends on the low identity map.
 * *dst is a shallow copy of *src with the RSDP and boot-config blobs duplicated into
 * freshly allocated frames and the initfs pointer rebased onto its alias (no copy — the
 * shared higher-half window already covers it, provided the firmware placed it inside
 * that window).  Returns 0 on success, -1 on a NULL argument or a failed allocation, in
 * which case *dst may hold a partially rebuilt copy. */
int kernel_boot_build_bootinfo_shadow(const boot_info_t* src, boot_info_t* dst);

/* Strip PML4[0] from every live, non-idle process root and verify the result, so no user
 * address space keeps an identity window into physical memory.  Failures are logged, not
 * fatal.  Run once, after every early process exists. */
void kernel_boot_run_low_slot_sweep_diagnostic(void);

/* The bootstrap processor's terminal scheduler loop.  Does not return: it panics only
 * when not even the idle thread is dispatchable. */
void kernel_boot_run_scheduler_loop(void);

#endif
