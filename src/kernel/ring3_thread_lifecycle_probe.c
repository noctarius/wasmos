/* ring3_thread_lifecycle_probe.c - Ring-3 thread lifecycle test binary.
 * Flat binary loaded by kernel_ring3_probe_runtime.c to verify that thread
 * spawn, yield, join, and exit syscalls work correctly from user space. */
#include <stdint.h>
#include "wasmos/syscall_x86_64.h"
#include "wasmos/thread_x86_64.h"

enum {
    PROBE_STACK_SIZE = 4096
};

static uint8_t g_probe_join_stack[PROBE_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t g_probe_detach_stack[PROBE_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t g_probe_post_exit_stack[PROBE_STACK_SIZE] __attribute__((aligned(16)));

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

typedef struct {
    int32_t status;
} probe_cont_t;

static void
probe_cont_cb(void *ctx, int32_t status)
{
    probe_cont_t *state = (probe_cont_t *)ctx;
    if (state) {
        state->status = status;
    }
}

void
_start(void)
{
    probe_cont_t cont = {0};
    uint32_t join_tid = 0;
    uint32_t detach_tid = 0;
    uint32_t post_exit_tid = 0;

    if (wasmos_thread_spawn_cont(g_probe_join_stack,
                                 PROBE_STACK_SIZE,
                                 (wasmos_thread_entry_fn_t)join_helper_thread,
                                 0,
                                 probe_cont_cb,
                                 &cont,
                                 &join_tid) > 0) {
        (void)wasmos_thread_join_cont(join_tid, probe_cont_cb, &cont);
    }

    if (wasmos_thread_spawn_cont(g_probe_detach_stack,
                                 PROBE_STACK_SIZE,
                                 (wasmos_thread_entry_fn_t)detach_helper_thread,
                                 0,
                                 probe_cont_cb,
                                 &cont,
                                 &detach_tid) > 0) {
        (void)wasmos_thread_detach_cont(detach_tid, probe_cont_cb, &cont);
        (void)wasmos_thread_join_cont(detach_tid, probe_cont_cb, &cont);
    }

    /* Join-after-exit ordering probe: let target finish, then join. */
    if (wasmos_thread_spawn_cont(g_probe_post_exit_stack,
                                 PROBE_STACK_SIZE,
                                 (wasmos_thread_entry_fn_t)post_exit_helper_thread,
                                 0,
                                 probe_cont_cb,
                                 &cont,
                                 &post_exit_tid) > 0) {
        for (uint32_t i = 0; i < 8u; ++i) {
            (void)wasmos_sys_thread_yield();
        }
        (void)wasmos_thread_join_cont(post_exit_tid, probe_cont_cb, &cont);
    }

    for (uint32_t i = 0; i < 16u; ++i) {
        (void)wasmos_sys_thread_yield();
    }

    wasmos_sys_thread_exit(0);
}
