#include <stdint.h>
#include "wasmos/syscall_x86_64.h"

static uint64_t
stack_aligned(uint64_t sp)
{
    return sp & ~0xFULL;
}

static uint64_t
current_sp(void)
{
    uint64_t sp = 0;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    return sp;
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
    uint64_t sp = stack_aligned(current_sp());
    uint64_t join_stack_top = stack_aligned(sp - 0x200u);
    uint64_t detach_stack_top = stack_aligned(sp - 0x800u);

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

    for (uint32_t i = 0; i < 16u; ++i) {
        (void)wasmos_sys_thread_yield();
    }

    wasmos_sys_thread_exit(0);
}
