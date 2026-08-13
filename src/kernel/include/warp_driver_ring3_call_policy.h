#ifndef WASMOS_WARP_DRIVER_RING3_CALL_POLICY_H
#define WASMOS_WARP_DRIVER_RING3_CALL_POLICY_H

#include <stdint.h>

/* What wasm_driver_call_entry must hand back before it enters a WARP guest.
 *
 * A ring-3 export call does not return through the caller's frame: it IRETs
 * into the guest, may block inside a host call, and resumes via the WARP_RETURN
 * longjmp on whichever CPU services it. Holding driver->lock or the per-CPU
 * WARP runtime binding across that is not sound, so both are dropped first, and
 * the need_resched accumulated during ring-0 startup is cleared so the guest
 * begins with a full scheduling window. */
typedef struct {
    uint8_t release_before_call;       /* drop driver->lock and the runtime binding */
    uint8_t clear_resched_before_call; /* clear the pending reschedule flag */
} warp_driver_ring3_call_policy_t;

/* Both fields are set for a nonzero r3_root (a ring-3 call) and clear for zero
 * (an ordinary ring-0 invocation that returns normally, where the caller keeps
 * the lock and the binding). Pure: reads no state and touches nothing. */
warp_driver_ring3_call_policy_t warp_driver_ring3_call_policy_resolve(uint64_t r3_root);

#endif
