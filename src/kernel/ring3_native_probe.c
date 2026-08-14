/* ring3_native_probe.c - Minimal ring-3 native binary for smoke-testing.
 * Linked as a flat binary (ring3_native_probe.bin, ring3_native.ld) and loaded
 * directly into a ring-3 process by kernel_ring3_probe_runtime.c.
 *
 * Drives one call of every non-destructive int 0x80 syscall the gate exposes --
 * user mutex, ipc_notify, yield, gettid, thread create/yield/join/detach --
 * and ignores every result. What is under test is the gate: each number must
 * return to ring 3, and arguments no real caller would pass (an invalid
 * endpoint, a null thread entry, tid 0, joining the calling thread) must be
 * refused rather than taking the kernel down. The getpid loop keeps the process
 * crossing the gate long enough to be timer-preempted inside it. The probe
 * itself asserts nothing; reaching thread_exit without a fault is the result. */
#include <stdint.h>
#include "wasmos/mutex.h"
#include "wasmos/syscall_x86_64.h"

/* Flat-binary entry point: the loader jumps here with no C runtime, no
 * arguments and no return address, so this must not return — it ends in
 * wasmos_sys_thread_exit(0).  There is no dynamic loader, so no relocation is
 * applied and the binary runs at whatever VA the probe loader mapped it to. */
void _start(void) {
    /* Declare mutex on the stack so writes go to the writable stack region,
     * not the flat-binary data section which is mapped READ+EXEC. */
    wasmos_mutex_t probe_mutex = WASMOS_MUTEX_INITIALIZER;
    wasmos_mutex_init(&probe_mutex);
    (void)wasmos_mutex_try_lock(&probe_mutex);
    (void)wasmos_mutex_unlock(&probe_mutex);
    (void)wasmos_sys_ipc_notify(0xFFFFFFFFu);
    (void)wasmos_sys_yield();
    (void)wasmos_sys_gettid();
    (void)wasmos_sys_thread_yield();
    (void)wasmos_sys_thread_create(0, 0);
    (void)wasmos_sys_thread_join((uint32_t)wasmos_sys_gettid()).status;
    (void)wasmos_sys_thread_detach((uint32_t)wasmos_sys_gettid());
    (void)wasmos_sys_thread_detach(0);

    for (uint32_t i = 0; i < 128u; ++i) {
        (void)wasmos_sys_getpid();
    }

    wasmos_sys_thread_exit(0);
}
