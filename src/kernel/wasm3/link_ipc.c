/* link_ipc.c - wasm3 host functions for the kernel IPC layer.
 *
 * Split out of link.c so the shims are reachable from a host test; see
 * link_ipc.h for why that split exists. Behaviour is unchanged by the move.
 *
 * These are thin: they validate the guest's arguments, translate handles, and
 * call into ipc.c. The m3ApiRawFunction ABI is pure stack marshalling -- args
 * and the return value are slots in _sp -- so nothing here needs a live wasm3
 * runtime, which is what makes the host test possible.
 */
#include "ipc.h"
#include "process.h"
#include "thread.h"
#include "wasmos_driver_abi.h"
#include "wasm3/link_ipc.h"
#include "wasm3/shim.h"
#include "sched.h"

#include <stdint.h>
#include <string.h>

m3ApiRawFunction(wasmos_ipc_create_endpoint) {
    m3ApiReturnType(int32_t) uint32_t context_id = 0;
    uint32_t endpoint = IPC_ENDPOINT_NONE;

    preempt_safepoint();
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(IPC_ERR_NOENT);
    }
    int create_rc = ipc_endpoint_create(context_id, &endpoint);
    if (create_rc != IPC_OK) {
        m3ApiReturn(create_rc);
    }
    preempt_safepoint();
    m3ApiReturn((int32_t)endpoint);
}

m3ApiRawFunction(wasmos_ipc_endpoint_owner) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, endpoint) uint32_t owner_context_id = 0;

    preempt_safepoint();
    if (endpoint < 0) {
        m3ApiReturn(IPC_ERR_INVALID);
    }
    int owner_rc = ipc_endpoint_owner((uint32_t)endpoint, &owner_context_id);
    if (owner_rc != IPC_OK) {
        m3ApiReturn(owner_rc);
    }
    if (owner_context_id == 0) {
        m3ApiReturn(IPC_ERR_NOENT); /* kernel-owned reads as "not yours to see" */
    }
    preempt_safepoint();
    m3ApiReturn((int32_t)owner_context_id);
}

m3ApiRawFunction(wasmos_ipc_send) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, destination_endpoint)
        m3ApiGetArg(int32_t, source_endpoint) m3ApiGetArg(int32_t, type)
            m3ApiGetArg(int32_t, request_id) m3ApiGetArg(int32_t, arg0) m3ApiGetArg(int32_t, arg1)
                m3ApiGetArg(int32_t, arg2) m3ApiGetArg(int32_t, arg3) uint32_t context_id = 0;
    ipc_message_t req;

    preempt_safepoint();
    if (destination_endpoint < 0 || source_endpoint < 0) {
        m3ApiReturn(IPC_ERR_INVALID);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(IPC_ERR_NOENT);
    }

    req.type = (uint32_t)type;
    req.source = (uint32_t)source_endpoint;
    req.destination = (uint32_t)destination_endpoint;
    req.request_id = (uint32_t)request_id;
    req.arg0 = (uint32_t)arg0;
    req.arg1 = (uint32_t)arg1;
    req.arg2 = (uint32_t)arg2;
    req.arg3 = (uint32_t)arg3;

    int rc = ipc_send_from(context_id, (uint32_t)destination_endpoint, &req);
    preempt_safepoint();
    m3ApiReturn(rc);
}

