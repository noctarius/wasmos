/* The lock/resched decision a WARP driver export call makes
 * (src/kernel/warp_driver_ring3_call_policy.c), which is split out of
 * warp_driver.cpp so it can be exercised without a driver, a runtime or a page
 * table.
 *
 * The input is the driver's ring-3 root page table. Nonzero means the call
 * IRETs to ring 3, where it must stay preemptible and may block, so the caller
 * releases the driver lock and the runtime binding before the call and clears
 * the need_resched that accumulated during ring-0 startup. Zero means there is
 * no ring-3 address space for this call: the export runs in ring 0 and the lock
 * is held across it.
 *
 * src/kernel/warp_driver_ring3_call_policy.c is the only source linked in and
 * the function it holds is pure, so nothing is stubbed and the two cases are
 * exhaustive over its input: zero and non-zero. What the caller then DOES with
 * the policy lives in warp_driver.cpp and is not covered here.
 *
 * The cases report through assert(), so the first failure aborts the process and
 * the trailing "ok" line prints only if both ran to completion. The suite is
 * compiled without -DNDEBUG, which those asserts depend on. */

#include <assert.h>
#include <stdio.h>

#include "test_shuffle.h"

#include "warp_driver_ring3_call_policy.h"

static void test_kernel_mode_startup_keeps_lock_held(void) {
    warp_driver_ring3_call_policy_t policy = warp_driver_ring3_call_policy_resolve(0u);

    assert(policy.release_before_call == 0u);
    assert(policy.clear_resched_before_call == 0u);
}

static void test_user_mode_entry_releases_before_iret(void) {
    warp_driver_ring3_call_policy_t policy = warp_driver_ring3_call_policy_resolve(0x12345000u);

    assert(policy.release_before_call == 1u);
    assert(policy.clear_resched_before_call == 1u);
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_kernel_mode_startup_keeps_lock_held),
        WASMOS_TEST_CASE(test_user_mode_entry_releases_before_iret),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_warp_driver_ring3_call_policy: ok\n");
    return 0;
}
