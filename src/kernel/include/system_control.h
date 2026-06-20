#ifndef WASMOS_SYSTEM_CONTROL_H
#define WASMOS_SYSTEM_CONTROL_H

void kernel_system_poweroff(void) __attribute__((noreturn));
void kernel_system_reboot(void) __attribute__((noreturn));

#endif