m3ApiRawFunction(wasmos_ipc_select_one) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, endpoint) uint32_t context_id = 0;
    uint32_t pid = process_current_pid();
    wasm_ipc_last_slot_t* slot;
    int rc;
    process_t* process;

    if (endpoint < 0) {
        m3ApiReturn(IPC_ERR_INVALID);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(IPC_ERR_NOENT);
    }

    slot = wasm_ipc_slot_for_pid(pid);
    if (!slot) {
        m3ApiReturn(IPC_ERR_NOENT);
    }

    process = process_get(pid);
    if (!process) {
        m3ApiReturn(IPC_ERR_NOENT);
    }
    process->in_hostcall = 1;

    preempt_safepoint();
    for (;;) {
        process->block_reason = PROCESS_BLOCK_IPC;
        /* Preserve the legacy sync-spawn contract for WASM children: the
         * first blocking IPC wait marks the process ready unless it requires
         * an explicit PROC_IPC_NOTIFY_READY handshake. */
        if (!process->ready && !process->require_explicit_ready) {
            process->ready = 1;
        }
        /* Use the blocking variant: sleeps in sched_event_wait until a message
         * arrives, then dequeues and returns IPC_OK.  Returns IPC_EMPTY only
         * on a spurious wake, in which case we retry immediately. */
        rc = ipc_recv_blocking_for(context_id, (uint32_t)endpoint, &slot->message);
        if (rc == IPC_EMPTY) {
            /* Spurious wake — re-block immediately, but honour a pending
             * reschedule first so the timer tick doesn't go unserviced. */
            preempt_safepoint();
            continue;
        }
        if (rc != IPC_OK) {
            process->block_reason = PROCESS_BLOCK_NONE;
            process->in_hostcall = 0;
            m3ApiReturn(rc);
        }
        process->block_reason = PROCESS_BLOCK_NONE;
        process->in_hostcall = 0;
        slot->valid = 1;
        wasm_fs_peer_slot_t* peer = wasm_fs_peer_slot_for_pid(pid);
        if (peer && slot->message.type >= FS_IPC_OPEN_REQ &&
            slot->message.type <= FS_IPC_READ_APP_REQ) {
            uint32_t owner_context = 0;
            int owner_rc = ipc_endpoint_owner(slot->message.source, &owner_context);
            if (owner_rc == IPC_OK && owner_context != 0) {
                peer->valid = 1;
                peer->peer_context_id = owner_context;
            } else {
                peer->valid = 0;
                peer->peer_context_id = 0;
            }
        }
        preempt_safepoint();
        m3ApiReturn(1);
    }
}

m3ApiRawFunction(wasmos_ipc_drain) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, endpoint) uint32_t context_id = 0;
    uint32_t pid = process_current_pid();
    wasm_ipc_last_slot_t* slot;
    int rc;

    if (endpoint < 0) {
        m3ApiReturn(IPC_ERR_INVALID);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(IPC_ERR_NOENT);
    }

    slot = wasm_ipc_slot_for_pid(pid);
    if (!slot) {
        m3ApiReturn(IPC_ERR_NOENT);
    }

    preempt_safepoint();
    rc = ipc_recv_for(context_id, (uint32_t)endpoint, &slot->message);
    if (rc == IPC_EMPTY) {
        m3ApiReturn(0); /* no message — return without blocking */
    }
    if (rc != IPC_OK) {
        m3ApiReturn(rc);
    }
    slot->valid = 1;
    preempt_safepoint();
    m3ApiReturn(1);
}

m3ApiRawFunction(wasmos_sys_select_create) {
    m3ApiReturnType(int32_t) uint32_t context_id = 0;
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(IPC_ERR_NOENT);
    }
    uint32_t select_id = 0;
    int rc = ipc_select_create(context_id, &select_id);
    if (rc != IPC_OK) {
        m3ApiReturn(rc);
    }
    m3ApiReturn((int32_t)select_id);
}

m3ApiRawFunction(wasmos_sys_select_add) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, select_id) m3ApiGetArg(int32_t, endpoint_id)
        uint32_t context_id = 0;
    if (select_id <= 0 || endpoint_id < 0) {
        m3ApiReturn(IPC_ERR_INVALID);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(IPC_ERR_NOENT);
    }
    int rc = ipc_select_add((uint32_t)select_id, (uint32_t)endpoint_id, context_id);
    m3ApiReturn(rc == IPC_OK ? 0 : rc);
}

