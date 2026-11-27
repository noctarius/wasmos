#ifndef WASMOS_WARP_EXCEPTION_H
#define WASMOS_WARP_EXCEPTION_H

/* The WARP backend's C++ exception checkpoint (warp/cxx_abi.cpp).  WARP throws
 * C++ exceptions internally; the kernel catches every one of them at the driver
 * boundary instead of unwinding.  Before a call into WARP, warp_driver.cpp arms a
 * checkpoint with __builtin_setjmp(jbuf) and active = 1, and disarms it when the
 * call returns.  __cxa_throw longjmps to the armed checkpoint, or panics when none
 * is armed.  Nested checkpoints are not supported. */

#include <stdint.h>

typedef struct warp_exception_checkpoint {
    void* jbuf[5]; /* __builtin_setjmp buffer */
    int active;    /* 1 while armed */
    /* Top of the SJLJ unwind context stack (warp/sjlj_unwind.cpp), which shares
     * the checkpoint's owner. */
    void* sjlj_top;
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

#ifdef __cplusplus
}
#endif

#endif
