#include <stdint.h>
#include "wasmos/syscall_x86_64.h"

static uint8_t g_join_stack[4096] __attribute__((aligned(16)));
static uint8_t g_detach_stack[4096] __attribute__((aligned(16)));

static uint64_t
stack_top_for(uint8_t *stack, uint64_t size)
{
    return ((uint64_t)(uintptr_t)(stack + size)) & ~0xFULL;
}

static void
join_helper_thread(void)
{
    wasmos_sys_thread_exit(7);
}

static void
detach_helper_thread(void)
{
    wasmos_sys_thread_exit(0);
}

void
_start(void)
{
    int64_t join_tid = wasmos_sys_thread_create((uint64_t)(uintptr_t)join_helper_thread,
                                                stack_top_for(g_join_stack, sizeof(g_join_stack)));
    if (join_tid > 0) {
        (void)wasmos_sys_thread_join((uint32_t)join_tid);
    }

    int64_t detach_tid = wasmos_sys_thread_create((uint64_t)(uintptr_t)detach_helper_thread,
                                                  stack_top_for(g_detach_stack, sizeof(g_detach_stack)));
    if (detach_tid > 0) {
        (void)wasmos_sys_thread_detach((uint32_t)detach_tid);
        (void)wasmos_sys_thread_join((uint32_t)detach_tid);
    }

    for (uint32_t i = 0; i < 16u; ++i) {
        (void)wasmos_sys_thread_yield();
    }

    wasmos_sys_thread_exit(0);
}