m3ApiRawFunction(wasmos_sys_select_wait) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, select_id) uint32_t context_id = 0;
    if (select_id <= 0) {
        m3ApiReturn(IPC_ERR_INVALID);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(IPC_ERR_NOENT);
    }
    uint32_t ready_ep = IPC_ENDPOINT_NONE;
    for (;;) {
        int rc = ipc_select_wait((uint32_t)select_id, context_id, &ready_ep, 0);
        if (rc == IPC_OK) {
            m3ApiReturn((int32_t)ready_ep);
        }
        if (rc == IPC_EMPTY) {
            /* Spurious wake — re-block, but honour pending reschedule first. */
            preempt_safepoint();
            continue;
        }
        m3ApiReturn(rc);
    }
}

/* Timed select wait: block until a watched endpoint is ready OR timeout_ms
 * elapses. Returns the ready endpoint id (>= 0), IPC_ERR_TIMEOUT when the
 * window elapsed without anything becoming ready (poll and retry), or the
 * transport code on error. Does NOT loop on IPC_EMPTY. */
m3ApiRawFunction(wasmos_sys_select_wait_timeout) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, select_id) m3ApiGetArg(int32_t, timeout_ms)
        uint32_t context_id = 0;
    if (select_id <= 0) {
        m3ApiReturn(IPC_ERR_INVALID);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(IPC_ERR_NOENT);
    }
    uint32_t ready_ep = IPC_ENDPOINT_NONE;
    int rc = ipc_select_wait((uint32_t)select_id, context_id, &ready_ep,
                             (uint32_t)(timeout_ms < 0 ? 0 : timeout_ms));
    if (rc == IPC_OK) {
        m3ApiReturn((int32_t)ready_ep);
    }
    if (rc == IPC_EMPTY) {
        m3ApiReturn(IPC_ERR_TIMEOUT); /* window elapsed, or a spurious wake */
    }
    m3ApiReturn(rc);
}

m3ApiRawFunction(wasmos_sys_select_destroy) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, select_id) uint32_t context_id = 0;
    if (select_id <= 0) {
        m3ApiReturn(IPC_ERR_INVALID);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(IPC_ERR_NOENT);
    }
    ipc_select_destroy((uint32_t)select_id, context_id);
    m3ApiReturn(0);
}

m3ApiRawFunction(wasmos_ipc_notify) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, endpoint) uint32_t context_id = 0;

    preempt_safepoint();
    if (endpoint < 0) {
        m3ApiReturn(IPC_ERR_INVALID);
    }
    if (current_process_context(&context_id) != 0) {
        m3ApiReturn(IPC_ERR_NOENT);
    }
    int rc = ipc_notify_from(context_id, (uint32_t)endpoint);
    rc = (rc == IPC_OK) ? 0 : rc;
    preempt_safepoint();
    m3ApiReturn(rc);
}

m3ApiRawFunction(wasmos_ipc_last_field) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int32_t, field) uint32_t pid = process_current_pid();
    wasm_ipc_last_slot_t* slot = wasm_ipc_slot_for_pid(pid);

    if (!slot || !slot->valid) {
        /* No message has been received on this process yet. A field value can
         * itself be -1, so this stays a distinct code rather than folding into
         * the value space. */
        m3ApiReturn(IPC_ERR_NOENT);
    }

    switch ((uint32_t)field) {
    case WASMOS_IPC_FIELD_TYPE:
        m3ApiReturn((int32_t)slot->message.type);
    case WASMOS_IPC_FIELD_REQUEST_ID:
        m3ApiReturn((int32_t)slot->message.request_id);
    case WASMOS_IPC_FIELD_ARG0:
        m3ApiReturn((int32_t)slot->message.arg0);
    case WASMOS_IPC_FIELD_ARG1:
        m3ApiReturn((int32_t)slot->message.arg1);
    case WASMOS_IPC_FIELD_SOURCE:
        m3ApiReturn((int32_t)slot->message.source);
    case WASMOS_IPC_FIELD_DESTINATION:
        m3ApiReturn((int32_t)slot->message.destination);
    case WASMOS_IPC_FIELD_ARG2:
        m3ApiReturn((int32_t)slot->message.arg2);
    case WASMOS_IPC_FIELD_ARG3:
        m3ApiReturn((int32_t)slot->message.arg3);
    default:
        m3ApiReturn(IPC_ERR_INVALID); /* not a field id */
    }
}
