/* link_ipc.cpp - WARP host calls for the kernel IPC layer.
 *
 * A separate translation unit from link.cpp so these shims are reachable from a
 * host test; see link_ipc.h for the seam and for the per-pid slot accessors
 * link.cpp owns.
 */
#include <cstdint>

extern "C" {
#include "ipc.h"
#include "klog.h"
#include "process.h"
#include "sched.h"
#include "serial.h"
#include "thread.h"
#include "wasmos_driver_abi.h"
}

#include "warp/link_ipc.h"

uint32_t warp_ipc_create_endpoint(void* ctx_) {
    uint32_t context_id = 0, endpoint = IPC_ENDPOINT_NONE;
    (void)ctx_;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)IPC_ERR_NOENT;
    int create_rc = ipc_endpoint_create(context_id, &endpoint);
    if (create_rc != IPC_OK)
        return (uint32_t)create_rc;
    return endpoint;
}

uint32_t warp_ipc_endpoint_owner(uint32_t endpoint, void* ctx_) {
    (void)ctx_;
    uint32_t owner = 0;
    if ((int32_t)endpoint < 0)
        return (uint32_t)IPC_ERR_INVALID;
    int owner_rc = ipc_endpoint_owner(endpoint, &owner);
    if (owner_rc != IPC_OK)
        return (uint32_t)owner_rc;
    if (!owner)
        return (uint32_t)IPC_ERR_NOENT; /* kernel-owned reads as "not yours to see" */
    return owner;
}

/* Negative handles are rejected before the cast, matching wasm3. Casting first
 * turned a malformed argument into IPC_ENDPOINT_NONE and reported whatever the
 * transport then said about it -- a different code for the same guest mistake
 * depending on which runtime the app happened to run under. */
uint32_t warp_ipc_send(uint32_t dest, uint32_t src, uint32_t type, uint32_t req_id, uint32_t a0,
                       uint32_t a1, uint32_t a2, uint32_t a3, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if ((int32_t)dest < 0 || (int32_t)src < 0)
        return (uint32_t)IPC_ERR_INVALID;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)IPC_ERR_NOENT;
    ipc_message_t msg;
    msg.type = type;
    msg.source = src;
    msg.destination = dest;
    msg.request_id = req_id;
    msg.arg0 = a0;
    msg.arg1 = a1;
    msg.arg2 = a2;
    msg.arg3 = a3;
    return (uint32_t)ipc_send_from(context_id, dest, &msg);
}

uint32_t warp_ipc_select_one(uint32_t endpoint, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    uint32_t pid = process_current_pid();
    process_t* process = nullptr;

    if ((int32_t)endpoint < 0) {
        return (uint32_t)IPC_ERR_INVALID;
    }
    if (warp_current_context_id(&context_id) != 0) {
        return (uint32_t)IPC_ERR_NOENT;
    }
    WarpIpcLastSlot* slot = warp_ipc_slot_for_pid(pid);
    if (!slot) {
        return (uint32_t)IPC_ERR_NOENT;
    }
    process = process_get(pid);
    if (!process) {
        return (uint32_t)IPC_ERR_NOENT;
    }
    process->in_hostcall = 1;

    for (;;) {
        process->block_reason = PROCESS_BLOCK_IPC;
        if (!process->ready && !process->require_explicit_ready) {
            process->ready = 1;
        }

        int rc = ipc_recv_blocking_for(context_id, endpoint, &slot->message);
        if (rc == IPC_EMPTY) {
            /* Spurious wake — another waiter claimed the message first.  Yield
             * so the scheduler can clear need_resched; without this the
             * in_hostcall guard in process_preempt_from_irq prevents timer
             * preemption and triggers the watchdog stall detector. */
            process_yield(PROCESS_RUN_YIELDED);
            continue;
        }
        if (rc != IPC_OK) {
            process->block_reason = PROCESS_BLOCK_NONE;
            process->in_hostcall = 0;
            return (uint32_t)rc;
        }

        process->block_reason = PROCESS_BLOCK_NONE;
        process->in_hostcall = 0;
        slot->valid = 1;
        WarpFsPeerSlot* peer = warp_fs_peer_slot_for_pid(pid);
        if (peer && slot->message.type >= FS_IPC_OPEN_REQ &&
            slot->message.type <= FS_IPC_READ_APP_REQ) {
            uint32_t owner_context = 0;
            if (ipc_endpoint_owner(slot->message.source, &owner_context) == IPC_OK &&
                owner_context != 0) {
                peer->valid = 1;
                peer->peer_context_id = owner_context;
            } else {
                peer->valid = 0;
                peer->peer_context_id = 0;
            }
        }
        if (warp_dbg_ipc_trace_process(process)) {
            klog_write("[dbg-r3-ipc] recv pid=");
            serial_write_hex64(pid);
            klog_write(" ep=");
            serial_write_hex64(endpoint);
            klog_write(" type=");
            serial_write_hex64(slot->message.type);
            klog_write(" req=");
            serial_write_hex64(slot->message.request_id);
            klog_write(" src=");
            serial_write_hex64(slot->message.source);
            klog_write(" dst=");
            serial_write_hex64(slot->message.destination);
            klog_write(" a0=");
            serial_write_hex64(slot->message.arg0);
            klog_write(" a1=");
            serial_write_hex64(slot->message.arg1);
            klog_write(" a2=");
            serial_write_hex64(slot->message.arg2);
            klog_write(" a3=");
            serial_write_hex64(slot->message.arg3);
            klog_write("\n");
        }
        return 1;
    }
}

