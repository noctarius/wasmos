#include <assert.h>
#include <stdio.h>

#include "warp_driver_ring3_call_policy.h"

static void
test_kernel_mode_startup_keeps_lock_held(void)
{
    warp_driver_ring3_call_policy_t policy =
        warp_driver_ring3_call_policy_resolve(0u);

    assert(policy.release_before_call == 0u);
    assert(policy.clear_resched_before_call == 0u);
}

static void
test_user_mode_entry_releases_before_iret(void)
{
    warp_driver_ring3_call_policy_t policy =
        warp_driver_ring3_call_policy_resolve(0x12345000u);

    assert(policy.release_before_call == 1u);
    assert(policy.clear_resched_before_call == 1u);
}

int
main(void)
{
    test_kernel_mode_startup_keeps_lock_held();
    test_user_mode_entry_releases_before_iret();
    printf("test_warp_driver_ring3_call_policy: ok\n");
    return 0;
}
