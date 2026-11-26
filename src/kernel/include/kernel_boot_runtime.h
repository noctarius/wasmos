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

/* With the boot option debug.kpanic_stop_test=<mode>, make CPU 1 spin where it
 * takes no ordinary interrupt and panic, so a test can check that kpanic
 * captured that CPU: mode 1 spins with interrupts masked, mode 2 inside the
 * timer interrupt before it is ended. Does nothing without the option; without
 * a second online CPU, or for an unknown mode, it logs that and returns. Boot
 * CPU only, after smp_cpus_up. */
void kernel_boot_kpanic_stop_test(void);

/* With the boot option debug.exception_test=<vector>, raise that CPU exception
 * in ring 0 from a rip and rsp logged beforehand, so a test can check the
 * exception dump against them. x86_64 only, vectors 1 (#DB) and 5; for any
 * other vector or architecture it logs that and returns. Boot CPU only. */
void kernel_boot_exception_test(void);

/* With the boot option debug.leak_test=1, build and release address spaces --
 * with a cloned low slot, and with it stripped again -- and, on an x86_64 WARP
 * kernel, WARP ring-3 setups, a fixed number of times each, and log free
 * physical memory before and after each run ("[leak-test] <what>: cycles=N
 * before=B after=A"), then "[leak-test] done". Boot CPU only, before the other
 * CPUs start, so nothing else allocates while it measures. */
void kernel_boot_leak_test(void);

/* Mode 2's spin: called from each architecture's timer interrupt before the
 * interrupt is ended. Returns at once unless the test is armed in mode 2 and
 * this is CPU 1. */
void kernel_boot_kpanic_stop_test_tick(void);

#endif