uint32_t warp_ipc_drain(uint32_t endpoint, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    uint32_t pid = process_current_pid();
    if ((int32_t)endpoint < 0) {
        return (uint32_t)IPC_ERR_INVALID;
    }
    if (warp_current_context_id(&context_id) != 0) {
        return (uint32_t)IPC_ERR_NOENT;
    }
    WarpIpcLastSlot* slot = warp_ipc_slot_for_pid(pid);
    if (!slot)
        return (uint32_t)IPC_ERR_NOENT;
    int rc = ipc_recv_for(context_id, endpoint, &slot->message);
    if (rc == IPC_EMPTY)
        return 0;
    if (rc != IPC_OK)
        return (uint32_t)rc;
    slot->valid = 1;
    WarpFsPeerSlot* peer = warp_fs_peer_slot_for_pid(pid);
    if (peer && slot->message.type >= FS_IPC_OPEN_REQ &&
        slot->message.type <= FS_IPC_READ_APP_REQ) {
        uint32_t owner_context = 0;
        if (ipc_endpoint_owner(slot->message.source, &owner_context) == IPC_OK &&
            owner_context != 0) {
            peer->valid = 1;
            peer->peer_context_id = owner_context;
        } else {
            peer->valid = 0;
            peer->peer_context_id = 0;
        }
    }
    return 1;
}

uint32_t warp_ipc_notify(uint32_t endpoint, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if ((int32_t)endpoint < 0)
        return (uint32_t)IPC_ERR_INVALID;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)IPC_ERR_NOENT;
    return (uint32_t)ipc_notify_from(context_id, endpoint);
}

uint32_t warp_ipc_last_field(uint32_t field, void* ctx_) {
    (void)ctx_;
    uint32_t pid = process_current_pid();
    WarpIpcLastSlot* slot = warp_ipc_slot_for_pid(pid);
    process_t* proc = process_get(pid);
    uint32_t value = 0;
    if (!slot || !slot->valid)
        /* No message received yet. A field value can itself be -1, so this is a
         * distinct code rather than a magic value in the field's own space. */
        return (uint32_t)IPC_ERR_NOENT;
    switch (field) {
    case 0:
        value = slot->message.type;
        break;
    case 1:
        value = slot->message.request_id;
        break;
    case 2:
        value = slot->message.arg0;
        break;
    case 3:
        value = slot->message.arg1;
        break;
    case 4:
        value = slot->message.source;
        break;
    case 5:
        value = slot->message.destination;
        break;
    case 6:
        value = slot->message.arg2;
        break;
    case 7:
        value = slot->message.arg3;
        break;
    default:
        return (uint32_t)IPC_ERR_INVALID; /* not a field id */
    }
    if (warp_dbg_ipc_trace_process(proc)) {
        klog_write("[dbg-r3-ipc] last pid=");
        serial_write_hex64(pid);
        klog_write(" field=");
        serial_write_hex64(field);
        klog_write(" value=");
        serial_write_hex64(value);
        klog_write("\n");
    }
    return value;
}

uint32_t warp_ipc_select_create(void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0, sel_id = 0;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)IPC_ERR_NOENT;
    int create_rc = ipc_select_create(context_id, &sel_id);
    if (create_rc != IPC_OK)
        return (uint32_t)create_rc;
    return sel_id;
}

uint32_t warp_ipc_select_add(uint32_t sel_id, uint32_t ep_id, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if ((int32_t)sel_id <= 0 || (int32_t)ep_id < 0)
        return (uint32_t)IPC_ERR_INVALID;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)IPC_ERR_NOENT;
    int rc = ipc_select_add(sel_id, ep_id, context_id);
    return (uint32_t)(rc == IPC_OK ? 0 : rc);
}

uint32_t warp_ipc_select_wait(uint32_t sel_id, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if ((int32_t)sel_id <= 0)
        return (uint32_t)IPC_ERR_INVALID;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)IPC_ERR_NOENT;
    uint32_t ready = IPC_ENDPOINT_NONE;
    for (;;) {
        int rc = ipc_select_wait(sel_id, context_id, &ready, 0);
        if (rc == IPC_OK)
            return ready;
        if (rc != IPC_EMPTY)
            return (uint32_t)rc;
    }
}

/* Timed select wait: block until a watched endpoint is ready OR timeout_ms
 * elapses. Returns the ready endpoint id (>= 0), IPC_ERR_TIMEOUT when the
 * window elapsed without anything becoming ready (poll and retry), or the
 * transport code on error. Unlike warp_ipc_select_wait this does NOT loop on
 * IPC_EMPTY, so a deadline reliably returns control. */
uint32_t warp_ipc_select_wait_timeout(uint32_t sel_id, uint32_t timeout_ms, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if ((int32_t)sel_id <= 0)
        return (uint32_t)IPC_ERR_INVALID;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)IPC_ERR_NOENT;
    uint32_t ready = IPC_ENDPOINT_NONE;
    int rc = ipc_select_wait(sel_id, context_id, &ready, timeout_ms);
    if (rc == IPC_OK)
        return ready;
    if (rc == IPC_EMPTY)
        return (uint32_t)IPC_ERR_TIMEOUT; /* window elapsed, or a spurious wake */
    return (uint32_t)rc;
}

uint32_t warp_ipc_select_destroy(uint32_t sel_id, void* ctx_) {
    (void)ctx_;
    uint32_t context_id = 0;
    if ((int32_t)sel_id <= 0)
        return (uint32_t)IPC_ERR_INVALID;
    if (warp_current_context_id(&context_id) != 0)
        return (uint32_t)IPC_ERR_NOENT;
    ipc_select_destroy(sel_id, context_id);
    return 0;
}
