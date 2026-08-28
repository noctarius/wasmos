/* system_control.h - Machine power state. Both entry points disable interrupts,
 * then try each known hardware mechanism in turn (there is no single portable
 * one; QEMU, Bochs and real firmware answer different ports). If every attempt
 * is ignored the CPU halts forever rather than returning, so the noreturn
 * attribute holds either way.
 *
 * Reachable from a guest only through the system.control capability. */
#ifndef WASMOS_SYSTEM_CONTROL_H
#define WASMOS_SYSTEM_CONTROL_H

#include <stdint.h>

void kernel_system_poweroff(void) __attribute__((noreturn));
void kernel_system_reboot(void) __attribute__((noreturn));

/* Bring the machine down for `reason` (WASMOS_SHUTDOWN_REASON_*), giving every
 * registered driver and service the orderly shutdown sequence first. This is
 * what a halt/reboot host call calls; it does not return.
 *
 * The sequence needs IPC round trips with the participants, so it is stepped by
 * the process manager's dispatch loop and normally powers the machine off from
 * there. This function PARKS the caller until that happens -- on a single-CPU
 * guest a yield loop would keep it runnable and the process manager would never
 * be scheduled -- and powers the machine off itself if the sequence stops making
 * progress. Halt always halts.
 *
 * `context_id` is the calling process's; the parking endpoint is created in it. */
void kernel_system_shutdown(uint32_t reason, uint32_t context_id) __attribute__((noreturn));
/* Record the request and return. Separated from kernel_system_shutdown for the
 * PM's own use and for tests; a caller that arms without waiting must have some
 * other reason to believe the machine will go down. */
void kernel_system_shutdown_arm(uint32_t reason);

#endif
