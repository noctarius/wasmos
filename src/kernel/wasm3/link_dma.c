/* link_dma.c - wasm3 host functions for device DMA over transfer-buffer borrows.
 *
 * Split out of link.c so the shims are reachable from a host test; see
 * link_dma.h for why that split exists.
 *
 * These are pure marshalling. m3ApiRawFunction's ABI is stack slots -- arguments
 * and the return value live in _sp -- so each function's whole job is to widen
 * the guest's i32s and hand them to hostcall_dma.c, which both runtimes share.
 * Nothing here needs a live wasm3 runtime, which is what makes the host test
 * possible; nothing here decides anything, which is what keeps the two runtimes
 * from drifting apart. Contracts are in hostcall_dma.h.
 */
#include "hostcall_dma.h"
#include "wasm3/link_dma.h"

#include <stdint.h>

m3ApiRawFunction(wasmos_dma_map_borrow) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, borrow_id) m3ApiGetArg(int32_t, offset)
        m3ApiGetArg(int32_t, length) m3ApiGetArg(int32_t, direction_flags)
            m3ApiReturn(hostcall_dma_map_borrow(borrow_id, offset, length, direction_flags));
}

m3ApiRawFunction(wasmos_dma_sync_borrow) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, borrow_id) m3ApiGetArg(int32_t, offset)
        m3ApiGetArg(int32_t, length) m3ApiGetArg(int32_t, sync_op)
            m3ApiReturn(hostcall_dma_sync_borrow(borrow_id, offset, length, sync_op));
}

m3ApiRawFunction(wasmos_dma_unmap_borrow) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, borrow_id)
        m3ApiReturn(hostcall_dma_unmap_borrow(borrow_id));
}
