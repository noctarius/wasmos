#ifndef WASMOS_WARP_EXCEPTION_H
#define WASMOS_WARP_EXCEPTION_H

/* The WARP backend's C++ exception checkpoint (warp/cxx_abi.cpp).  WARP throws
 * C++ exceptions internally; the kernel catches every one of them at the driver
 * boundary instead of unwinding.  Before a call into WARP, warp_driver.cpp arms a
 * checkpoint with warp_setjmp(jbuf) and active = 1, and disarms it when the
 * call returns.  __cxa_throw longjmps to the armed checkpoint, or panics when none
 * is armed.  Nested checkpoints are not supported. */

#include <stdint.h>

#include "arch/warp_jmp.h"

/* Depth of nested function-local static initialisation a thread may reach; one
 * more panics (warp_exception_note_guard). */
#define WARP_EXCEPTION_MAX_GUARDS 8u

typedef struct warp_exception_checkpoint {
    warp_jmp_buf_t jbuf; /* warp_setjmp buffer (arch/warp_jmp.h) */
    int active;          /* 1 while armed */
    /* Top of the SJLJ unwind context stack (warp/sjlj_unwind.cpp), which shares
     * the checkpoint's owner. */
    void* sjlj_top;
    /* Function-local static guards (warp/cxa_guard.h) this thread holds BUSY,
     * innermost last: a throw skips their __cxa_guard_abort. */
    unsigned long long* guards[WARP_EXCEPTION_MAX_GUARDS];
    uint32_t guard_count;
} warp_exception_checkpoint_t;

#ifdef __cplusplus
extern "C" {
#endif

/* The calling thread's checkpoint, which stays with the thread when it is switched
 * out or moves to another CPU: the pointer is valid for the thread's lifetime.
 * Before the scheduler runs a thread on this CPU there is no thread to own one, and
 * the CPU's own checkpoint is returned (one per CPU, capped at WASMOS_MAX_CPUS; a
 * cpu_id at or above the cap folds onto slot 0).  The thread is read with
 * preemption disabled. */
warp_exception_checkpoint_t* warp_exception_get_checkpoint(void);

/* Disarm `ckpt` for delivery of a throw, settling what the longjmp to the checkpoint
 * skips because nothing is unwound: every guard the thread holds BUSY returns to 0,
 * so the static is initialised again on its next use rather than spun on forever,
 * and the SJLJ stack is emptied, since every frame registered since arming is
 * discarded.  __cxa_throw calls it immediately before warp_longjmp. */
void warp_exception_abandon(warp_exception_checkpoint_t* ckpt);

/* Record that the calling thread now holds guard `g` BUSY / no longer does.  Called
 * by warp_cxa_guard_acquire on success and by its release and abort.  Nesting deeper
 * than WARP_EXCEPTION_MAX_GUARDS panics: an untracked guard would be left BUSY by a
 * throw. */
void warp_exception_note_guard(unsigned long long* g);
void warp_exception_drop_guard(unsigned long long* g);

#ifdef __cplusplus
}
#endif

#endif
