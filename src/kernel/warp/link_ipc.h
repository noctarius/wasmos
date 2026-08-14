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

/* Last message a process received, backing the ipc_last_field accessor. `valid` is 0
 * until the first successful receive on that pid and stays 1 afterwards; `message` is
 * overwritten in place by every subsequent receive, so a guest must read the fields it
 * wants before its next ipc_select_one / ipc_drain. Mirrors wasm3's
 * wasm_ipc_last_slot_t. */
struct WarpIpcLastSlot {
    uint32_t pid;
    uint8_t valid;
    ipc_message_t message;
};

/* Context of the peer that sent the in-flight FS request, captured from the message's
 * source endpoint so an FS reply can be attributed without trusting the guest. Written
 * only for message types in [FS_IPC_OPEN_REQ, FS_IPC_READ_APP_REQ]; `valid` is cleared
 * when that endpoint has no resolvable owner. Mirrors wasm3's wasm_fs_peer_slot_t. */
struct WarpFsPeerSlot {
    uint32_t pid;
    uint8_t valid;
    uint32_t peer_context_id;
};

/* Owned by link.cpp: per-pid slots keyed in a growable hashmap, created on
 * first use and removed on process exit.
 *
 * Return the calling process's slot, allocating and zeroing one on first use with
 * `pid` stamped into it; nullptr if `pid` is 0 or the table cannot allocate. The
 * returned pointer is borrowed and stays valid until warp_release_pid drops the pid
 * (hashmap rehash relinks nodes rather than moving them, so growth does not
 * invalidate it). */
WarpIpcLastSlot* warp_ipc_slot_for_pid(uint32_t pid);
WarpFsPeerSlot* warp_fs_peer_slot_for_pid(uint32_t pid);

/* Owned by link.cpp. 0 and *out set, or -1 if there is no current process. */
int warp_current_context_id(uint32_t* out);

/* IPC receive tracing, off by default: returns non-zero when receives on `proc` should
 * be logged. This build has tracing compiled out, so it returns 0 for every process
 * including a null one, and the trace blocks in link_ipc.cpp are dead. */
uint8_t warp_dbg_ipc_trace_process(process_t* proc);

/* The IPC host calls. The trailing void* is WARP's per-call context, unused by
 * every one of these; a host test calls them directly with nullptr.
 *
 * Shared conventions. Every handle argument arrives as a wasm i32 widened to
 * uint32_t, so a negative handle shows up above INT32_MAX; each call re-checks the
 * sign before use, and a malformed handle is IPC_ERR_INVALID rather than whatever the
 * transport would have said about the wrapped value. Results are the transport axis
 * of ipc.h returned as uint32_t: a success value is small and non-negative when read
 * back as i32, and a failure is a negative IPC_ERR_* code. None of these touch guest
 * linear memory — every argument and result is a scalar, so no bounds or commit state
 * is involved. The kernel resolves the caller's context itself rather than accepting a
 * caller-supplied one; where a handle names something the caller must own, ownership is
 * checked against that resolved context. */

/* Create an endpoint owned by the calling context. Returns the new endpoint id, or
 * IPC_ERR_NOENT with no current process / the transport's own code. */
uint32_t warp_ipc_create_endpoint(void* ctx_);
/* Context id that owns `endpoint`. Returns the owner, IPC_ERR_INVALID for a negative
 * handle, or IPC_ERR_NOENT for an unknown endpoint AND for a kernel-owned one — a
 * kernel endpoint reads as "not yours to see" rather than exposing owner 0. */
uint32_t warp_ipc_endpoint_owner(uint32_t endpoint, void* ctx_);
/* Send a message to `dest`, declaring `src` as the reply endpoint and carrying `type`,
 * `req_id` and the four payload words verbatim. `src` is not taken on trust: the
 * transport rejects it with IPC_ERR_PERM unless the calling context owns that endpoint,
 * so a receiver may treat the source as authentic. Returns 0 (IPC_OK) on success,
 * IPC_ERR_INVALID if `dest` or `src` is negative, IPC_ERR_NOENT with no current process,
 * otherwise the transport's code (IPC_ERR_PERM, IPC_ERR_FULL on a full queue). */
