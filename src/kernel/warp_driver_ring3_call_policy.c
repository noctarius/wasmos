#include "warp_driver_ring3_call_policy.h"

warp_driver_ring3_call_policy_t
warp_driver_ring3_call_policy_resolve(uint64_t r3_root)
{
    warp_driver_ring3_call_policy_t policy;

    policy.release_before_call = 0u;
    policy.clear_resched_before_call = 0u;

    if (r3_root != 0u) {
        policy.release_before_call = 1u;
        policy.clear_resched_before_call = 1u;
    }

    return policy;
}
