/* ring3_thread_lifecycle_probe.c - Ring-3 thread lifecycle test binary.
 * Flat binary loaded by kernel_ring3_probe_runtime.c, which drives the thread
 * syscalls from user space in the three orderings that differ in the kernel:
 * spawn then join a still-running thread, spawn then detach then join the
 * detached tid, and spawn then let the thread exit before joining it. Those
 * results are discarded -- the probe reaching thread_exit without faulting is
 * what the orderings are being checked for.
 *
 * One more spawn checks the start record itself: its thread must be entered at
 * the entry function with the argument the spawner passed, which it proves by
 * exiting with START_THREAD_STATUS only when the argument points at
 * g_start_token. The main thread then exits with PROBE_START_RECORD_OK, which
 * the kernel reports as "[test] ring3 thread start record ok" (syscall.c).
 *
 * Regression: 2026-11-22-native-thread-bootstrap-sp -- the thread trampoline
 * read the start record from the stack pointer AFTER its own C prologue had
 * moved it, so a spawned thread called a stale register as its entry (x86_64:
 * whatever RAX held, with `arg` set to the entry pointer; aarch64: the saved
 * X30) instead of the function and argument it was given. These orderings
 * passed anyway because their threads ignore `arg` and their results are
 * discarded. */
#include <stdint.h>
#include "wasmos/syscall_native.h"
#include "wasmos/thread_native.h"

enum { PROBE_STACK_SIZE = 4096 };

/* Exit status of the start-record thread when it received its argument. */
enum { START_THREAD_STATUS = 11 };
/* Exit status of the main thread when the start-record check passed; the value
 * syscall.c matches (RING3_THREAD_START_RECORD_OK). */
enum { PROBE_START_RECORD_OK = 0x5354 };

static const uint32_t g_start_token = 0x52454344u;

static uint8_t g_probe_join_stack[PROBE_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t g_probe_detach_stack[PROBE_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t g_probe_post_exit_stack[PROBE_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t g_probe_start_record_stack[PROBE_STACK_SIZE] __attribute__((aligned(16)));

static void join_helper_thread(void) {
    wasmos_sys_thread_exit(7);
}

static void detach_helper_thread(void) {
    wasmos_sys_thread_exit(0);
}

static void post_exit_helper_thread(void) {
    wasmos_sys_thread_exit(9);
}

static void start_record_thread(void* arg) {
    const uint32_t* token = (const uint32_t*)arg;
    wasmos_sys_thread_exit(
        (token == &g_start_token && *token == g_start_token) ? START_THREAD_STATUS : 0);
}

typedef struct {
    int32_t status;
} probe_cont_t;

static void probe_cont_cb(void* ctx, int32_t status) {
    probe_cont_t* state = (probe_cont_t*)ctx;
    if (state) {
        state->status = status;
    }
}

/* Flat-binary entry point, entered at offset 0 (ring3_native.ld places
 * .text.start first) with no C runtime and no return address, so it must not
 * return — it ends in wasmos_sys_thread_exit.  A spawn that fails simply skips
 * its ordering rather than aborting the probe, so a run that spawned nothing
 * still exits cleanly and reports nothing. */
__attribute__((section(".text.start"))) void _start(void) {
    probe_cont_t cont = {0};
    uint32_t join_tid = 0;
    uint32_t detach_tid = 0;
    uint32_t post_exit_tid = 0;
    uint32_t start_record_tid = 0;
    int32_t start_record_status = 0;
    int32_t exit_status = 0;

    if (wasmos_thread_spawn_cont(g_probe_join_stack,
                                 PROBE_STACK_SIZE,
                                 (wasmos_thread_entry_fn_t)join_helper_thread,
                                 0,
                                 probe_cont_cb,
                                 &cont,
                                 &join_tid) > 0) {
        (void)wasmos_thread_join_cont(join_tid, probe_cont_cb, &cont, 0);
    }

    if (wasmos_thread_spawn_cont(g_probe_detach_stack,
                                 PROBE_STACK_SIZE,
                                 (wasmos_thread_entry_fn_t)detach_helper_thread,
                                 0,
                                 probe_cont_cb,
                                 &cont,
                                 &detach_tid) > 0) {
        (void)wasmos_thread_detach_cont(detach_tid, probe_cont_cb, &cont);
        (void)wasmos_thread_join_cont(detach_tid, probe_cont_cb, &cont, 0);
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
        (void)wasmos_thread_join_cont(post_exit_tid, probe_cont_cb, &cont, 0);
    }

    if (wasmos_thread_spawn_cont(g_probe_start_record_stack,
                                 PROBE_STACK_SIZE,
                                 start_record_thread,
                                 (void*)&g_start_token,
                                 probe_cont_cb,
                                 &cont,
                                 &start_record_tid) > 0 &&
        wasmos_thread_join_cont(start_record_tid, probe_cont_cb, &cont, &start_record_status) ==
            0 &&
        start_record_status == START_THREAD_STATUS) {
        exit_status = PROBE_START_RECORD_OK;
    }

    for (uint32_t i = 0; i < 16u; ++i) {
        (void)wasmos_sys_thread_yield();
    }

    wasmos_sys_thread_exit(exit_status);
}
