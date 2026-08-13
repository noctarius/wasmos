/* system_control.h - Machine power state. Both entry points disable interrupts,
 * then try each known hardware mechanism in turn (there is no single portable
 * one; QEMU, Bochs and real firmware answer different ports). If every attempt
 * is ignored the CPU halts forever rather than returning, so the noreturn
 * attribute holds either way.
 *
 * Reachable from a guest only through the system.control capability. */
#ifndef WASMOS_SYSTEM_CONTROL_H
#define WASMOS_SYSTEM_CONTROL_H

void kernel_system_poweroff(void) __attribute__((noreturn));
void kernel_system_reboot(void) __attribute__((noreturn));

#endif
