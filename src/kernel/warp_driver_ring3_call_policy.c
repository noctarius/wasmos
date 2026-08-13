/* warp_driver_ring3_call_policy.c - What wasm_driver_call_entry must hand back
 * before it enters the guest.
 *
 * A ring-3 export call (r3_root != 0) does not return through the caller's
 * frame: it IRETs into the guest, may block inside a host call, and resumes via
 * the WARP_RETURN longjmp on whichever CPU services it.  Holding driver->lock or
 * the per-CPU WARP runtime binding across that is not sound, so both are dropped
 * first, and the need_resched accumulated during ring-0 startup is cleared so the
 * guest begins with a full scheduling window.
 *
 * With no ring-3 root the call is an ordinary ring-0 invocation that returns
 * normally, and the caller keeps both. */
#include "warp_driver_ring3_call_policy.h"

warp_driver_ring3_call_policy_t warp_driver_ring3_call_policy_resolve(uint64_t r3_root) {
    warp_driver_ring3_call_policy_t policy;

    policy.release_before_call = 0u;
    policy.clear_resched_before_call = 0u;

    if (r3_root != 0u) {
        policy.release_before_call = 1u;
        policy.clear_resched_before_call = 1u;
    }

    return policy;
}
