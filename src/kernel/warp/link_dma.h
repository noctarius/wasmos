/* link_dma.h - Internal seam between link.cpp and link_dma.cpp.
 *
 * Mirrors wasm3/link_dma.h, and exists for the same reason: the DMA host calls
 * are thin wrappers around a policy decision that is worth testing directly,
 * but link.cpp carries the whole WARP engine and cannot be built for a host
 * test.
 *
 * The decision itself is not here: all three wrappers marshal WARP's widened
 * i32 arguments and delegate to hostcall_dma.h, which both runtimes share. See
 * that header for the contract each call honours.
 */
#ifndef WASMOS_WARP_LINK_DMA_H
#define WASMOS_WARP_LINK_DMA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The DMA host calls, in the shape WARP's symbol table expects
 * (wasmos_symbols_warp.inc references them by name). The trailing void* is
 * WARP's per-call context, unused by all three because every argument and
 * result is a scalar and none of them touches guest linear memory; a host test
 * calls them directly with nullptr.
 *
 * Each handle argument arrives as a wasm i32 widened to uint32_t, so a negative
 * handle shows up above INT32_MAX; the shared decision re-narrows to int32_t
 * before validating, which is why a malformed handle is WASMOS_ERR_DMA_INVALID
 * rather than whatever the wrapped value would have meant. Results are returned
 * as uint32_t but read back by the guest as i32: a success is a non-negative
 * value and a failure is a negative WASMOS_ERR_DMA_* code. */

/* Map [offset, offset+length) of the caller's borrow for device DMA in
 * `flags`. Returns the device DMA address, or a negative WASMOS_ERR_DMA_*. */
uint32_t warp_dma_map_borrow(uint32_t borrow_id, uint32_t offset, uint32_t length, uint32_t flags,
                             void* ctx_);
/* Make the CPU's and the device's view of [offset, offset+length) within the
 * borrow's mapping coherent. Returns 0, or a negative WASMOS_ERR_DMA_*. */
uint32_t warp_dma_sync_borrow(uint32_t borrow_id, uint32_t offset, uint32_t length, uint32_t op,
                              void* ctx_);
/* Tear down the borrow's DMA mapping, after which the device address returned
 * by warp_dma_map_borrow is no longer valid. Returns 0, or a negative
 * WASMOS_ERR_DMA_*. */
uint32_t warp_dma_unmap_borrow(uint32_t borrow_id, void* ctx_);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WASMOS_WARP_LINK_DMA_H */
