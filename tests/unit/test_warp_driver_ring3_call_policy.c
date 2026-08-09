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
