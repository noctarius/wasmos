/* chardev_server.c - Character device IPC server (WASM).
 * Minimal single-byte read/write service used primarily to exercise IPC
 * request/reply semantics and as a template for real character-device drivers.
 * A normal initfs driver: spawned by device-manager, registers the "chardev"
 * service with the process manager, then serves request/reply IPC. */
#include <stdint.h>
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/libsys.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

/*
 * The chardev server is intentionally tiny. It models a single-byte device-like
 * service over IPC and exists to exercise request/reply semantics, blocking
 * receive behavior, and scheduler fairness under active IPC traffic.
 */

static uint8_t g_last_byte;
static uint8_t g_has_data;
static int32_t g_service_endpoint = -1;

static void chardev_reply(int32_t reply_endpoint, int32_t type, int32_t request_id, int32_t status,
                          int32_t value) {
    if (g_service_endpoint < 0 || reply_endpoint < 0) {
        return;
    }
    (void)wasmos_ipc_reply(reply_endpoint, g_service_endpoint, type, request_id, status, value);
}

WASMOS_WASM_EXPORT int32_t initialize(int32_t proc_endpoint, int32_t arg1, int32_t arg2,
                                      int32_t arg3) {
    (void)arg1;
    (void)arg2;
    (void)arg3;

    proc_endpoint = wasmos_startup_proc_endpoint();
    g_last_byte = 0;
    g_has_data = 0;
    g_service_endpoint = wasmos_ipc_create_endpoint();
    if (g_service_endpoint < 0) {
        return -1;
    }
    if (wasmos_svc_register(proc_endpoint, g_service_endpoint, "chardev", 1) != 0) {
        return -1;
    }
    wasmos_sys_notify_ready(proc_endpoint, g_service_endpoint);

    for (;;) {
        int32_t recv_rc = wasmos_ipc_select_one(g_service_endpoint);
        if (recv_rc < 0) {
            continue;
        }
        wasmos_ipc_message_t msg;
        wasmos_ipc_message_read_last(&msg);

        /* The protocol is deliberately simple: one slot of state, one read
         * opcode, one write opcode, and a generic error path. */
        switch ((uint32_t)msg.type) {
        case WASM_CHARDEV_IPC_READ_REQ:
            if (!g_has_data) {
                chardev_reply(msg.source, WASM_CHARDEV_IPC_READ_RESP, msg.request_id,
                              WASMOS_ERR_CHARDEV_NO_DATA, 0);
            } else {
                chardev_reply(msg.source, WASM_CHARDEV_IPC_READ_RESP, msg.request_id, 0,
                              (int32_t)g_last_byte);
            }
            break;
        case WASM_CHARDEV_IPC_WRITE_REQ:
            g_last_byte = (uint8_t)(msg.arg0 & 0xFF);
            g_has_data = 1;
            chardev_reply(msg.source, WASM_CHARDEV_IPC_WRITE_RESP, msg.request_id, 0,
                          msg.arg0 & 0xFF);
            break;
        default:
            chardev_reply(msg.source, WASM_CHARDEV_IPC_ERROR_RESP, msg.request_id,
                          WASMOS_ERR_CHARDEV_UNSUPPORTED_REQUEST, msg.type);
            break;
        }
    }
    return 0;
}
