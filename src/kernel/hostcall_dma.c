/* hostcall_dma.c - Policy decision behind the dma_* host calls, shared by both
 * runtimes. Contract in hostcall_dma.h. */
#include "hostcall_dma.h"

#include "capability.h"
#include "hostcall_value.h"
#include "policy.h"
#include "process.h"
#include "wasmos_driver_abi.h"
#include "xfer_buffer.h"

/* Resolve the calling process's context. 0 and *out set, or -1 when there is no
 * current process -- which is not a state a guest can reach, but is one a
 * kernel-internal caller can. */
static int dma_caller_context(uint32_t* out) {
    uint32_t pid = process_current_pid();
    process_t* proc = process_get(pid);

    if (!proc || !out) {
        return -1;
    }
    *out = proc->context_id;
    return 0;
}

/* The dma.buffer capability, which policy_authorize treats as capability-only:
 * there is no per-action narrowing beyond holding the bit. The direction, byte
 * and window limits below come from the spawn profile instead. */
static int dma_capability_allows(uint32_t context_id) {
    return policy_authorize(context_id, POLICY_ACTION_DMA_BUFFER, 0) == 0;
}

int32_t hostcall_dma_map_borrow(int32_t borrow_id, int32_t offset, int32_t length,
                                int32_t direction_flags) {
    uint32_t context_id = 0;
    uint32_t max_bytes = 0;
    xfer_buffer_borrow_t borrow;
    xfer_buffer_dma_mapping_t mapping;

    if (borrow_id <= 0 || offset < 0 || length <= 0 || direction_flags <= 0) {
        return WASMOS_ERR_DMA_INVALID;
    }
    if (dma_caller_context(&context_id) != 0 || !dma_capability_allows(context_id)) {
        return WASMOS_ERR_DMA_DENY;
    }
    /* get_borrowed enforces that the caller is the borrower, and
     * xfer_buffer_dma_map_borrow enforces direction ⊆ the borrow's rights. The
     * capability check below is the separate question of what the CONTEXT may
     * do, which a borrow's rights cannot answer: the lender does not know what
     * the borrower's manifest declared. */
    if (xfer_buffer_get_borrowed((uint32_t)borrow_id, context_id, &borrow, 0) != WASMOS_ERR_NONE) {
        return WASMOS_ERR_DMA_DENY;
    }
    if (!capability_dma_direction_allowed(context_id, (uint32_t)direction_flags)) {
        return WASMOS_ERR_DMA_DENY;
    }
    max_bytes = capability_dma_max_bytes(context_id);
    if (max_bytes == 0 || (uint32_t)length > max_bytes) {
        return WASMOS_ERR_DMA_RANGE;
    }
    if (xfer_buffer_dma_map_borrow(
            &borrow, (uint32_t)offset, (uint32_t)length, (uint32_t)direction_flags, &mapping) !=
        WASMOS_ERR_NONE) {
        return WASMOS_ERR_DMA_DENY;
    }
    /* The window check needs the mapping to exist, because what it bounds is the
     * PHYSICAL address the device will be programmed with, which only the
     * mapping knows. Both refusals from here on therefore undo it first. */
    if (!capability_dma_range_allowed(
            context_id, mapping.device_addr, (uint64_t)(uint32_t)length)) {
        (void)xfer_buffer_dma_unmap(&mapping);
        return WASMOS_ERR_DMA_RANGE;
    }
    /* Distinct from UNAVAILABLE on purpose. UNAVAILABLE means the platform had
     * no mapping slot or no backing to give (region_alloc's pfa_alloc_pages_below
     * and warp_linmem_place_phys return it); this is a mapping that succeeded and
     * produced an address the i32 return channel cannot carry. Overloading one
     * code with both reintroduces exactly the ambiguity the packed model exists
     * to remove. */
    if (hostcall_value_check(mapping.device_addr) != WASMOS_OK) {
        (void)xfer_buffer_dma_unmap(&mapping);
        return WASMOS_ERR_DMA_ADDR_TOO_LARGE;
    }
    return (int32_t)mapping.device_addr;
}

int32_t hostcall_dma_sync_borrow(int32_t borrow_id, int32_t offset, int32_t length,
                                 int32_t sync_op) {
    uint32_t context_id = 0;
    xfer_buffer_borrow_t borrow;
    xfer_buffer_dma_mapping_t mapping;

    if (borrow_id <= 0 || offset < 0 || length <= 0 ||
        (sync_op != WASMOS_DMA_SYNC_TO_DEVICE && sync_op != WASMOS_DMA_SYNC_FROM_DEVICE &&
         sync_op != WASMOS_DMA_SYNC_BIDIR)) {
        return WASMOS_ERR_DMA_INVALID;
    }
    if (dma_caller_context(&context_id) != 0 || !dma_capability_allows(context_id)) {
        return WASMOS_ERR_DMA_DENY;
    }
    if (xfer_buffer_get_borrowed((uint32_t)borrow_id, context_id, &borrow, &mapping) !=
        WASMOS_ERR_NONE) {
        return WASMOS_ERR_DMA_DENY;
    }
    if (xfer_buffer_dma_sync(&mapping, (uint32_t)offset, (uint32_t)length) != WASMOS_ERR_NONE) {
        return WASMOS_ERR_DMA_DENY;
    }
    return WASMOS_ERR_NONE;
}

int32_t hostcall_dma_unmap_borrow(int32_t borrow_id) {
    uint32_t context_id = 0;
    xfer_buffer_borrow_t borrow;
    xfer_buffer_dma_mapping_t mapping;

    if (borrow_id <= 0) {
        return WASMOS_ERR_DMA_INVALID;
    }
    if (dma_caller_context(&context_id) != 0 || !dma_capability_allows(context_id)) {
        return WASMOS_ERR_DMA_DENY;
    }
    if (xfer_buffer_get_borrowed((uint32_t)borrow_id, context_id, &borrow, &mapping) !=
        WASMOS_ERR_NONE) {
        return WASMOS_ERR_DMA_DENY;
    }
    if (xfer_buffer_dma_unmap(&mapping) != WASMOS_ERR_NONE) {
        return WASMOS_ERR_DMA_DENY;
    }
    return WASMOS_ERR_NONE;
}
