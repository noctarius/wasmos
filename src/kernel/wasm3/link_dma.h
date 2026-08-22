/* link_dma.h - Internal seam between link.c and link_dma.c.
 *
 * Mirrors link_ipc.h and exists for the same reason: the DMA host calls are
 * thin wrappers around a policy decision that is worth testing directly, but
 * link.c pulls in io.h (x86 port-I/O inline asm) and two dozen other kernel
 * headers, so it does not compile off x86 at all and the shims inside it are
 * unreachable to a test however simple they are.
 *
 * The decision itself is not here: all three wrappers marshal wasm i32 slots
 * and delegate to hostcall_dma.h, which both runtimes share. See that header
 * for the contract each call honours.
 */
#ifndef WASMOS_WASM3_LINK_DMA_H
#define WASMOS_WASM3_LINK_DMA_H

#include <stdint.h>

/* Whole body inside the linkage guard: these are C declarations over C headers,
 * and including them outside it from a C++ TU would give them C++ linkage. */
#ifdef __cplusplus
extern "C" {
#endif

#include "wasm3.h" /* IM3Runtime / IM3ImportContext / m3ApiRawFunction */

/*
 * The DMA host functions. m3ApiRawFunction expands to the wasm3 raw-call
 * signature: arguments and the return value are slots in _sp, with no
 * dependency on a live runtime or instance. link.c's generated link table
 * (wasmos_link_wasm3.inc) references them by name, and a host test can call
 * them directly with a stack array.
 */
m3ApiRawFunction(wasmos_dma_map_borrow);
m3ApiRawFunction(wasmos_dma_sync_borrow);
m3ApiRawFunction(wasmos_dma_unmap_borrow);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WASMOS_WASM3_LINK_DMA_H */
