#include <stdint.h>
#include "wasmos/mutex.h"
#include "wasmos/syscall_x86_64.h"

static wasmos_mutex_t g_probe_mutex = WASMOS_MUTEX_INITIALIZER;

void
_start(void)
{
    wasmos_mutex_init(&g_probe_mutex);
    (void)wasmos_mutex_try_lock(&g_probe_mutex);
    (void)wasmos_mutex_unlock(&g_probe_mutex);
    (void)wasmos_sys_ipc_notify(0xFFFFFFFFu);
    (void)wasmos_sys_yield();
    (void)wasmos_sys_gettid();
    (void)wasmos_sys_thread_yield();
    (void)wasmos_sys_thread_create(0, 0);
    (void)wasmos_sys_thread_join((uint32_t)wasmos_sys_gettid());
    (void)wasmos_sys_thread_detach((uint32_t)wasmos_sys_gettid());
    (void)wasmos_sys_thread_detach(0);

    for (uint32_t i = 0; i < 128u; ++i) {
        (void)wasmos_sys_getpid();
    }

    wasmos_sys_thread_exit(0);
}
