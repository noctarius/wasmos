/* warp/exception_checkpoint.c - where the WARP C++ exception checkpoint lives
 * (include/warp_exception.h). */

#include "cpu_local.h"
#include "kpanic.h"
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

void warp_exception_abandon(warp_exception_checkpoint_t* ckpt) {
    for (uint32_t i = ckpt->guard_count; i > 0; --i) {
        __atomic_store_n(ckpt->guards[i - 1], 0ULL, __ATOMIC_RELEASE);
    }
    ckpt->guard_count = 0;
    ckpt->sjlj_top = 0;
    ckpt->active = 0;
}

void warp_exception_note_guard(unsigned long long* g) {
    warp_exception_checkpoint_t* ckpt = warp_exception_get_checkpoint();
    if (ckpt->guard_count >= WARP_EXCEPTION_MAX_GUARDS) {
        kpanic("warp_static_init_too_deep", (uint64_t)ckpt->guard_count, (uint64_t)(uintptr_t)g);
    }
    ckpt->guards[ckpt->guard_count++] = g;
}

void warp_exception_drop_guard(unsigned long long* g) {
    warp_exception_checkpoint_t* ckpt = warp_exception_get_checkpoint();
    for (uint32_t i = ckpt->guard_count; i > 0; --i) {
        if (ckpt->guards[i - 1] == g) {
            for (uint32_t j = i; j < ckpt->guard_count; ++j) {
                ckpt->guards[j - 1] = ckpt->guards[j];
            }
            --ckpt->guard_count;
            return;
        }
    }
}
