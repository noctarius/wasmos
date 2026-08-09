/* link_ipc.h - Internal seam between link.cpp and link_ipc.cpp.
 *
 * Mirrors wasm3/link_ipc.h, and exists for the same reason: the IPC host calls
 * are thin argument-validating wrappers worth testing directly, but link.cpp
 * carries the whole WARP engine and cannot be built for a host test. The per-pid
 * side tables stay in link.cpp, which owns their lifecycle, and are reached from
 * here through the accessors below.
 */
#ifndef WASMOS_WARP_LINK_IPC_H
#define WASMOS_WARP_LINK_IPC_H

#include <stdint.h>

/* The kernel headers below are C, and so is everything declared here, so the
 * whole body sits inside the linkage guard: pulling process.h in outside it
 * from a C++ TU would give its declarations C++ linkage and clash with the
 * same header included as C elsewhere. */
#ifdef __cplusplus
extern "C" {
#endif

#include "ipc.h"
#include "process.h"

struct WarpIpcLastSlot {
    uint32_t pid;
    uint8_t valid;
    ipc_message_t message;
};

struct WarpFsPeerSlot {
    uint32_t pid;
    uint8_t valid;
    uint32_t peer_context_id;
};

/* Owned by link.cpp: per-pid slots keyed in a growable hashmap, created on
 * first use and removed on process exit. */
WarpIpcLastSlot* warp_ipc_slot_for_pid(uint32_t pid);
WarpFsPeerSlot* warp_fs_peer_slot_for_pid(uint32_t pid);

/* Owned by link.cpp. 0 and *out set, or -1 if there is no current process. */
int warp_current_context_id(uint32_t* out);

/* IPC receive tracing, off by default. */
uint8_t warp_dbg_ipc_trace_process(process_t* proc);

/* The IPC host calls. The trailing void* is WARP's per-call context, unused by
 * every one of these; a host test calls them directly with nullptr. */
uint32_t warp_ipc_create_endpoint(void* ctx_);
uint32_t warp_ipc_endpoint_owner(uint32_t endpoint, void* ctx_);
uint32_t warp_ipc_send(uint32_t dest, uint32_t src, uint32_t type, uint32_t req_id, uint32_t arg0,
                       uint32_t arg1, uint32_t arg2, uint32_t arg3, void* ctx_);
uint32_t warp_ipc_select_one(uint32_t endpoint, void* ctx_);
uint32_t warp_ipc_drain(uint32_t endpoint, void* ctx_);
uint32_t warp_ipc_notify(uint32_t endpoint, void* ctx_);
uint32_t warp_ipc_last_field(uint32_t field, void* ctx_);
uint32_t warp_ipc_select_create(void* ctx_);
uint32_t warp_ipc_select_add(uint32_t sel_id, uint32_t ep_id, void* ctx_);
uint32_t warp_ipc_select_wait(uint32_t sel_id, void* ctx_);
uint32_t warp_ipc_select_wait_timeout(uint32_t sel_id, uint32_t timeout_ms, void* ctx_);
uint32_t warp_ipc_select_destroy(uint32_t sel_id, void* ctx_);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WASMOS_WARP_LINK_IPC_H */