uint32_t warp_ipc_send(uint32_t dest, uint32_t src, uint32_t type, uint32_t req_id, uint32_t arg0,
                       uint32_t arg1, uint32_t arg2, uint32_t arg3, void* ctx_);
/* Block until a message arrives on `endpoint` and store it in the caller's
 * last-message slot, where warp_ipc_last_field then reads it. Parks the caller in the
 * scheduler (PROCESS_BLOCK_IPC) and marks it ready on the first wait unless it
 * requires an explicit ready handshake; a spurious wake yields and re-blocks rather
 * than returning. Returns 1 on success, IPC_ERR_INVALID for a negative handle,
 * IPC_ERR_NOENT with no current process or slot, otherwise the transport's code. */
uint32_t warp_ipc_select_one(uint32_t endpoint, void* ctx_);
/* Non-blocking counterpart of warp_ipc_select_one: dequeue at most one message.
 * Returns 1 when one was stored, 0 when the queue was empty, IPC_ERR_INVALID for a
 * negative handle, IPC_ERR_NOENT with no current process or slot, otherwise the
 * transport's code. Like select_one it also records the FS peer context for an FS
 * request type — the wasm3 counterpart does not. */
uint32_t warp_ipc_drain(uint32_t endpoint, void* ctx_);
/* Send a contentless notification to `endpoint`. Returns 0 (IPC_OK) on success,
 * IPC_ERR_INVALID for a negative handle, IPC_ERR_NOENT with no current process. */
uint32_t warp_ipc_notify(uint32_t endpoint, void* ctx_);
/* Read one field of the caller's last received message: 0=type, 1=request_id, 2=arg0,
 * 3=arg1, 4=source, 5=destination, 6=arg2, 7=arg3. Returns the raw field value, which
 * may itself be any 32-bit pattern including -1 — which is why "nothing received yet"
 * is IPC_ERR_NOENT and an out-of-range `field` is IPC_ERR_INVALID rather than a
 * sentinel in the value space. */
uint32_t warp_ipc_last_field(uint32_t field, void* ctx_);
/* Create a select set owned by the calling context. Returns the select-set id, or
 * IPC_ERR_NOENT with no current process / the transport's own code. */
uint32_t warp_ipc_select_create(void* ctx_);
/* Add `ep_id` to select set `sel_id`. Returns 0 on success, IPC_ERR_INVALID when
 * `sel_id` is not positive or `ep_id` is negative, IPC_ERR_NOENT with no current
 * process, otherwise the transport's code. */
uint32_t warp_ipc_select_add(uint32_t sel_id, uint32_t ep_id, void* ctx_);
/* Block until any endpoint in `sel_id` is ready and return that endpoint id. Retries
 * internally on a spurious wake, so it does not return until something is ready or the
 * wait fails. Returns IPC_ERR_INVALID when `sel_id` is not positive, IPC_ERR_NOENT
 * with no current process, otherwise the transport's code. */
uint32_t warp_ipc_select_wait(uint32_t sel_id, void* ctx_);
/* Bounded warp_ipc_select_wait: waits at most `timeout_ms` milliseconds and returns
 * IPC_ERR_TIMEOUT when the window elapses. It does NOT retry on a spurious wake, so a
 * caller that must distinguish the two has to poll; that is the price of a deadline
 * that reliably returns control. */
uint32_t warp_ipc_select_wait_timeout(uint32_t sel_id, uint32_t timeout_ms, void* ctx_);
/* Destroy select set `sel_id` if the calling context owns it. Returns 0 whether or not
 * a set was destroyed, or IPC_ERR_INVALID / IPC_ERR_NOENT for a non-positive id or no
 * current process. */
uint32_t warp_ipc_select_destroy(uint32_t sel_id, void* ctx_);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WASMOS_WARP_LINK_IPC_H */
