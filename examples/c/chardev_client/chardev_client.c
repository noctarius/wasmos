/* chardev_client - the minimal "talk to a driver over IPC" tutorial.
 *
 * Demonstrates the standard client sequence every WASMOS guest uses to reach a
 * service:
 *   1. read the process-manager endpoint from startup argument 0
 *      (wasmos_startup_arg(0)) — it is not passed in argv;
 *   2. create an endpoint of one's own for replies;
 *   3. resolve the service by name with wasmos_svc_lookup, retrying with
 *      sched_yield because a driver may still be registering (bounded at 2048
 *      attempts rather than waiting forever);
 *   4. send a request and block for its reply with wasmos_ipc_select_one +
 *      wasmos_ipc_message_read_last.
 *
 * The exchange itself writes one byte to the wasm chardev driver and reads it
 * back, checking the opcode, the request id and the echoed byte each time.
 *
 * Returns 0 on success and -1 at the first failed step; there is no console
 * output, so the exit status is the whole result.
 *
 * Precondition: the "chardev" driver must be running. */
#include <stdint.h>
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

int main(int argc, char** argv) {
    int32_t proc_endpoint = wasmos_startup_arg(0);
    (void)argc;
    (void)argv;

    int32_t reply_endpoint = wasmos_ipc_create_endpoint();
    if (reply_endpoint < 0) {
        return -1;
    }
    if (proc_endpoint <= 0) {
        return -1;
    }

    int32_t write_request_id = 1;
    int32_t read_request_id = 2;
    int32_t write_value = 0x41;
    int32_t chardev_endpoint = -1;

    for (int32_t spins = 0; spins < 2048; ++spins) {
        chardev_endpoint = wasmos_svc_lookup(proc_endpoint, reply_endpoint, "chardev", 100 + spins);
        if (chardev_endpoint >= 0) {
            break;
        }
        (void)wasmos_sched_yield();
    }
    if (chardev_endpoint < 0) {
        return -1;
    }

    if (wasmos_ipc_send(chardev_endpoint, reply_endpoint, WASM_CHARDEV_IPC_WRITE_REQ,
                        write_request_id, write_value, 0, 0, 0) != 0) {
        return -1;
    }

    if (wasmos_ipc_select_one(reply_endpoint) < 0) {
        return -1;
    }

    wasmos_ipc_message_t resp;
    wasmos_ipc_message_read_last(&resp);
    if (resp.type != WASM_CHARDEV_IPC_WRITE_RESP || resp.request_id != write_request_id ||
        resp.arg0 != 0 || (resp.arg1 & 0xFF) != (write_value & 0xFF)) {
        return -1;
    }

    if (wasmos_ipc_send(chardev_endpoint, reply_endpoint, WASM_CHARDEV_IPC_READ_REQ,
                        read_request_id, 0, 0, 0, 0) != 0) {
        return -1;
    }

    if (wasmos_ipc_select_one(reply_endpoint) < 0) {
        return -1;
    }

    wasmos_ipc_message_read_last(&resp);
    if (resp.type != WASM_CHARDEV_IPC_READ_RESP || resp.request_id != read_request_id ||
        resp.arg0 != 0 || (resp.arg1 & 0xFF) != (write_value & 0xFF)) {
        return -1;
    }

    return 0;
}
