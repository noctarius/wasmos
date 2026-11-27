/* test_warp_exception_checkpoint.c - which WARP C++ exception checkpoint
 * warp_exception_get_checkpoint (src/kernel/warp/exception_checkpoint.c) hands
 * out as threads are switched on a CPU and moved between CPUs.
 *
 * No real concurrency is needed: the test plays the scheduler, setting each
 * fake CPU's current_thread and pointing the host cpu_local() at the CPU a
 * thread is running on, exactly as a context switch and a migration leave them. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_local.h"
#include "test_shuffle.h"
#include "thread.h"
#include "warp_exception.h"

cpu_local_t g_cpus[WASMOS_MAX_CPUS];
_Thread_local cpu_local_t* g_host_cpu_local;

static thread_t g_t1;
static thread_t g_t2;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                          \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

/* preempt_disable/enable from process.c: the host has no preemption to hold
 * off, so they only count, and the test checks they balance. */
static int g_preempt_depth;
void preempt_disable(void) {
    ++g_preempt_depth;
}
void preempt_enable(void) {
    if (g_preempt_depth > 0) {
        --g_preempt_depth;
    }
}

static void reset(void) {
    memset(g_cpus, 0, sizeof(g_cpus));
    memset(&g_t1, 0, sizeof(g_t1));
    memset(&g_t2, 0, sizeof(g_t2));
    for (uint32_t i = 0; i < WASMOS_MAX_CPUS; ++i) {
        g_cpus[i].cpu_id = i;
        g_cpus[i].self = &g_cpus[i];
    }
    g_preempt_depth = 0;
}

static void run_on(uint32_t cpu, thread_t* t) {
    g_cpus[cpu].current_thread = t;
    g_host_cpu_local = &g_cpus[cpu];
}

/* Regression: 2026-11-27-warp-checkpoint-per-cpu -- the checkpoint was one per
 * CPU, but a ring-0 WARP call (a module's start function) runs with preemption
 * enabled and can block in a host call. Another thread then armed the same
 * CPU's checkpoint over the first thread's jbuf, and a thread resumed on another
 * CPU found that CPU's checkpoint instead of its own: a WARP throw longjmped
 * into a different thread's stack frame, or panicked on a checkpoint it had
 * armed itself. */
static void test_a_switched_out_thread_keeps_its_checkpoint(void) {
    reset();
    run_on(0, &g_t1);
    warp_exception_checkpoint_t* c1 = warp_exception_get_checkpoint();
    c1->active = 1;
    c1->jbuf[0] = &g_t1;

    run_on(0, &g_t2);
    warp_exception_checkpoint_t* c2 = warp_exception_get_checkpoint();
    CHECK(c2 != c1, "a second thread on the same CPU got the first thread's checkpoint");
    c2->active = 1;
    c2->jbuf[0] = &g_t2;
    c2->active = 0;

    run_on(0, &g_t1);
    CHECK(warp_exception_get_checkpoint() == c1, "the first thread lost its checkpoint");
    CHECK(c1->active == 1 && c1->jbuf[0] == &g_t1, "the first thread's checkpoint was overwritten");
    CHECK(g_preempt_depth == 0, "preemption left disabled");
}

static void test_a_migrated_thread_finds_its_checkpoint(void) {
    reset();
    run_on(0, &g_t1);
    warp_exception_checkpoint_t* c1 = warp_exception_get_checkpoint();
    c1->active = 1;

    g_cpus[0].current_thread = NULL;
    run_on(1, &g_t1);
    CHECK(warp_exception_get_checkpoint() == c1, "a migrated thread got its new CPU's checkpoint");
    CHECK(c1->active == 1, "the migrated thread's checkpoint is no longer armed");
}

/* Before the scheduler runs a thread there is no thread to own one: each CPU has
 * its own, and they are distinct. */
static void test_without_a_thread_each_cpu_has_its_own(void) {
    reset();
    run_on(0, NULL);
    warp_exception_checkpoint_t* a = warp_exception_get_checkpoint();
    run_on(1, NULL);
    warp_exception_checkpoint_t* b = warp_exception_get_checkpoint();
    CHECK(a != NULL && b != NULL && a != b, "CPUs without a thread share a checkpoint");
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_switched_out_thread_keeps_its_checkpoint),
        WASMOS_TEST_CASE(test_a_migrated_thread_finds_its_checkpoint),
        WASMOS_TEST_CASE(test_without_a_thread_each_cpu_has_its_own),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_warp_exception_checkpoint: ok\n");
    return 0;
}
