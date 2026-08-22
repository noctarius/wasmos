#ifndef WASMOS_KERNEL_HOSTCALL_DMA_H
#define WASMOS_KERNEL_HOSTCALL_DMA_H

#include <stdint.h>

#include "wasmos_status.h"

/*
 * hostcall_dma.h — the policy decision behind the dma_* host calls, once.
 *
 * `abi/hostcalls.yaml` declares dma_map_borrow, dma_sync_borrow and
 * dma_unmap_borrow, and each runtime registers its own shim for them: wasm3's
 * m3ApiRawFunction stack marshalling in `wasm3/link_dma.c`, WARP's widened
 * scalar arguments in `warp/link_dma.cpp`. What a guest is PERMITTED to do is
 * not part of that difference, so it lives here and both shims call it.
 *
 * The split matters because it was got wrong: the checks were written once per
 * runtime, WARP's copy omitted every capability check wasm3's performed, and a
 * WARP guest could consequently program a device to DMA in a direction it was
 * never granted, over an unbounded length, at a physical address outside every
 * window its manifest declared. A shim that only marshals cannot drift that
 * way.
 *
 * Arguments are signed on purpose. Both runtimes receive a wasm i32, but WARP
 * widens it to uint32_t before the call, so a negative handle arrives above
 * INT32_MAX; taking int32_t here means the sign is re-checked in one place and
 * a malformed handle is WASMOS_ERR_DMA_INVALID under both runtimes rather than
 * whatever the wrapped value happened to select.
 *
 * The caller's context is resolved here from the current process, not passed
 * in: a guest must not be able to name the context its rights are checked
 * against.
 */

/*
 * Map [offset, offset+length) of the caller's borrow `borrow_id` for device DMA
 * in `direction_flags` (WASMOS_DMA_DIR_*). Requires POLICY_ACTION_DMA_BUFFER,
 * that the caller is the borrower, and that the direction is within both the
 * borrow's rights and the context's DMA capability.
 *
 * Returns the device DMA address as a positive i32. Failures are negative:
 *
 *   WASMOS_ERR_DMA_INVALID      a non-positive argument, or a negative offset
 *   WASMOS_ERR_DMA_DENY         no current process, no dma.buffer capability,
 *                               an unresolvable borrow, a direction the
 *                               capability does not grant, or a failed mapping
 *   WASMOS_ERR_DMA_RANGE        `length` exceeds the capability's per-mapping
 *                               byte budget, or the resulting device address
 *                               falls outside every granted window
 *   WASMOS_ERR_DMA_ADDR_TOO_LARGE  the mapping succeeded but its device address
 *                               does not fit the i32 return channel
 *
 * The RANGE and ADDR_TOO_LARGE paths tear the mapping down before returning: a
 * refused call must not leave the device holding a live window.
 */
int32_t hostcall_dma_map_borrow(int32_t borrow_id, int32_t offset, int32_t length,
                                int32_t direction_flags);

/*
 * Synchronise [offset, offset+length) of the mapping behind `borrow_id` for
 * cache coherency, where the range is relative to the mapped range and not to
 * the object. `sync_op` must be WASMOS_DMA_SYNC_TO_DEVICE, _FROM_DEVICE or
 * _BIDIR; it is validated but not forwarded, because this architecture is
 * DMA-coherent and xfer_buffer_dma_sync receives only the range, so all three
 * directions behave identically. Validating it anyway keeps a guest that passes
 * a garbage opcode from believing it asked for something.
 *
 * Requires POLICY_ACTION_DMA_BUFFER and that the caller is the borrower.
 * Returns WASMOS_ERR_NONE, WASMOS_ERR_DMA_INVALID for a bad argument, or
 * WASMOS_ERR_DMA_DENY for the capability, borrow or sync failing.
 */
int32_t hostcall_dma_sync_borrow(int32_t borrow_id, int32_t offset, int32_t length,
                                 int32_t sync_op);

/*
 * Tear down the DMA mapping established for `borrow_id`, after which the device
 * address hostcall_dma_map_borrow returned is no longer valid. Requires
 * POLICY_ACTION_DMA_BUFFER and that the caller is the borrower. Returns
 * WASMOS_ERR_NONE, WASMOS_ERR_DMA_INVALID for a non-positive borrow_id, or
 * WASMOS_ERR_DMA_DENY when the borrow or its mapping cannot be resolved or the
 * unmap fails.
 */
int32_t hostcall_dma_unmap_borrow(int32_t borrow_id);

#endif /* WASMOS_KERNEL_HOSTCALL_DMA_H */
