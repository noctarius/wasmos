/* warp/exception_checkpoint.c - where the WARP C++ exception checkpoint lives
 * (include/warp_exception.h). */

#include "cpu_local.h"
#include "process.h"
#include "thread.h"
#include "warp_exception.h"

/* Checkpoints for a CPU that has no current thread.  Zero-initialised, so `active`
 * is 0 until a driver arms one. */
static warp_exception_checkpoint_t g_warp_cpu_ckpt[WASMOS_MAX_CPUS];

warp_exception_checkpoint_t* warp_exception_get_checkpoint(void) {
    preempt_disable();
    cpu_local_t* cpu = cpu_local();
    thread_t* t = cpu->current_thread;
    warp_exception_checkpoint_t* ckpt =
        t ? &t->warp_exc_ckpt : &g_warp_cpu_ckpt[cpu->cpu_id < WASMOS_MAX_CPUS ? cpu->cpu_id : 0u];
    preempt_enable();
    return ckpt;
}
