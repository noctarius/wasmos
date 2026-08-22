/* link_dma.cpp - WARP host functions for device DMA over transfer-buffer borrows.
 *
 * Split out of link.cpp so the shims are reachable from a host test; see
 * link_dma.h for why that split exists.
 *
 * These are pure marshalling. WARP hands each wasm i32 over widened to
 * uint32_t, so the whole job here is to narrow it back and call
 * hostcall_dma.c, which both runtimes share: the sign of a handle, the
 * capability, the direction, the byte budget and the device-address window are
 * all decided there. Nothing here decides anything, which is what keeps the two
 * runtimes from drifting apart. Contracts are in hostcall_dma.h.
 */
#include "warp/link_dma.h"

extern "C" {
#include "hostcall_dma.h"
}

uint32_t warp_dma_map_borrow(uint32_t borrow_id, uint32_t offset, uint32_t length, uint32_t flags,
                             void* ctx_) {
    (void)ctx_;
    return (uint32_t)hostcall_dma_map_borrow(
        (int32_t)borrow_id, (int32_t)offset, (int32_t)length, (int32_t)flags);
}

uint32_t warp_dma_sync_borrow(uint32_t borrow_id, uint32_t offset, uint32_t length, uint32_t op,
                              void* ctx_) {
    (void)ctx_;
    return (uint32_t)hostcall_dma_sync_borrow(
        (int32_t)borrow_id, (int32_t)offset, (int32_t)length, (int32_t)op);
}

uint32_t warp_dma_unmap_borrow(uint32_t borrow_id, void* ctx_) {
    (void)ctx_;
    return (uint32_t)hostcall_dma_unmap_borrow((int32_t)borrow_id);
}
