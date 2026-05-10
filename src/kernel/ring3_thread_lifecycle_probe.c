#include <stdint.h>
#include "wasmos/syscall_x86_64.h"

static uint64_t
stack_aligned(uint64_t sp)
{
    return sp & ~0xFULL;
}

enum {
    PROBE_STACK_SIZE = 4096
};

static uint8_t g_probe_join_stack[PROBE_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t g_probe_detach_stack[PROBE_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t g_probe_post_exit_stack[PROBE_STACK_SIZE] __attribute__((aligned(16)));

static uint64_t
stack_top(uint8_t *stack, uint32_t stack_size)
{
    return stack_aligned((uint64_t)(uintptr_t)(stack + stack_size));
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

static void
post_exit_helper_thread(void)
{
    wasmos_sys_thread_exit(9);
}

void
_start(void)
{
    uint64_t join_stack_top = stack_top(g_probe_join_stack, PROBE_STACK_SIZE);
    uint64_t detach_stack_top = stack_top(g_probe_detach_stack, PROBE_STACK_SIZE);
    uint64_t post_exit_stack_top = stack_top(g_probe_post_exit_stack, PROBE_STACK_SIZE);

    int64_t join_tid = wasmos_sys_thread_create((uint64_t)(uintptr_t)join_helper_thread, join_stack_top);
    if (join_tid > 0) {
        (void)wasmos_sys_thread_join((uint32_t)join_tid);
    }

    int64_t detach_tid =
        wasmos_sys_thread_create((uint64_t)(uintptr_t)detach_helper_thread, detach_stack_top);
    if (detach_tid > 0) {
        (void)wasmos_sys_thread_detach((uint32_t)detach_tid);
        (void)wasmos_sys_thread_join((uint32_t)detach_tid);
    }

    /* Join-after-exit ordering probe: let target finish, then join. */
    int64_t post_exit_tid =
        wasmos_sys_thread_create((uint64_t)(uintptr_t)post_exit_helper_thread, post_exit_stack_top);
    if (post_exit_tid > 0) {
        for (uint32_t i = 0; i < 8u; ++i) {
            (void)wasmos_sys_thread_yield();
        }
        (void)wasmos_sys_thread_join((uint32_t)post_exit_tid);
    }

    /* TODO(threading): migrate this probe to wasmos/thread_x86_64.h once
     * ring3-threading startup guarantees writable user stack space for wrapper
     * bootstrap metadata writes. */
    for (uint32_t i = 0; i < 16u; ++i) {
        (void)wasmos_sys_thread_yield();
    }

    wasmos_sys_thread_exit(0);
}
