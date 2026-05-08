#include <stdint.h>
#include "wasmos/syscall_x86_64.h"
#include "wasmos/thread_x86_64.h"

static uint8_t g_join_stack[0x600];
static uint8_t g_detach_stack[0x600];
static int32_t g_spawn_join_status;
static int32_t g_spawn_detach_status;
static int32_t g_join_result;
static int32_t g_detach_result;

static void
join_helper_thread(void *arg)
{
    (void)arg;
    wasmos_sys_thread_exit(7);
}

static void
detach_helper_thread(void *arg)
{
    (void)arg;
    wasmos_sys_thread_exit(0);
}

static void
capture_status(void *ctx, int32_t status)
{
    int32_t *slot = (int32_t *)ctx;
    if (slot) {
        *slot = status;
    }
}

void
_start(void)
{
    uint32_t join_tid = 0;
    uint32_t detach_tid = 0;

    g_spawn_join_status = wasmos_thread_spawn_cont(g_join_stack,
                                                   sizeof(g_join_stack),
                                                   join_helper_thread,
                                                   NULL,
                                                   capture_status,
                                                   &g_spawn_join_status,
                                                   &join_tid);
    if (g_spawn_join_status > 0) {
        g_join_result = wasmos_thread_join_cont(join_tid, capture_status, &g_join_result);
    }

    g_spawn_detach_status = wasmos_thread_spawn_cont(g_detach_stack,
                                                     sizeof(g_detach_stack),
                                                     detach_helper_thread,
                                                     NULL,
                                                     capture_status,
                                                     &g_spawn_detach_status,
                                                     &detach_tid);
    if (g_spawn_detach_status > 0) {
        g_detach_result = wasmos_thread_detach_cont(detach_tid, capture_status, &g_detach_result);
        (void)wasmos_sys_thread_join(detach_tid);
    }

    for (uint32_t i = 0; i < 16u; ++i) {
        (void)wasmos_sys_thread_yield();
    }

    wasmos_sys_thread_exit(0);
}
