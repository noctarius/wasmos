#ifndef WASMOS_WARP_DRIVER_RING3_CALL_POLICY_H
#define WASMOS_WARP_DRIVER_RING3_CALL_POLICY_H

#include <stdint.h>

typedef struct {
    uint8_t release_before_call;
    uint8_t clear_resched_before_call;
} warp_driver_ring3_call_policy_t;

warp_driver_ring3_call_policy_t
warp_driver_ring3_call_policy_resolve(uint64_t r3_root);

#endif
