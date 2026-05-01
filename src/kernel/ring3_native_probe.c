#include <stdint.h>
#include "wasmos/syscall_x86_64.h"

void
_start(void)
{
    (void)wasmos_sys_ipc_notify(0xFFFFFFFFu);
    (void)wasmos_sys_yield();

    for (uint32_t i = 0; i < 128u; ++i) {
        (void)wasmos_sys_getpid();
    }

    wasmos_sys_exit(0);
}
